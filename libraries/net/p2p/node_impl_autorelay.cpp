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
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

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
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"

#include "details/cancellation_latch.hxx"
#include "details/host_addresses.hxx"
#include "details/reachability_manager.hxx"

namespace forge::net::p2p {

void node::impl::initialize_autorelay() {
   const auto weak = weak_from_this();
   autorelay_manager_value = std::make_shared<detail::autorelay_manager>(
       runtime.context().get_executor(), options.relay_policy,
       detail::autorelay_manager::callbacks{
           .current = [weak] {
              if (const auto self = weak.lock()) { return self->autorelay_snapshot(); }
              return detail::autorelay_manager::snapshot{};
           },
           .reserve = [weak](auto candidate, std::uint64_t generation, auto cancellation) {
              const auto self = weak.lock();
              if (!self) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P AutoRelay owner is closed"); }
              return reserve_autorelay_owned(self, std::move(candidate), generation, std::move(cancellation));
           },
       });
}

void node::impl::start_autorelay() {
   if (!autorelay_manager_value || private_network_enabled() ||
       (!options.relay_policy.client_enabled && !options.relay_policy.service_enabled)) { return; }
   autorelay_manager_value->start(lifecycle);
}

void node::impl::notify_autorelay_changed() noexcept {
   if (autorelay_manager_value) { autorelay_manager_value->notify(); }
}

boost::asio::awaitable<void> node::impl::join_autorelay() {
   if (autorelay_manager_value) { co_await autorelay_manager_value->async_join(); }
}

void node::impl::stop_autorelay() noexcept {
   auto operations = std::map<peer_id, std::shared_ptr<relay_reservation_operation>>{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (autorelay_generation != (std::numeric_limits<std::uint64_t>::max)()) { ++autorelay_generation; }
      operations.swap(relay_reservation_operations);
      for (const auto& [peer, _] : outbound_relay_reservations) {
         static_cast<void>(connections.unprotect(peer, "forge:relay-client"));
      }
      outbound_relay_reservations.clear();
      published_relay_endpoints.clear();
      static_cast<void>(advance_identify_generation_locked());
   }
   if (autorelay_manager_value) { autorelay_manager_value->request_stop(); }
   for (const auto& [_, operation] : operations) { operation->cancellation->request_stop(); }
}

std::vector<endpoint> node::impl::relay_advertised_endpoints_locked() const {
   auto canonical = std::map<std::string, endpoint>{};
   if (stopped || session_admission_closed || private_network_enabled() || !options.relay_policy.client_enabled ||
       (reachability_manager_value &&
        reachability_manager_value->current().effective == reachability::state::publicly_reachable)) { return {}; }
   const auto now = std::chrono::steady_clock::now();
   for (const auto& [peer, value] : outbound_relay_reservations) {
      const auto session = sessions.find(value.session_id);
      if (value.canceled || value.expires_at <= now || session == sessions.end() || session->second->closed ||
          session->second->info.path != path::kind::direct || session->second->info.remote_peer != peer) { continue; }
      for (auto address : value.info.relay_endpoints) {
         address.relayed = endpoint::circuit{.target = local};
         canonical.emplace(address.to_string(), std::move(address));
         if (canonical.size() == options.identify.max_listen_endpoints) { break; }
      }
      if (canonical.size() == options.identify.max_listen_endpoints) { break; }
   }
   auto result = std::vector<endpoint>{};
   result.reserve(canonical.size());
   for (auto& [_, address] : canonical) { result.push_back(std::move(address)); }
   return result;
}

void node::impl::refresh_relay_publication() {
   auto cancellations = std::vector<std::shared_ptr<relay_reservation_operation>>{};
   auto push = false;
   auto changed = false;
   {
      const auto lock = std::scoped_lock{mutex};
      cleanup_expired_relay_reservations_locked();
      const auto public_host = reachability_manager_value &&
          reachability_manager_value->current().effective == reachability::state::publicly_reachable;
      if (public_host != autorelay_public) {
         autorelay_public = public_host;
         if (autorelay_generation != (std::numeric_limits<std::uint64_t>::max)()) { ++autorelay_generation; }
      }
      if (public_host || stopped || session_admission_closed) {
         for (auto it = outbound_relay_reservations.begin(); it != outbound_relay_reservations.end();) {
            if (it->second.automatic || stopped || session_admission_closed) {
               static_cast<void>(connections.unprotect(it->first, "forge:relay-client"));
               it = outbound_relay_reservations.erase(it);
            } else { ++it; }
         }
         for (auto it = relay_reservation_operations.begin(); it != relay_reservation_operations.end();) {
            if (it->second->automatic || stopped || session_admission_closed ||
                !options.relay_policy.public_relay_allowed) {
               it->second->canceled = true;
               cancellations.push_back(it->second);
               ++it;
            } else { ++it; }
         }
      }
      auto addresses = relay_advertised_endpoints_locked();
      changed = !std::ranges::equal(addresses, published_relay_endpoints, {},
                                    &endpoint::to_string, &endpoint::to_string);
      if (changed) {
         published_relay_endpoints = std::move(addresses);
         push = advance_identify_generation_locked() && schedule_identify_push_locked();
      }
   }
   for (const auto& operation : cancellations) { operation->cancellation->request_stop(); }
   if (push) { launch_identify_pushes(); }
   if (changed && provider_registry) { provider_registry->notify_endpoints_changed(); }
}

detail::autorelay_manager::snapshot node::impl::autorelay_snapshot() {
   refresh_relay_publication();
   // Read manager-owned selection before the node mutex; callbacks never reverse these locks.
   const auto selection = autorelay_manager_value->selection();
   auto result = detail::autorelay_manager::snapshot{};
   auto selected = std::set<peer_id>{};
   auto excluded = std::set<peer_id>{selection.unavailable.begin(), selection.unavailable.end()};
   excluded.insert(local);
   auto hints_first = false;
   {
      const auto lock = std::scoped_lock{mutex};
      result.generation = autorelay_generation;
      result.permitted = !stopped && !session_admission_closed && !private_network_enabled() &&
          options.relay_policy.client_enabled && options.relay_policy.auto_discovery_enabled &&
          !autorelay_public && autorelay_generation != (std::numeric_limits<std::uint64_t>::max)();
      for (const auto& [_, reservation] : inbound_relay_reservations) {
         result.next_expiry = std::min(result.next_expiry, reservation.expires_at);
      }
      // Existing ownership comes first, so a bounded hint index cannot hide
      // a relay due for renewal or consume its slot with a cached claim.
      for (const auto& [peer, value] : outbound_relay_reservations) {
         const auto margin = std::min(options.relay_policy.refresh_margin, std::max(std::chrono::milliseconds{1},
                                                                                   value.info.ttl / 4));
         result.reservations.push_back({value.info, value.expires_at, value.expires_at - margin, value.automatic});
         if (value.automatic && result.candidates.size() < options.relay_policy.max_candidates_per_refresh) {
            const auto session = sessions.find(value.session_id);
            if (session != sessions.end() && !session->second->closed &&
                session->second->info.identify_state == identify::state::identified &&
                session->second->authentication != peer_authentication::unverified &&
                std::ranges::find(session->second->remote_protocols, builtins::relay_hop) !=
                    session->second->remote_protocols.end()) {
               result.candidates.push_back({peer, value.session_id});
               selected.insert(peer);
            }
         }
      }
      if (!result.permitted) { return result; }
      for (const auto& pending : selection.pending) {
         if (result.candidates.size() == options.relay_policy.max_candidates_per_refresh) { break; }
         if (selected.contains(pending.peer)) { continue; }
         if (pending.session_id != 0) {
            const auto live = sessions.find(pending.session_id);
            if (live == sessions.end() || live->second->closed || live->second->info.path != path::kind::direct ||
                live->second->info.remote_peer != pending.peer ||
                live->second->authentication == peer_authentication::unverified ||
                live->second->info.identify_state != identify::state::identified ||
                std::ranges::find(live->second->remote_protocols, builtins::relay_hop) ==
                    live->second->remote_protocols.end()) { continue; }
         }
         result.candidates.push_back(pending);
         selected.insert(pending.peer);
      }
      for (const auto& [_, live] : sessions) {
         if (live->closed || live->info.path != path::kind::direct) { continue; }
         if (live->authentication == peer_authentication::unverified ||
             live->info.identify_state == identify::state::failed ||
             (live->info.identify_state == identify::state::identified &&
              std::ranges::find(live->remote_protocols, builtins::relay_hop) == live->remote_protocols.end())) {
            excluded.insert(live->info.remote_peer);
         }
      }
      hints_first = autorelay_hints_first;
      autorelay_hints_first = !autorelay_hints_first;
   }
   const auto append_live = [&] {
      const auto lock = std::scoped_lock{mutex};
      auto next = sessions.upper_bound(autorelay_session_cursor);
      for (auto remaining = sessions.size(); remaining > 0 &&
           result.candidates.size() < options.relay_policy.max_candidates_per_refresh; --remaining) {
         if (next == sessions.end()) { next = sessions.begin(); }
         const auto& [id, session] = *next++;
         if (session->closed || session->info.path != path::kind::direct ||
             session->authentication == peer_authentication::unverified ||
             session->info.identify_state != identify::state::identified ||
             std::ranges::find(session->remote_protocols, builtins::relay_hop) == session->remote_protocols.end() ||
             excluded.contains(session->info.remote_peer) || selected.contains(session->info.remote_peer) ||
             relay_reservation_operations.contains(session->info.remote_peer)) { continue; }
         result.candidates.push_back({session->info.remote_peer, id});
         selected.insert(session->info.remote_peer);
         autorelay_session_cursor = id;
      }
   };
   const auto append_hints = [&] {
      if (result.candidates.size() == options.relay_policy.max_candidates_per_refresh) { return; }
      auto omitted = std::vector<peer_id>{excluded.begin(), excluded.end()};
      omitted.insert(omitted.end(), selected.begin(), selected.end());
      const auto hints = store.candidates(capabilities::relay | capabilities::relay_reservation,
                                          options.relay_policy.max_candidates_per_refresh, omitted);
      for (const auto& hint : relay_discovery::select_candidates(hints, relay_discovery::request{
               .local = local, .now = std::chrono::system_clock::now(),
               .limit = options.relay_policy.max_candidates_per_refresh})) {
         if (result.candidates.size() == options.relay_policy.max_candidates_per_refresh) { break; }
         if (selected.contains(hint.peer)) { continue; }
         const auto lock = std::scoped_lock{mutex};
         if (relay_reservation_operations.contains(hint.peer)) { continue; }
         const auto live = session_for_path_locked(hint.peer, path::kind::direct, std::nullopt);
         if (live && (live->authentication == peer_authentication::unverified ||
                      live->info.identify_state == identify::state::failed ||
                      (live->info.identify_state == identify::state::identified &&
                       std::ranges::find(live->remote_protocols, builtins::relay_hop) == live->remote_protocols.end()))) {
            continue;
         }
         result.candidates.push_back({hint.peer, live && live->info.identify_state == identify::state::identified ? live->id : 0});
         selected.insert(hint.peer);
      }
   };
   if (hints_first) { append_hints(); append_live(); }
   else { append_live(); append_hints(); }
   return result;
}

boost::asio::awaitable<relay::reservation::info> node::impl::reserve_autorelay_owned(
    std::shared_ptr<impl> self, detail::autorelay_manager::candidate candidate, std::uint64_t generation,
    std::shared_ptr<cancellation_latch> cancellation) {
   {
      const auto lock = std::scoped_lock{self->mutex};
      ++self->metrics_value.relay_discovery_attempts;
   }
   auto failure = std::exception_ptr{};
   auto result = std::optional<relay::reservation::info>{};
   try {
      result = co_await self->request_relay_reservation(candidate.peer,
          relay::reservation::options{
              .ttl = self->options.limits.relay.reservation_ttl,
              .max_streams = self->options.limits.relay.max_streams_per_reservation,
              .max_bytes = self->options.limits.relay.max_relay_bytes,
              .max_queued_bytes = self->options.limits.relay.max_queued_bytes},
          self->options.limits.topology.query_timeout, std::move(cancellation), true, generation, candidate.session_id);
   } catch (...) { failure = std::current_exception(); }
   if (failure) {
      try { std::rethrow_exception(failure); }
      catch (const forge::exceptions::base& error) {
         const auto code = p2p_code(error);
         if (code != exceptions::code::closed && code != exceptions::code::canceled) {
            const auto lock = std::scoped_lock{self->mutex};
            ++self->metrics_value.relay_discovery_failures;
         }
         throw;
      }
   }
   {
      const auto lock = std::scoped_lock{self->mutex};
      ++self->metrics_value.relay_discovery_successes;
   }
   co_return std::move(*result);
}

boost::asio::awaitable<std::vector<relay::reservation::info>>
node::impl::refresh_relay_candidates(std::optional<peer_id> target, std::chrono::milliseconds timeout) {
   validate_operation_timeout(timeout, "P2P AutoRelay refresh timeout");
   if (!options.relay_policy.client_enabled) {
      FORGE_THROW_EXCEPTION(exceptions::relay_not_available, "P2P relay client policy is disabled");
   }
   if (!options.relay_policy.auto_discovery_enabled) { co_return std::vector<relay::reservation::info>{}; }
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped || session_admission_closed) { FORGE_THROW_EXCEPTION(exceptions::closed, "P2P node is stopped"); }
      ++metrics_value.relay_discovery_refreshes;
   }
   start_autorelay();
   auto result = co_await autorelay_manager_value->async_refresh(timeout);
   if (target) { std::erase_if(result, [&](const auto& value) { return value.relay_peer == *target; }); }
   co_return result;
}

void node::impl::invalidate_relay_session_locked(std::uint64_t session_id) noexcept {
   auto changed = false;
   for (auto it = inbound_relay_reservations.begin(); it != inbound_relay_reservations.end();) {
      if (it->second.session_id == session_id) {
         static_cast<void>(connections.unprotect(it->first, "forge:relay-service"));
         it = inbound_relay_reservations.erase(it);
      } else { ++it; }
   }
   metrics_value.active_relay_reservations = inbound_relay_reservations.size();
   for (auto it = outbound_relay_reservations.begin(); it != outbound_relay_reservations.end();) {
      if (it->second.session_id == session_id) {
         static_cast<void>(connections.unprotect(it->first, "forge:relay-client"));
         it = outbound_relay_reservations.erase(it);
         changed = true;
      } else { ++it; }
   }
   if (changed) {
      if (autorelay_generation != (std::numeric_limits<std::uint64_t>::max)()) { ++autorelay_generation; }
      static_cast<void>(advance_identify_generation_locked());
   }
   notify_autorelay_changed();
}

void node::impl::cancel_outbound_relay(const peer_id& peer) {
   auto operation = std::shared_ptr<relay_reservation_operation>{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (autorelay_generation != (std::numeric_limits<std::uint64_t>::max)()) { ++autorelay_generation; }
      if (const auto active = relay_reservation_operations.find(peer); active != relay_reservation_operations.end()) {
         operation = active->second;
         operation->canceled = true;
      }
      outbound_relay_reservations.erase(peer);
      static_cast<void>(connections.unprotect(peer, "forge:relay-client"));
   }
   if (operation) { operation->cancellation->request_stop(); }
   if (autorelay_manager_value) { autorelay_manager_value->cancel_peer(peer); }
   refresh_relay_publication();
   notify_autorelay_changed();
}

} // namespace forge::net::p2p
