module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/owner_cancellation.hxx"
#include "details/cancellation_latch.hxx"
#include "details/peer_failure.hxx"
#include "details/resource_stream.hxx"

namespace forge::net::p2p {

namespace {

[[nodiscard]] resource_manager::stream_reservation::bind_result
bind_stream_resource(const std::shared_ptr<detail::resource_stream>& resource, const protocol_id& protocol,
                     bool dht_profile) noexcept {
   const auto protocol_result = resource->bind_protocol(protocol);
   return protocol_result == resource_manager::stream_reservation::bind_result::accepted
              ? resource->bind_service_for_protocol(protocol, dht_profile)
              : protocol_result;
}

} // namespace

boost::asio::awaitable<forge::net::p2p::stream>
node::impl::open_session_stream(const std::shared_ptr<session_state>& session, const protocol_id& protocol, bool relay,
                                detail::stream_admission_handler admitted) {
   const auto direction = resource_manager::session_direction::outbound;
   auto reservation = resources.reserve_stream(session->info.remote_peer, direction);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         auto lock = std::scoped_lock{mutex};
         ++metrics_value.backpressure_rejections;
         ++metrics_value.protocol_rejections;
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P stream limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P stream resource admission failed");
   }
   auto [guarded, resource] = detail::prepare_resource_stream(std::move(*reservation));
   auto selected = forge::net::p2p::stream{};
   auto failure = std::exception_ptr{};
   try {
      auto raw = co_await session->connection.async_open_stream();
      resource->attach(std::move(raw));
      if (admitted) {
         admitted(resource);
      }
      selected = co_await protocol_negotiation::async_select(std::move(guarded), protocol);
      const auto binding = bind_stream_resource(resource, protocol, dht_profiles.contains(protocol));
      if (binding != resource_manager::stream_reservation::bind_result::accepted) {
         if (binding == resource_manager::stream_reservation::bind_result::policy_rejected) {
            auto lock = std::scoped_lock{mutex};
            ++metrics_value.backpressure_rejections;
            ++metrics_value.protocol_rejections;
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P scoped stream limit reached");
         }
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P scoped stream binding failed at runtime");
      }
      admitted.commit();
      detail::stream_access::set_authentication(selected, session->authentication);
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure) {
      // Keep the reservation through the native terminal barrier. cancel()
      // retires resource ownership immediately and is only an abandon fallback.
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      resource->request_cancel();
      try { co_await resource->async_close(); } catch (...) {}
      std::rethrow_exception(failure);
   }
   co_return selected;
}

boost::asio::awaitable<forge::net::p2p::stream>
node::impl::open_yamux_stream(const peer_id& peer, const std::shared_ptr<forge::net::yamux::session>& yamux,
                              const protocol_id& protocol, bool relay) {
   auto reservation = resources.reserve_stream(peer, resource_manager::session_direction::outbound);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P relayed stream limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P relayed stream resource admission failed");
   }
   auto [guarded, resource] = detail::prepare_resource_stream(std::move(*reservation));
   auto raw = co_await yamux->async_open_stream();
   resource->attach(std::move(raw));
   auto selected = forge::net::p2p::stream{};
   try {
      selected = co_await protocol_negotiation::async_select(std::move(guarded), protocol);
   } catch (...) {
      resource->cancel();
      throw;
   }
   const auto binding = bind_stream_resource(resource, protocol, dht_profiles.contains(protocol));
   if (binding != resource_manager::stream_reservation::bind_result::accepted) {
      selected.cancel();
      resource->cancel();
      if (binding == resource_manager::stream_reservation::bind_result::policy_rejected) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P scoped relayed stream limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P scoped relayed stream binding failed at runtime");
   }
   co_return selected;
}

boost::asio::awaitable<node::impl::admitted_stream>
node::impl::accept_resource_stream(const peer_id& peer, forge::net::transport::stream stream,
                                   resource_manager::stream_reservation reservation) {
   auto guarded = forge::net::transport::stream{};
   auto resource = std::shared_ptr<detail::resource_stream>{};
   try {
      auto prepared = detail::prepare_resource_stream(std::move(reservation));
      guarded = std::move(prepared.first);
      resource = std::move(prepared.second);
   } catch (...) {
      stream.cancel();
      throw;
   }
   resource->attach(std::move(stream));
   auto negotiated = protocol_negotiation::negotiated_stream{};
   try {
      negotiated = co_await protocol_negotiation::async_accept(std::move(guarded), supported_protocols());
   } catch (...) {
      resource->cancel();
      throw;
   }
   const auto binding = bind_stream_resource(resource, negotiated.protocol, dht_profiles.contains(negotiated.protocol));
   if (binding != resource_manager::stream_reservation::bind_result::accepted) {
      negotiated.stream.cancel();
      resource->cancel();
      if (binding == resource_manager::stream_reservation::bind_result::policy_rejected) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P scoped inbound stream limit reached");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P scoped inbound stream binding failed at runtime");
   }
   co_return admitted_stream{
       .protocol = std::move(negotiated.protocol),
       .stream = std::move(negotiated.stream),
       .resource = std::move(resource),
   };
}

boost::asio::awaitable<forge::net::p2p::stream> node::impl::open_protocol_on_direct_session(
    const peer_id& peer, const protocol_id& protocol, std::shared_ptr<node::impl::session_state> session,
    std::chrono::milliseconds timeout, std::shared_ptr<cancellation_latch> cancellation) {
   auto deadline = operation_deadline{runtime.context(), timeout};
   auto cancellation_subscription = cancellation_latch::subscribe(
       cancellation, [stop = deadline.stopping()] noexcept { static_cast<void>(stop.request_stop()); });
   auto deadline_id = std::uint64_t{};
   auto stopping = deadline.stopping();
   auto stop_before_arm = false;
   {
      auto lock = std::scoped_lock{mutex};
      if (stopped) {
         stop_before_arm = true;
      } else {
         deadline_id = next_protocol_open_deadline_id++;
         protocol_open_deadlines.emplace(deadline_id, std::move(stopping));
      }
   }
   if (stop_before_arm) {
      static_cast<void>(stopping.request_stop());
   }
   auto release_deadline = [this, deadline_id](void*) noexcept {
      if (deadline_id == 0) {
         return;
      }
      auto lock = std::scoped_lock{mutex};
      protocol_open_deadlines.erase(deadline_id);
   };
   auto deadline_guard = std::unique_ptr<void, decltype(release_deadline)>{this, std::move(release_deadline)};
   if (deadline.stopped()) {
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
      }
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P protocol open stopped with its node");
   }
   auto stream_stop = std::make_shared<detail::worker_stop_bridge>();
   auto stream_stop_requested = std::make_shared<std::atomic_bool>(false);
   deadline.arm([stream_stop, stream_stop_requested] noexcept {
      stream_stop_requested->store(true, std::memory_order_release);
      stream_stop->request_stop();
   });
   const auto record_open_timeout = [&] {
      record_direct_session_failure(session);
   };
   record_path_attempt(path::kind::direct);
   try {
      auto selected = std::optional<forge::net::p2p::stream>{};
      co_await detail::async_run_with_owner_cancellation(
          stream_stop,
          [this, session, protocol, stream_stop, stream_stop_requested,
           &selected](boost::asio::cancellation_slot slot) -> boost::asio::awaitable<void> {
             if (stream_stop_requested->load(std::memory_order_acquire)) {
                FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled before stream admission");
             }
             selected.emplace(co_await open_session_stream(
                 session, protocol, false,
                 detail::make_owner_stream_admission(slot, stream_stop, detail::owner_stream_lifetime::negotiation)));
          });
      const auto completed = deadline.finish();
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
      }
      if (deadline.stopped()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P protocol open stopped with its node");
      }
      if (!completed) {
         throw_operation_timeout("P2P protocol open");
      }
      if (!selected) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled before operation start");
      }
      increment_opened_protocol();
      record_path_open(path::kind::direct);
      co_return std::move(*selected);
   } catch (const forge::exceptions::base& error) {
      const auto completed = deadline.finish();
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
      }
      if (deadline.timed_out() || !completed) {
         record_open_timeout();
         throw_operation_timeout("P2P protocol open");
      }
      if (deadline.stopped()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P protocol open stopped with its node");
      }
      const auto kind = p2p_code(error);
      auto node_stopped = false;
      {
         auto lock = std::scoped_lock{mutex};
         node_stopped = stopped;
      }
      if (node_stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P protocol open stopped with its node");
      }
      const auto p2p_kind = exceptions::code_of(error);
      if (p2p_kind == exceptions::code::unsupported_protocol || p2p_kind == exceptions::code::protocol_error ||
          p2p_kind == exceptions::code::codec_error) {
         throw;
      }
      if (kind == exceptions::code::canceled || kind == exceptions::code::backpressure_rejected) {
         FORGE_THROW_CODE(kind, error.what());
      }
      session->closed = true;
      forget_session(session);
      if (detail::remote_peer_attributable_failure(kind, node_stopped)) {
         record_direct_session_failure(session);
      }
      FORGE_THROW_CODE(kind, error.what());
   } catch (const boost::system::system_error& error) {
      const auto completed = deadline.finish();
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
      }
      if (deadline.timed_out() || !completed) {
         record_open_timeout();
         throw_operation_timeout("P2P protocol open");
      }
      auto node_stopped = false;
      {
         const auto lock = std::scoped_lock{mutex};
         node_stopped = stopped;
      }
      if (deadline.stopped() || node_stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P protocol open stopped with its node");
      }
      if (error.code() == boost::asio::error::operation_aborted) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, std::string{"P2P protocol open failed: "} + error.what());
   }
}

boost::asio::awaitable<node::impl::opened_direct_stream>
node::impl::open_protocol_direct_with_context(const peer_id& peer, const protocol_id& protocol,
                                              std::chrono::milliseconds timeout, std::size_t max_direct_endpoints,
                                              std::chrono::milliseconds direct_attempt_timeout,
                                              std::shared_ptr<cancellation_latch> cancellation) {
   const auto started = std::chrono::steady_clock::now();
   auto cached = session_for_path(peer, path::kind::direct);
   return open_protocol_direct_owned(shared_from_this(), std::move(cached), peer, protocol, started,
                                     timeout, max_direct_endpoints, direct_attempt_timeout,
                                     std::move(cancellation));
}

boost::asio::awaitable<node::impl::opened_direct_stream>
node::impl::open_protocol_direct_owned(std::shared_ptr<impl> self, std::shared_ptr<session_state> cached,
                                       peer_id peer, protocol_id protocol,
                                       std::chrono::steady_clock::time_point started,
                                       std::chrono::milliseconds timeout, std::size_t max_direct_endpoints,
                                       std::chrono::milliseconds direct_attempt_timeout,
                                       std::shared_ptr<cancellation_latch> cancellation) {
   auto last_kind = std::optional<exceptions::code>{};
   auto last_message = std::string{};
   // A stale cached connection gets one fresh bounded dial, not a nested
   // max_direct_endpoints-squared set of transport attempts.
   const auto acquisitions = cached ? 2U : 1U;
   for (std::size_t attempt = 0; attempt < acquisitions; ++attempt) {
      const auto remaining = remaining_timeout(started, timeout, "P2P protocol open");
      auto session = std::move(cached);
      if (!session) {
         session = co_await self->ensure_direct_session(
             peer, remaining, max_direct_endpoints, direct_attempt_timeout, cancellation);
      }
      const auto open_timeout = attempt_timeout(
          remaining_timeout(started, timeout, "P2P protocol open"),
          direct_attempt_timeout, "P2P protocol open direct attempt");
      try {
         auto selected = co_await self->open_protocol_on_direct_session(peer, protocol, session, open_timeout, cancellation);
         co_return opened_direct_stream{
             .stream = std::move(selected),
             .remote_endpoint = session->remote_endpoint,
             .direct_endpoint = session->direct_endpoint,
         };
      } catch (const forge::exceptions::base& error) {
         if (cancellation && cancellation->stop_requested()) {
            FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
         }
         const auto p2p_kind = exceptions::code_of(error);
         if (p2p_kind == exceptions::code::unsupported_protocol || p2p_kind == exceptions::code::protocol_error ||
             p2p_kind == exceptions::code::codec_error) {
            throw;
         }
         last_kind = p2p_code(error);
         last_message = error.what();
      }
   }
   if (last_kind) {
      FORGE_THROW_CODE(*last_kind, last_message);
   }
   FORGE_THROW_EXCEPTION(exceptions::peer_not_found, "P2P direct path attempts were exhausted");
}

boost::asio::awaitable<forge::net::p2p::stream>
node::impl::open_protocol_direct(const peer_id& peer, const protocol_id& protocol, std::chrono::milliseconds timeout,
                                 std::size_t max_direct_endpoints, std::chrono::milliseconds direct_attempt_timeout) {
   auto selected = co_await open_protocol_direct_with_context(peer, protocol, timeout, max_direct_endpoints,
                                                              direct_attempt_timeout, {});
   co_return std::move(selected.stream);
}

} // namespace forge::net::p2p
