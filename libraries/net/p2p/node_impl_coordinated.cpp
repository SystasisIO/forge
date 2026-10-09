module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
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
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.multiformats.multiaddr;
import forge.net.tcp.connection;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/cancellation_latch.hxx"
#include "details/coordinated_dial.hxx"
#include "details/node_impl.hxx"
#include "details/stream_upgrade.hxx"

namespace forge::net::p2p {
namespace asio = boost::asio;
namespace {

endpoint concrete_coordinated_endpoint(endpoint value) {
   if ((!value.is_direct_tcp() && !value.is_direct_quic()) || value.transport.port == 0 ||
       !value.transport.zone.empty() ||
       (value.transport.host_type != endpoint::host_kind::ip4 &&
        value.transport.host_type != endpoint::host_kind::ip6)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated connect requires concrete direct IP endpoints");
   }
   auto error = boost::system::error_code{};
   const auto address = asio::ip::make_address(value.transport.host, error);
   if (error || address.is_unspecified() || address.is_multicast() ||
       address.is_v4() != (value.transport.host_type == endpoint::host_kind::ip4)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated connect requires a concrete unicast IP");
   }
   value.transport.host = address.to_string();
   return value;
}

} // namespace

bool node::impl::has_coordinated_dials() const {
   const auto lock = std::scoped_lock{mutex};
   return !coordinated_dials.empty();
}

void node::impl::cancel_coordinated_dials() noexcept {
   auto owners = std::array<std::shared_ptr<detail::coordinated_dial>, detail::coordinated_dial::max_active>{};
   auto count = std::size_t{};
   {
      const auto lock = std::scoped_lock{mutex};
      coordinated_admission_closed = true;
      for (const auto& [_, owner] : coordinated_dials) { owners[count++] = owner; }
   }
   for (std::size_t index = 0; index != count; ++index) { owners[index]->request_cancel(); }
}

boost::asio::awaitable<void> node::impl::join_coordinated_dials() {
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   auto owners = std::array<std::shared_ptr<detail::coordinated_dial>, detail::coordinated_dial::max_active>{};
   auto count = std::size_t{};
   {
      const auto lock = std::scoped_lock{mutex};
      for (const auto& [_, owner] : coordinated_dials) { owners[count++] = owner; }
   }
   for (std::size_t index = 0; index != count; ++index) { co_await owners[index]->async_join(); }
}

boost::asio::awaitable<void>
node::impl::run_coordinated_dial(std::shared_ptr<detail::coordinated_dial> operation) {
   auto connection = direct::connection{};
   auto failure = std::exception_ptr{};
   try {
      connection = co_await direct_registry.async_connect_coordinated(operation);
      const auto inbound = connection.role == upgrade_role::responder && connection.remote_endpoint &&
          connection.remote_endpoint->is_direct_quic();
      static_cast<void>(operation->install(connection, inbound));
   } catch (...) { failure = std::current_exception(); }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   co_await direct::async_discard_unpublished(connection);
   if (failure) { std::rethrow_exception(failure); }
}

boost::asio::awaitable<node::session_info>
node::impl::connect_coordinated(endpoint remote, node::coordinated_connect_options value,
                                std::shared_ptr<cancellation_latch> parent, bool upgrade_only) {
   validate_operation_timeout(value.timeout, "P2P coordinated timeout");
   if (value.timeout >= std::chrono::duration_cast<std::chrono::milliseconds>(
           std::chrono::steady_clock::time_point::max() - std::chrono::steady_clock::now())) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated timeout exceeds the steady clock range");
   }
   if (!valid_peer_id(value.expected_peer) || value.expected_peer == local ||
       (value.side != node::coordinated_connect_options::role::initiator &&
        value.side != node::coordinated_connect_options::role::responder)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated connect requires an expected remote identity and role");
   }
   remote = concrete_coordinated_endpoint(std::move(remote));
   value.local_source = concrete_coordinated_endpoint(std::move(value.local_source));
   if ((remote.peer && *remote.peer != value.expected_peer) ||
       (value.local_source.peer && *value.local_source.peer != local)) {
      FORGE_THROW_EXCEPTION(exceptions::peer_verification_failed, "coordinated endpoint identity does not match its owner");
   }
   if (remote.transport.protocol != value.local_source.transport.protocol ||
       remote.transport.host_type != value.local_source.transport.host_type) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "coordinated endpoints must use the same transport and IP family");
   }
   require_private_direct_tcp(remote, "coordinated connect");
   remote.peer.reset();
   value.local_source.peer.reset();
   const auto key = std::make_pair(value.local_source.to_string(), remote.to_string());
   remote.peer = value.expected_peer;
   auto tracked = lifecycle.track();
   if (!tracked.active()) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P lifecycle is stopping"); }
   const auto executor = tracked.executor();
   const auto role = value.side == node::coordinated_connect_options::role::initiator
       ? upgrade_role::initiator : upgrade_role::responder;
   auto operation = std::shared_ptr<detail::coordinated_dial>{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped || session_admission_closed || coordinated_admission_closed) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P coordinated admission is closed");
      }
      if (!options.path_policy.allow_direct) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P direct paths are disabled");
      }
      if (coordinated_dials.contains(key)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P coordinated tuple is already active");
      }
      if (coordinated_dials.size() == detail::coordinated_dial::max_active) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P coordinated operations are at capacity");
      }
      if (coordinated_generation == std::numeric_limits<std::uint64_t>::max()) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P coordinated generation exhausted");
      }
      operation = std::make_shared<detail::coordinated_dial>(executor, std::move(remote), std::move(value),
                                                           ++coordinated_generation, role);
      coordinated_dials.emplace(key, operation);
   }

   auto failure = std::exception_ptr{};
   auto deadline = std::unique_ptr<operation_deadline>{};
   auto attempt = detail::direct_attempt{};
   auto connection = direct::connection{};
   auto result = node::session_info{};
   auto launched = false;
   auto setup_failure = std::exception_ptr{};
   auto parent_stop = cancellation_latch::subscription{};
   try {
      parent_stop = cancellation_latch::subscribe(parent, [operation] noexcept { operation->request_cancel(); });
      const auto inherited = co_await asio::this_coro::cancellation_state;
      if (inherited.cancelled() != asio::cancellation_type::none) { operation->request_cancel(); }
      co_await asio::this_coro::reset_cancellation_state(
          [operation](asio::cancellation_type type) noexcept {
             if (type != asio::cancellation_type::none) { operation->request_cancel(); }
             return asio::cancellation_type::none;
          }, asio::disable_cancellation{});
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          operation->deadline - std::chrono::steady_clock::now());
      if (remaining <= std::chrono::milliseconds::zero()) { operation->request_cancel(true); }
      else {
         deadline = std::make_unique<operation_deadline>(runtime.context(), remaining);
         deadline->arm([operation] noexcept { operation->request_cancel(true); });
      }
      // Prepare publication ownership before creating any native connection.
      auto staged = std::make_shared<detail::direct_attempt_resources>();
      staged->teardown_ticket = teardown.track([weak = std::weak_ptr{operation}] noexcept {
         if (const auto owner = weak.lock()) { owner->request_cancel(); }
      });
      if (!staged->teardown_ticket.active()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "P2P coordinated teardown ownership is closed");
      }
      auto announcement = std::function<asio::awaitable<void>()>{
          [self = shared_from_this(), peer = operation->options.expected_peer] {
             return self->announce_pubsub_subscriptions(peer);
          }};
      auto error = boost::system::error_code{};
      co_await operation->async_run([&, self = shared_from_this()] {
         launched = true;
         if (operation->stopped()) { operation->end_outbound({}); return; }
         // Install the exact listener-generation lease synchronously, before
         // publishing either the outgoing worker or a role-changing accept.
         try {
            direct_registry.prepare_coordinated(operation);
            asio::co_spawn(executor, self->run_coordinated_dial(operation),
                [self, operation](std::exception_ptr error) noexcept { operation->end_outbound(std::move(error)); });
         } catch (...) { setup_failure = std::current_exception(); throw; }
      }, asio::redirect_error(asio::use_awaitable, error));
      if (error && error != asio::error::operation_aborted) { throw boost::system::system_error{error}; }
      connection = operation->take();
      if (setup_failure) { std::rethrow_exception(setup_failure); }
      if (operation->timed_out()) { throw_operation_timeout("P2P coordinated connect"); }
      if (operation->stopped()) { FORGE_THROW_EXCEPTION(exceptions::canceled, "P2P coordinated connect canceled"); }
      if (!connection.session.valid()) {
         if (const auto native_failure = operation->failure()) { std::rethrow_exception(native_failure); }
         FORGE_THROW_EXCEPTION(exceptions::peer_not_found, "P2P coordinated connect has no exact native winner");
      }
      // Allocate metadata before moving the only terminal transport owner.
      attempt.target = operation->remote;
      auto roots = std::vector<forge::multiformats::multiaddr>{operation->remote.to_multiaddr()};
      staged->session = std::move(*connection.admission);
      connection.admission.reset();
      attempt.resources = std::move(staged);
      attempt.connection = std::move(connection);
      attempt.started_at = operation->deadline - operation->options.timeout;
      const auto direction = operation->inbound_winner() ? connection_manager::direction::inbound
                                                        : connection_manager::direction::outbound;
      const auto session = co_await commit_direct_attempt(std::move(attempt), std::move(roots),
          operation->deadline, operation->cancellation, upgrade_only, direction, false);
      result = session_info_for(session);
      // Publication completes this operation. Ancillary protocol work belongs
      // to the existing host lifecycle, not the caller's native dial barrier.
      static_cast<void>(launch_tracked(std::move(announcement)));
   } catch (...) {
      failure = std::current_exception();
   }

   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   if (!launched) {
      // Native wait preparation failed before children existed. Retire its
      // reserved setup marker once, without an allocating retry/drain loop.
      operation->request_cancel();
      operation->end_outbound(failure);
   }
   if (attempt.connection.session.valid() || attempt.connection.native_lifetime || attempt.resources) {
      try { co_await async_close_direct_attempt(attempt); }
      catch (...) { if (!failure) { failure = std::current_exception(); } }
   }
   co_await direct::async_discard_unpublished(connection);
   auto unpublished = operation->take();
   co_await direct::async_discard_unpublished(unpublished);
   direct_registry.release_coordinated(operation);
   if (deadline) { static_cast<void>(deadline->finish()); }
   tracked.release();
   operation->finish();
   parent_stop.reset();
   {
      const auto lock = std::scoped_lock{mutex};
      const auto found = coordinated_dials.find(key);
      if (found != coordinated_dials.end() && found->second == operation) { coordinated_dials.erase(found); }
   }
   if (failure) { std::rethrow_exception(failure); }
   co_return result;
}

} // namespace forge::net::p2p
