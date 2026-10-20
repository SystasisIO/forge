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
#include <stop_token>
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

import :lifecycle_stop_listener;

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
                                detail::stream_admission_handler admitted, detail::stream_open_phase* phase) {
   if (phase) {
      *phase = detail::stream_open_phase::admission;
   }
   const auto direction = resource_manager::session_direction::outbound;
   auto reservation = resources.reserve_stream(session->info.remote_peer, direction);
   if (!reservation) {
      if (reservation.outcome() == resource_manager::transition_result::policy_rejected) {
         auto lock = std::scoped_lock{mutex};
         ++metrics_value.backpressure_rejections;
         ++metrics_value.protocol_rejections;
         if (phase) {
            *phase = detail::stream_open_phase::local_rejected;
         }
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P stream limit reached");
      }
      if (phase) {
         *phase = detail::stream_open_phase::local_failed;
      }
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P stream resource admission failed");
   }
   auto [guarded, resource] = detail::prepare_resource_stream(std::move(*reservation));
   auto native_owner = std::shared_ptr<forge::net::transport::detail::session_concept>{};
   auto selected = forge::net::p2p::stream{};
   auto failure = std::exception_ptr{};
   try {
      {
         const auto lock = std::scoped_lock{mutex};
         native_owner = forge::net::transport::detail::session_access::cancellation_owner(session->connection);
         if (!native_owner || !native_owner->valid()) {
            FORGE_THROW_EXCEPTION(exceptions::closed, "P2P transport session is closed");
         }
      }
      // Retirement can move the facade while the native open or its cleanup is suspended.
      // Coroutine-frame creation is still local admission, before native work starts.
      auto opening = native_owner->async_open_stream();
      if (phase) {
         *phase = detail::stream_open_phase::native_open;
      }
      auto raw = co_await std::move(opening);
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
      if (phase) {
         *phase = detail::stream_open_phase::committed;
      }
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

boost::asio::awaitable<bool>
node::impl::dispatch_registered_handler(const std::shared_ptr<session_state>& session, admitted_stream& admitted) {
   auto failure = std::exception_ptr{};
   try {
      require_private_protocol_allowed(admitted.protocol);
      // Explicit routing must not turn a circuit into a native relay owner connection.
      if (session->info.path != path::kind::direct &&
          (admitted.protocol == builtins::relay_hop || admitted.protocol == builtins::relay_stop)) {
         increment_protocol_rejected();
         FORGE_THROW_EXCEPTION(exceptions::unsupported_protocol, "P2P relay HOP/STOP requires a direct session");
      }
      auto handler = handler_for(admitted.protocol);
      if (!handler) { co_return false; }
      increment_protocol_accepted();
      co_await (*handler)(node::incoming_protocol_stream{
          .session = session_info_for(session),
          .protocol = admitted.protocol,
          .stream = std::move(admitted.stream),
      });
   } catch (...) { failure = std::current_exception(); }
   if (failure) {
      co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
      admitted.stream = {};
   }
   try { co_await detail::async_close_unescaped(admitted.resource); }
   catch (...) { if (!failure) { throw; } }
   if (failure) { std::rethrow_exception(failure); }
   co_return true;
}

boost::asio::awaitable<forge::net::p2p::stream> node::impl::open_protocol_on_session(
    const peer_id& peer, const protocol_id& protocol, std::shared_ptr<node::impl::session_state> session,
    std::chrono::milliseconds timeout, std::shared_ptr<cancellation_latch> cancellation,
    detail::stream_open_phase* phase) {
   if (session->info.remote_peer != peer) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed,
         "P2P protocol acquisition resolved to a different authenticated peer");
   }
   auto local_phase = detail::stream_open_phase::admission;
   if (!phase) { phase = &local_phase; }
   *phase = detail::stream_open_phase::admission;
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
   auto failure_recorded = false;
   const auto record_open_failure = [&] {
      if (failure_recorded) { return; }
      failure_recorded = true;
      if (session->info.path == path::kind::direct) { record_direct_session_failure(session); }
      else { record_relay_failure(); }
   };
   record_path_attempt(session->info.path);
   try {
      auto selected = std::optional<forge::net::p2p::stream>{};
      co_await detail::async_run_with_owner_cancellation(
          stream_stop,
          [this, session, protocol, stream_stop, stream_stop_requested, phase,
           &selected](boost::asio::cancellation_slot slot) -> boost::asio::awaitable<void> {
             if (stream_stop_requested->load(std::memory_order_acquire)) {
                FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled before stream admission");
             }
             selected.emplace(co_await open_session_stream(
                 session, protocol, false,
                 detail::make_owner_stream_admission(slot, stream_stop, detail::owner_stream_lifetime::negotiation),
                 phase));
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
      record_path_open(session->info.path);
      co_return std::move(*selected);
   } catch (const forge::exceptions::base& error) {
      const auto completed = deadline.finish();
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
      }
      if (deadline.timed_out() || !completed) {
         record_open_failure();
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
      if (*phase == detail::stream_open_phase::local_failed && kind == exceptions::code::internal) {
         throw;
      }
      auto accounting_failure = std::exception_ptr{};
      try {
         if (detail::remote_peer_attributable_failure(kind, node_stopped)) {
            record_open_failure();
         }
      } catch (...) {
         accounting_failure = std::current_exception();
      }
      // Local accounting pressure must not leave this failed native owner registered.
      session->closed = true;
      forget_session(session);
      if (accounting_failure) { std::rethrow_exception(accounting_failure); }
      FORGE_THROW_CODE(kind, error.what());
   } catch (const boost::system::system_error& error) {
      const auto completed = deadline.finish();
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol open canceled");
      }
      if (deadline.timed_out() || !completed) {
         record_open_failure();
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
                                              std::shared_ptr<cancellation_latch> cancellation,
                                              std::shared_ptr<const detail::open_address_hints> hints) {
   const auto started = std::chrono::steady_clock::now();
   auto cached = session_for_path(peer, path::kind::direct);
   return open_protocol_direct_owned(shared_from_this(), std::move(cached), peer, protocol, started,
                                     timeout, max_direct_endpoints, direct_attempt_timeout,
                                     std::move(cancellation), std::move(hints));
}

boost::asio::awaitable<node::impl::opened_direct_stream>
node::impl::open_protocol_direct_owned(std::shared_ptr<impl> self, std::shared_ptr<session_state> cached,
                                       peer_id peer, protocol_id protocol,
                                       std::chrono::steady_clock::time_point started,
                                       std::chrono::milliseconds timeout, std::size_t max_direct_endpoints,
                                       std::chrono::milliseconds direct_attempt_timeout,
                                       std::shared_ptr<cancellation_latch> cancellation,
                                       std::shared_ptr<const detail::open_address_hints> hints) {
   auto last_kind = std::optional<exceptions::code>{};
   auto last_message = std::string{};
   const auto check_cancel = [&] {
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol path acquisition canceled");
      }
   };
   check_cancel();
   // A stale cached connection gets one fresh bounded dial, not a nested
   // max_direct_endpoints-squared set of transport attempts.
   const auto acquisitions = cached ? 2U : 1U;
   for (std::size_t attempt = 0; attempt < acquisitions; ++attempt) {
      const auto remaining = remaining_timeout(started, timeout, "P2P protocol open");
      auto session = std::move(cached);
      if (!session) {
         session = co_await self->ensure_direct_session(
             peer, remaining, max_direct_endpoints, direct_attempt_timeout, cancellation,
             hints ? hints->direct : std::vector<forge::multiformats::multiaddr>{});
      }
      const auto open_timeout = attempt_timeout(
          remaining_timeout(started, timeout, "P2P protocol open"),
          direct_attempt_timeout, "P2P protocol open direct attempt");
      auto phase = detail::stream_open_phase::admission;
      try {
         auto selected = co_await self->open_protocol_on_session(peer, protocol, session, open_timeout, cancellation, &phase);
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
         if (phase == detail::stream_open_phase::local_failed && p2p_kind == exceptions::code::internal) {
            throw;
         }
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
                                 std::size_t max_direct_endpoints, std::chrono::milliseconds direct_attempt_timeout,
                                 std::shared_ptr<const detail::open_address_hints> hints) {
   auto selected = co_await open_protocol_direct_with_context(peer, protocol, timeout, max_direct_endpoints,
      direct_attempt_timeout, hints ? hints->cancellation : nullptr, hints);
   co_return std::move(selected.stream);
}


boost::asio::awaitable<forge::net::p2p::stream> node::impl::open_protocol_paths(std::shared_ptr<impl> self,
   peer_id peer, protocol_id protocol, node::open_options options,
   std::chrono::steady_clock::time_point started, std::shared_ptr<cancellation_latch> cancellation) {
   validate_operation_timeout(options.timeout, "P2P protocol open timeout");
   validate_operation_timeout(options.direct_attempt_timeout, "P2P direct attempt timeout");
   validate_operation_timeout(options.relay_attempt_timeout, "P2P relay attempt timeout");
   if (options.max_direct_endpoints == 0 || options.max_relay_candidates == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P path attempt limits must be positive");
   }
   self->require_private_protocol_allowed(protocol);
   if (self->private_network_enabled()) {
      if (options.relay_peer) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                               "P2P private-network protocol open does not permit a relay peer");
      }
      options.allow_relay = false;
      options.allow_hole_punch = false;
   }
   auto effective = std::move(options);
   effective.allow_relay =
       effective.allow_relay && self->options.path_policy.allow_relay && self->options.relay_policy.client_enabled;
   effective.allow_hole_punch = effective.allow_hole_punch && self->options.path_policy.allow_hole_punch;
   effective.max_direct_endpoints =
       std::min(effective.max_direct_endpoints, self->options.path_policy.max_direct_endpoints);
   effective.max_relay_candidates =
       std::min(effective.max_relay_candidates, self->options.path_policy.max_relay_candidates);
   auto hints = std::make_shared<const detail::open_address_hints>(peer, std::move(effective.address_hints),
      self->options.dns_resolution.bounds, effective.max_direct_endpoints, effective.direct_attempt_timeout, cancellation);
   if (self->private_network_enabled() && !hints->relays.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P private-network open does not permit circuit hints");
   }
   const auto check_cancel = [&] {
      if (cancellation && cancellation->stop_requested()) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P protocol path acquisition canceled");
      }
   };
   check_cancel();
   auto last_kind = std::optional<exceptions::code>{};
   auto last_message = std::string{};
   if (self->options.path_policy.allow_direct) {
      try {
         co_return co_await self->open_protocol_direct(
             peer, protocol, remaining_timeout(started, effective.timeout, "P2P protocol open"),
             effective.max_direct_endpoints, effective.direct_attempt_timeout, hints);
      } catch (const forge::exceptions::base& error) {
         const auto kind = p2p_code(error);
         last_kind = kind;
         last_message = error.what();
         auto node_stopped = false;
         if (kind == exceptions::code::closed) {
            auto lock = std::scoped_lock{self->mutex};
            node_stopped = self->stopped;
         }
         if ((kind == exceptions::code::closed && node_stopped) || kind == exceptions::code::canceled ||
             kind == exceptions::code::unsupported_protocol || kind == exceptions::code::protocol_error ||
             kind == exceptions::code::codec_error) {
            throw;
         }
         try {
            (void)remaining_timeout(started, effective.timeout, "P2P protocol open");
         } catch (const forge::exceptions::base&) {
            throw;
         }
         if (!effective.allow_relay && !(effective.allow_hole_punch && (effective.relay_peer || !hints->relays.empty()))) {
            throw;
         }
      }
   }

   auto relay_candidates = std::vector<peer_id>{};
   if (effective.relay_peer) {
      relay_candidates.push_back(*effective.relay_peer);
   } else if (effective.allow_relay || effective.allow_hole_punch) {
      for (const auto& relay : hints->relays) {
         if (relay_candidates.size() >= effective.max_relay_candidates) break;
         relay_candidates.push_back(relay.peer);
      }
      for (auto& relay : self->fresh_outbound_relay_candidates(effective.max_relay_candidates,
              self->options.relay_policy.refresh_margin)) {
         if (relay_candidates.size() >= effective.max_relay_candidates) break;
         if (std::ranges::find(relay_candidates, relay) == relay_candidates.end()) relay_candidates.push_back(std::move(relay));
      }
      if (relay_candidates.empty() && self->options.relay_policy.auto_discovery_enabled) {
         const auto remaining = remaining_timeout(started, effective.timeout, "P2P AutoRelay refresh");
         const auto refresh_timeout =
             attempt_timeout(remaining, effective.relay_attempt_timeout, "P2P AutoRelay refresh");
         try {
            auto refresh_stop = std::make_shared<detail::worker_stop_bridge>();
            auto refresh_subscription = cancellation_latch::subscribe(cancellation,
               [refresh_stop] noexcept { refresh_stop->request_stop(); });
            co_await detail::async_run_with_owner_cancellation(refresh_stop,
               [self, peer, refresh_timeout](boost::asio::cancellation_slot) -> boost::asio::awaitable<void> {
                  (void)co_await self->refresh_relay_candidates(peer, refresh_timeout);
               });
            check_cancel();
            relay_candidates = self->fresh_outbound_relay_candidates(effective.max_relay_candidates,
                                                                     self->options.relay_policy.refresh_margin);
         } catch (const forge::exceptions::base& error) {
            check_cancel();
            if (p2p_code(error) == exceptions::code::canceled) throw;
            last_kind = p2p_code(error);
            last_message = error.what();
         }
      }
   }

   if (effective.allow_hole_punch) {
      for (const auto& relay_peer : relay_candidates) {
         check_cancel();
         const auto remaining = remaining_timeout(started, effective.timeout, "P2P hole punch");
         const auto per_attempt = attempt_timeout(remaining, effective.relay_attempt_timeout, "P2P hole punch attempt");
         try {
            const auto status = co_await self->attempt_hole_punch(peer, relay_peer, per_attempt, hints);
            if (status == hole_punch::status::succeeded) {
               co_return co_await self->open_protocol_direct(
                   peer, protocol, remaining_timeout(started, effective.timeout, "P2P protocol open after hole punch"),
                   effective.max_direct_endpoints, effective.direct_attempt_timeout, hints);
            }
         } catch (const forge::exceptions::base& error) {
            check_cancel();
            if (p2p_code(error) == exceptions::code::canceled) throw;
            last_kind = p2p_code(error);
            last_message = error.what();
         }
      }
   }

   if (!effective.allow_relay) {
      if (last_kind) {
         FORGE_THROW_CODE(*last_kind, last_message);
      }
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay fallback is disabled");
   }

   if (relay_candidates.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P path manager found no reserved relay candidate");
   }
   for (const auto& relay_peer : relay_candidates) {
      check_cancel();
      const auto remaining = remaining_timeout(started, effective.timeout, "P2P protocol open");
      const auto per_attempt = attempt_timeout(remaining, effective.relay_attempt_timeout, "P2P relay path attempt");
      try {
         co_return co_await self->open_protocol_via_relay(peer, protocol, relay_peer, per_attempt, hints);
      } catch (const forge::exceptions::base& error) {
         check_cancel();
         if (p2p_code(error) == exceptions::code::canceled) throw;
         last_kind = p2p_code(error);
         last_message = error.what();
      }
   }
   if (last_kind) {
      FORGE_THROW_CODE(*last_kind, last_message);
   }
   FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P path manager exhausted relay candidates");
}

} // namespace forge::net::p2p
