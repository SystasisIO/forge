module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <iterator>
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
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/scope/scope_exit.hpp>

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
import forge.net.p2p.envelope;
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
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.tcp.connection;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/cancellation_latch.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/node_impl.hxx"
#include "details/owner_cancellation.hxx"
#include "details/observed_address_manager.hxx"
#include "details/path_manager_dial_batch.hxx"
#include "details/stream_upgrade.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;
using path_owner = detail::path_manager::operation;
using path_role = detail::path_manager::role;
using path_exchange = detail::path_manager::exchange;

std::vector<endpoint> node::impl::local_hole_punch_endpoints() const {
   auto listeners = std::vector<endpoint>{};
   auto observations = std::shared_ptr<detail::observed_address_manager>{};
   auto limit = std::size_t{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped || private_network_enabled()) { return {}; }
      listeners = direct_registry.local_endpoints();
      observations = observed_addresses;
      limit = std::min(options.path_policy.max_direct_endpoints, hole_punch::options{}.max_observed_endpoints);
   }
   auto values = std::vector<endpoint>{};
   for (const auto& listener : listeners) {
      if (host_addresses::classify_endpoint_scope(listener) == host_addresses::endpoint_scope::public_address) {
         values.push_back(listener);
      }
   }
   if (observations) {
      auto candidates = observations->hole_punch_candidates(std::chrono::steady_clock::now(), listeners, limit);
      values.insert(values.end(), std::make_move_iterator(candidates.begin()), std::make_move_iterator(candidates.end()));
   }
   // This list is protocol-private: no configured WAN inference, circuit
   // addresses, Identify publication or weakening of advertisement quorum.
   return detail::path_manager::direct_endpoints(values, local, limit);
}

bool node::impl::path_session_eligible(const std::shared_ptr<session_state>& session, path_role side) const {
   const auto lock = std::scoped_lock{mutex};
   const auto found = sessions.find(session->id);
   return !stopped && !session_admission_closed && !private_network_enabled() && !session->closed &&
       options.capabilities.has(capabilities::hole_punching) && options.path_policy.allow_hole_punch &&
       options.path_policy.allow_direct && found != sessions.end() && found->second == session &&
       detail::path_manager::eligible(session->info.path, session->authentication,
           session->direction == connection_manager::direction::inbound, side);
}

std::shared_ptr<node::impl::session_state> node::impl::authenticated_direct_session(const peer_id& peer) const {
   const auto lock = std::scoped_lock{mutex};
   if (stopped || session_admission_closed) { return {}; }
   for (const auto& [_, session] : sessions) {
      if (!session->closed && session->info.path == path::kind::direct && session->info.remote_peer == peer &&
          session->authentication != peer_authentication::unverified) { return session; }
   }
   return {};
}

detail::path_manager::claim node::impl::request_path_upgrade(
    const std::shared_ptr<session_state>& session, std::chrono::steady_clock::time_point deadline) {
   const auto side = session->direction == connection_manager::direction::inbound
       ? path_role::initiator : path_role::responder;
   if (!paths || !path_session_eligible(session, side)) { return {}; }
   auto claim = paths->begin(session->info.remote_peer, session->id, side, deadline);
   if (!claim.leader) { return claim; }
   auto self = shared_from_this();
   auto tracked = lifecycle.track();
   if (!tracked.active()) {
      claim.owner->cancellation->request_stop();
      paths->finish(claim.owner, hole_punch::status::failed);
      return claim;
   }
   const auto executor = tracked.executor();
   try {
      asio::co_spawn(executor, self->run_path_upgrade(session, claim.owner),
          [self, owner = claim.owner, tracked = std::move(tracked)](
              std::exception_ptr error, hole_punch::status result) mutable noexcept {
             if (error) { result = hole_punch::status::failed; }
             owner->cancellation->request_stop();
             tracked.release();
             try { self->record_hole_punch_result(result); } catch (...) {}
             self->paths->finish(owner, result);
          });
   } catch (...) {
      claim.owner->cancellation->request_stop();
      paths->finish(claim.owner, hole_punch::status::failed);
   }
   return claim;
}

boost::asio::awaitable<bool> node::impl::wait_path_identify(
    const std::shared_ptr<session_state>& session, const std::shared_ptr<path_owner>& owner,
    const std::shared_ptr<path_exchange>& ticket) {
   for (;;) {
      const auto epoch = lifecycle_wakeup->epoch();
      if (!ticket && paths->inspect(owner).handed_over) { co_return true; }
      if (owner->cancellation->stop_requested() || std::chrono::steady_clock::now() >= owner->deadline ||
          (ticket && ticket->cancellation->stop_requested()) ||
          !path_session_eligible(session, ticket ? ticket->side : owner->side)) { co_return false; }
      {
         const auto lock = std::scoped_lock{mutex};
         if (session->identify_completed) {
            co_return session->info.identify_state == identify::state::identified;
         }
      }
      co_await lifecycle_wakeup->async_wait_until(epoch, owner->deadline);
   }
}

boost::asio::awaitable<hole_punch::status> node::impl::run_path_upgrade(
    std::shared_ptr<session_state> session, std::shared_ptr<path_owner> owner) {
   auto result = hole_punch::status::failed;
   auto deadline = std::unique_ptr<operation_deadline>{};
   auto stopped = cancellation_latch::subscription{};
   try {
      stopped = cancellation_latch::subscribe(owner->cancellation,
          [wakeup = lifecycle_wakeup] { wakeup->notify(); });
      const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
          owner->deadline - std::chrono::steady_clock::now());
      if (remaining > std::chrono::milliseconds::zero() && !owner->cancellation->stop_requested()) {
         deadline = std::make_unique<operation_deadline>(runtime.context(), remaining);
         deadline->arm([cancellation = owner->cancellation] { cancellation->request_stop(); });
         launch_identify(session);
         const auto identified = co_await wait_path_identify(session, owner);
         if (identified && owner->side == path_role::initiator) {
               auto candidates = std::vector<endpoint>{};
               if (const auto record = store.find(owner->peer)) {
                  for (const auto& value : path_selector::rank_direct(*record, std::chrono::system_clock::now())) {
                     if (value.backoff_until > std::chrono::system_clock::now()) { continue; }
                     try {
                        auto endpoint = parse_endpoint(value.address.to_string());
                        if (host_addresses::classify_endpoint_scope(endpoint) ==
                            host_addresses::endpoint_scope::public_address) {
                           candidates.push_back(std::move(endpoint));
                        }
                     } catch (...) {}
                     if (candidates.size() >= detail::path_manager::max_parallel_dials) { break; }
                  }
               }
               candidates = detail::path_manager::direct_endpoints(candidates, owner->peer,
                   std::min(options.path_policy.max_direct_endpoints, detail::path_manager::max_parallel_dials));
               if (!candidates.empty() && !authenticated_direct_session(owner->peer)) {
                  // The ordinary shortcut shares exchange exclusion and native
                  // join, but does not consume a DCUtR retry.
                  const auto ticket = paths->start_exchange(owner, false);
                  if (ticket) {
                  auto exchange = boost::scope::scope_exit{[this, owner, ticket] { paths->end_exchange(owner, ticket); }};
                  auto parent = cancellation_latch::subscribe(owner->cancellation,
                      [cancellation = ticket->cancellation] { cancellation->request_stop(); });
                  auto roots = std::vector<forge::multiformats::multiaddr>{};
                  for (const auto& candidate : candidates) { roots.push_back(candidate.to_multiaddr()); }
                  const auto remaining_dial = std::chrono::ceil<std::chrono::milliseconds>(
                      owner->deadline - std::chrono::steady_clock::now());
                  try {
                     static_cast<void>(co_await connect_direct(std::move(roots), node::connect_options{
                         .expected_peer = owner->peer, .allow_relay = false,
                         .timeout = std::min(remaining_dial, std::chrono::milliseconds{2'000}),
                         .max_direct_endpoints = candidates.size(), .allow_hole_punch = false,
                     }, ticket->cancellation));
                  } catch (...) {}
                  }
               }
         }
            for (;;) {
               const auto epoch = lifecycle_wakeup->epoch();
               const auto progress = paths->inspect(owner);
               if (authenticated_direct_session(owner->peer) || owner->cancellation->stop_requested() ||
                   std::chrono::steady_clock::now() >= owner->deadline ||
                   (progress.attempts >= detail::path_manager::max_attempts && !progress.exchanging)) { break; }
               // A handover's session and role belong to its immutable ticket.
               // The original circuit disappearing must not cancel that wave.
               if (!progress.handed_over) {
                  if (!identified || !path_session_eligible(session, owner->side)) {
                     if (paths->seal(owner, true)) { break; }
                     continue;
                  }
                  if (owner->side == path_role::initiator) {
                     if (const auto ticket = paths->start_exchange(owner)) {
                        auto exchange = boost::scope::scope_exit{
                            [this, owner, ticket] { paths->end_exchange(owner, ticket); }};
                        if (co_await run_dcutr_initiator(session, owner, ticket)) { break; }
                        continue;
                     }
                  }
               }
               co_await lifecycle_wakeup->async_wait_until(epoch, owner->deadline);
            }
      }
      if (authenticated_direct_session(owner->peer) && !owner->cancellation->stop_requested() &&
          std::chrono::steady_clock::now() < owner->deadline) { result = hole_punch::status::succeeded; }
   } catch (...) {
      // No path failure retires the authenticated relay connection or its streams.
   }
   static_cast<void>(paths->seal(owner));
   owner->cancellation->request_stop();
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   for (;;) {
      const auto epoch = lifecycle_wakeup->epoch();
      if (!paths->inspect(owner).exchanging) { break; }
      co_await lifecycle_wakeup->async_wait(epoch);
   }
   if (deadline) { static_cast<void>(deadline->finish()); }
   stopped.reset();
   co_return result;
}

boost::asio::awaitable<void> node::impl::handle_dcutr(std::shared_ptr<session_state> session,
                                                    forge::net::p2p::stream stream,
                                                    std::shared_ptr<detail::resource_stream> resource) {
   if (!path_session_eligible(session, path_role::responder) ||
       stream.authentication() == peer_authentication::unverified ||
       stream.authentication() != session->authentication || !resource) {
      stream.request_cancel();
      FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR requires an authenticated outbound relay session");
   }
   const auto claim = request_path_upgrade(session, std::chrono::steady_clock::now() + hole_punch::options{}.timeout);
   const auto owner = claim.owner;
   if (!owner) {
      stream.request_cancel();
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "DCUtR peer operation is unavailable or already exchanging");
   }
   const auto ticket = std::make_shared<path_exchange>(path_exchange{
       session->id, path_role::responder, std::make_shared<cancellation_latch>()});
   auto exchange = boost::scope::scope_exit{[this, owner, ticket] { paths->end_exchange(owner, ticket); }};
   auto cancellation = cancellation_latch::subscription{};
   auto child_cancellation = cancellation_latch::subscription{};
   auto failure = std::exception_ptr{};
   try {
      cancellation = cancellation_latch::subscribe(owner->cancellation,
          [resource] { resource->request_cancel(); });
      child_cancellation = cancellation_latch::subscribe(ticket->cancellation,
          [resource] { resource->request_cancel(); });
      if (!co_await paths->async_accept_exchange(owner, ticket, local)) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "DCUtR peer exchange is unavailable");
      }
      launch_identify(session);
      auto memory = resource->reserve_memory(hole_punch::options{}.max_message_size,
          resource_manager::memory_priority::always);
      if (!memory) { FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "DCUtR decode memory limit reached"); }
      if (!co_await wait_path_identify(session, owner, ticket)) {
         FORGE_THROW_EXCEPTION(exceptions::canceled, "DCUtR stopped before Identify completed");
      }
      auto observed = local_hole_punch_endpoints();
      if (observed.empty()) { FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR has no local direct endpoints"); }
      auto buffer = std::vector<std::uint8_t>{};
      const auto request = hole_punch::codec::decode(
          co_await async_read_length_delimited(stream, buffer, hole_punch::options{}.max_message_size));
      if (request.kind != hole_punch::message::message_kind::connect) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR expected CONNECT");
      }
      auto candidates = detail::path_manager::direct_endpoints(request.observed_endpoints, owner->peer,
          std::min(options.path_policy.max_direct_endpoints, detail::path_manager::max_parallel_dials));
      if (candidates.empty()) { FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR has no remote direct endpoints"); }
      co_await stream.async_write(hole_punch::codec::encode(hole_punch::message{
          .kind = hole_punch::message::message_kind::connect, .observed_endpoints = std::move(observed),
      }));
      const auto sync = hole_punch::codec::decode(
          co_await async_read_length_delimited(stream, buffer, hole_punch::options{}.max_message_size));
      if (sync.kind != hole_punch::message::message_kind::sync || !sync.observed_endpoints.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR expected empty SYNC");
      }
      if (!buffer.empty()) { FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR has trailing exchange data"); }
      co_await paths->async_close_completed_exchange(resource);
      cancellation.reset();
      static_cast<void>(co_await dial_coordinated_path(std::move(candidates), owner, ticket));
   } catch (...) { failure = std::current_exception(); }
   cancellation.reset();
   child_cancellation.reset();
   // Await the native barrier before the exchange guard admits another handler.
   co_await paths->async_close_exchange(resource, failure);
}

boost::asio::awaitable<bool> node::impl::run_dcutr_initiator(
    const std::shared_ptr<session_state>& session, const std::shared_ptr<path_owner>& owner,
    const std::shared_ptr<path_exchange>& ticket) {
   auto resource = std::shared_ptr<detail::resource_stream>{};
   auto candidates = std::vector<endpoint>{};
   auto delayed = false;
   auto succeeded = false;
   try {
      auto observed = local_hole_punch_endpoints();
      if (observed.empty() || !path_session_eligible(session, path_role::initiator)) { co_return false; }
      co_await paths->async_run_exchange(owner,
          [this, session, owner, ticket, &resource, &observed, &candidates, &delayed](
              asio::cancellation_slot slot, std::shared_ptr<detail::worker_stop_bridge> stop) -> asio::awaitable<void> {
             const auto admitted = detail::make_owner_stream_admission(slot, std::move(stop));
             auto stream = co_await open_session_stream(session, builtins::dcutr, true,
                 detail::stream_admission_handler{
                     [&resource, admitted](const std::shared_ptr<detail::resource_stream>& value) {
                        resource = value;
                        admitted(value);
                     }, [admitted] { admitted.commit(); },
                 });
             if (!resource) { FORGE_THROW_EXCEPTION(exceptions::internal, "DCUtR stream lost its resource scope"); }
             auto memory = resource->reserve_memory(hole_punch::options{}.max_message_size,
                 resource_manager::memory_priority::always);
             if (!memory) { FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "DCUtR decode memory limit reached"); }
             const auto sent = std::chrono::steady_clock::now();
             co_await stream.async_write(hole_punch::codec::encode(hole_punch::message{
                 .kind = hole_punch::message::message_kind::connect, .observed_endpoints = std::move(observed),
             }));
             auto buffer = std::vector<std::uint8_t>{};
             const auto response = hole_punch::codec::decode(
                 co_await async_read_length_delimited(stream, buffer, hole_punch::options{}.max_message_size));
             const auto rtt = std::chrono::steady_clock::now() - sent;
             if (response.kind != hole_punch::message::message_kind::connect || !buffer.empty()) {
                FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR expected one CONNECT response");
             }
             candidates = detail::path_manager::direct_endpoints(response.observed_endpoints, owner->peer,
                 std::min(options.path_policy.max_direct_endpoints, detail::path_manager::max_parallel_dials));
             if (candidates.empty()) { FORGE_THROW_EXCEPTION(exceptions::protocol_error, "DCUtR has no remote direct endpoints"); }
             co_await stream.async_write(hole_punch::codec::encode(hole_punch::message{
                 .kind = hole_punch::message::message_kind::sync,
             }));
             delayed = co_await paths->async_delay(owner, std::chrono::steady_clock::now() + rtt / 2, ticket);
          }, ticket);
   } catch (...) { delayed = false; }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   if (resource) {
      try {
         if (delayed) { co_await paths->async_close_completed_exchange(resource); }
         else {
            resource->request_cancel();
            co_await paths->async_close_exchange(resource);
         }
      } catch (...) { delayed = false; }
   }
   if (delayed && !owner->cancellation->stop_requested() && !ticket->cancellation->stop_requested() &&
       std::chrono::steady_clock::now() < owner->deadline) {
      try { succeeded = co_await dial_coordinated_path(std::move(candidates), owner, ticket); } catch (...) {}
   }
   co_return succeeded;
}

boost::asio::awaitable<void> node::impl::dial_path_candidate(endpoint candidate,
    std::shared_ptr<path_owner> owner, std::shared_ptr<path_exchange> ticket, std::shared_ptr<path_dial_batch> batch,
    std::chrono::steady_clock::time_point deadline, std::optional<endpoint> local_source) {
   const auto budget = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
   if (!local_source || budget <= std::chrono::milliseconds::zero() || batch->cancellation->stop_requested() ||
       owner->cancellation->stop_requested() || ticket->cancellation->stop_requested() ||
       authenticated_direct_session(owner->peer)) { co_return; }
   using role = node::coordinated_connect_options::role;
   // Use the real exact-tuple owner: its TCP lease accounts for inbound native
   // workers as well as the outgoing worker, and return awaits both close barriers.
   static_cast<void>(co_await connect_coordinated(std::move(candidate), node::coordinated_connect_options{
       .expected_peer = owner->peer, .local_source = std::move(*local_source),
       .side = ticket->side == path_role::initiator ? role::responder : role::initiator, .timeout = budget,
   }, batch->cancellation, true));
}

boost::asio::awaitable<bool> node::impl::dial_coordinated_path(
    std::vector<endpoint> candidates, const std::shared_ptr<path_owner>& owner,
    const std::shared_ptr<path_exchange>& ticket) {
   candidates = detail::path_manager::direct_endpoints(candidates, owner->peer,
       std::min(options.path_policy.max_direct_endpoints, detail::path_manager::max_parallel_dials));
   if (candidates.empty() || owner->cancellation->stop_requested() || ticket->cancellation->stop_requested()) {
      co_return false;
   }
   const auto now = std::chrono::steady_clock::now();
   const auto deadline = detail::path_manager::dial_deadline(owner->deadline, now);
   if (deadline <= now) { co_return false; }
   // All cancellation is delivered through owned latches. An outer Asio slot
   // must not unwind the coordinator before its candidate workers have joined.
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   auto batch = std::make_shared<path_dial_batch>(runtime.context().get_executor());
   auto expires = operation_deadline{runtime.context(), std::chrono::ceil<std::chrono::milliseconds>(deadline - now)};
   expires.arm([cancellation = batch->cancellation] { cancellation->request_stop(); });
   const auto self = shared_from_this();
   auto listeners = std::vector<endpoint>{};
   auto relay_local = std::optional<endpoint>{};
   {
      const auto lock = std::scoped_lock{mutex};
      listeners = direct_registry.local_endpoints();
      const auto session = sessions.find(ticket->session_id);
      if (session != sessions.end() && session->second->info.carrier_session_id) {
         const auto carrier = sessions.find(*session->second->info.carrier_session_id);
         if (carrier != sessions.end() && !carrier->second->closed &&
             carrier->second->info.path == path::kind::direct &&
             carrier->second->authentication != peer_authentication::unverified &&
             session->second->info.relay_peer == carrier->second->info.remote_peer) {
            relay_local = carrier->second->local_endpoint;
         }
      }
   }
   const auto has_direct = std::function<bool()>{[self, owner] {
      return static_cast<bool>(self->authenticated_direct_session(owner->peer));
   }};
   const auto launch = std::function<void()>{[&] {
      for (auto& candidate : candidates) {
         if (batch->cancellation->stop_requested()) { break; }
         auto source = std::optional<endpoint>{};
         try {
            auto routed_source = std::optional<endpoint>{};
            if (!relay_local || relay_local->transport.protocol != candidate.transport.protocol ||
                relay_local->transport.host_type != candidate.transport.host_type) {
               routed_source = direct::select_dial_source(runtime.context(), resources, listeners, candidate);
            }
            source = detail::path_manager::coordinated_source(listeners, candidate, relay_local, routed_source);
         } catch (...) { continue; }
         auto tracked = lifecycle.track();
         if (!tracked.active()) { break; }
         const auto executor = tracked.executor();
         // Simultaneous TCP can authenticate on the listener after an early
         // outbound refusal. Both protocols retain that remaining wave budget.
         const auto wait_inbound = candidate.is_direct_tcp() ||
             (candidate.is_direct_quic() && ticket->side == path_role::initiator);
         batch->add_worker(wait_inbound);
         try {
            asio::co_spawn(executor, self->dial_path_candidate(std::move(candidate), owner, ticket, batch, deadline, std::move(source)),
                [self, batch, tracked = std::move(tracked)](std::exception_ptr) mutable noexcept {
                   tracked.release();
                   self->lifecycle_wakeup->notify();
                   batch->complete_worker();
                });
         } catch (...) {
            tracked.release();
            batch->complete_worker();
            throw;
         }
      }
   }};
   const auto succeeded = co_await paths->async_wait_dials(batch, owner, deadline, has_direct, launch, ticket);
   static_cast<void>(expires.finish());
   co_return succeeded;
}

boost::asio::awaitable<bool> node::impl::wait_for_direct_session(const peer_id& peer,
                                                              std::chrono::milliseconds timeout) {
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   for (;;) {
      const auto epoch = lifecycle_wakeup->epoch();
      if (lifecycle.stop_requested()) { co_return false; }
      if (authenticated_direct_session(peer)) { co_return true; }
      if (std::chrono::steady_clock::now() >= deadline) { co_return false; }
      co_await lifecycle_wakeup->async_wait_until(epoch, deadline);
   }
}

boost::asio::awaitable<hole_punch::status> node::impl::attempt_hole_punch(
    peer_id peer, std::optional<peer_id> relay_peer, std::chrono::milliseconds timeout) {
   validate_operation_timeout(timeout, "P2P hole punch timeout");
   const auto deadline = std::chrono::steady_clock::now() + timeout;
   if (authenticated_direct_session(peer)) { co_return hole_punch::status::succeeded; }
   auto session = session_for_path(peer, path::kind::relay, relay_peer);
   if (!session && !relay_peer) {
      if (const auto record = store.find(peer)) {
         for (const auto& value : record->endpoints) {
            if (value.relay_peer) { relay_peer = value.relay_peer; break; }
         }
      }
   }
   if (!session && !relay_peer) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P hole punching requires a relay peer");
   }
   try {
      if (!session) {
         session = co_await ensure_relay_session(peer, *relay_peer,
             std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
      }
      if (authenticated_direct_session(peer)) { co_return hole_punch::status::succeeded; }
      const auto claim = request_path_upgrade(session, deadline);
      if (claim.owner) { co_return co_await paths->async_wait(claim.owner, deadline); }
   } catch (...) {
      // The existing relay remains a usable fallback for callers opening streams.
   }
   co_return hole_punch::status::failed;
}

} // namespace forge::net::p2p
