module;

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <forge/log/macros.hpp>

module forge.net.p2p.resource_manager;

import forge.log.logger;

#include "details/resource_manager_state.hxx"

namespace forge::net::p2p {

namespace {

std::atomic_bool service_bind_prepare_failpoint = false;
std::atomic_bool session_establish_prepare_failpoint = false;
std::atomic_bool dial_bind_prepare_failpoint = false;
std::atomic_bool malformed_record_prepare_failpoint = false;
std::atomic_bool lifecycle_reserve_prepare_failpoint = false;
std::atomic_bool session_reserve_prepare_failpoint = false;
std::atomic_bool dial_reserve_prepare_failpoint = false;
std::atomic_bool stream_reserve_prepare_failpoint = false;
std::atomic_bool relay_reserve_prepare_failpoint = false;

template <typename T> [[nodiscard]] bool can_add(T current, T delta, T limit) noexcept {
   return current <= limit && delta <= limit - current;
}

template <typename T> [[nodiscard]] T saturating_sum(T left, T right) noexcept {
   const auto maximum = (std::numeric_limits<T>::max)();
   return left > maximum - right ? maximum : left + right;
}

[[nodiscard]] std::size_t total_connections(const resource_manager::scope_totals& value) noexcept {
   return saturating_sum(value.inbound_connections, value.outbound_connections);
}

[[nodiscard]] std::size_t total_streams(const resource_manager::scope_totals& value) noexcept {
   return saturating_sum(value.inbound_streams, value.outbound_streams);
}

[[nodiscard]] resource_manager::scope_totals connection_delta(resource_manager::session_direction direction) noexcept {
   auto result = resource_manager::scope_totals{};
   if (direction == resource_manager::session_direction::inbound) {
      result.inbound_connections = 1;
   } else {
      result.outbound_connections = 1;
   }
   return result;
}

[[nodiscard]] resource_manager::scope_totals stream_delta(resource_manager::session_direction direction) noexcept {
   auto result = resource_manager::scope_totals{};
   if (direction == resource_manager::session_direction::inbound) {
      result.inbound_streams = 1;
   } else {
      result.outbound_streams = 1;
   }
   return result;
}

[[nodiscard]] bool empty(const resource_manager::scope_totals& value) noexcept {
   return value.memory == 0 && value.file_descriptors == 0 && value.inbound_connections == 0 &&
          value.outbound_connections == 0 && value.inbound_streams == 0 && value.outbound_streams == 0;
}

void add_saturating(resource_manager::scope_totals& target, const resource_manager::scope_totals& source) noexcept {
   target.memory = saturating_sum(target.memory, source.memory);
   target.file_descriptors = saturating_sum(target.file_descriptors, source.file_descriptors);
   target.inbound_connections = saturating_sum(target.inbound_connections, source.inbound_connections);
   target.outbound_connections = saturating_sum(target.outbound_connections, source.outbound_connections);
   target.inbound_streams = saturating_sum(target.inbound_streams, source.inbound_streams);
   target.outbound_streams = saturating_sum(target.outbound_streams, source.outbound_streams);
}

[[nodiscard]] std::uint64_t priority_threshold(std::uint64_t limit,
                                               resource_manager::memory_priority priority) noexcept {
   if (limit == (std::numeric_limits<std::uint64_t>::max)()) {
      return limit;
   }
   const auto factor = static_cast<std::uint64_t>(static_cast<std::uint8_t>(priority)) + 1;
   const auto quotient = limit / 256;
   const auto remainder = limit % 256;
   return quotient * factor + (remainder * factor) / 256;
}

// The commit phase changes counters only after every potentially allocating
// operation has completed. These swaps therefore cannot bypass rollback.
static_assert(noexcept(std::declval<std::optional<peer_id>&>().swap(std::declval<std::optional<peer_id>&>())));
static_assert(noexcept(std::declval<std::optional<protocol_id>&>().swap(std::declval<std::optional<protocol_id>&>())));
static_assert(noexcept(std::declval<std::optional<std::string>&>().swap(std::declval<std::optional<std::string>&>())));

} // namespace

resource_manager::state::state(limits value) noexcept : limits_(std::move(value)) {}

const resource_manager::limits& resource_manager::state::configured_limits() const noexcept {
   return limits_;
}

resource_manager::snapshot resource_manager::state::current() const noexcept {
   auto lock = std::scoped_lock{mutex_};
   auto out = snapshot_;
   out.system = system_.usage;
   out.transient = transient_.usage;
   out.connections = connections_.usage;
   out.streams = streams_.usage;
   const auto aggregate = [](const auto& accounts) {
      auto result = scope_totals{};
      for (const auto& [_, account] : accounts) {
         add_saturating(result, account.usage);
      }
      return result;
   };
   const auto aggregate_nested = [](const auto& accounts) {
      auto result = scope_totals{};
      for (const auto& [_, per_peer] : accounts) {
         for (const auto& [__, account] : per_peer) {
            add_saturating(result, account.usage);
         }
      }
      return result;
   };
   const auto count_active = [](const auto& accounts) {
      return static_cast<std::size_t>(std::count_if(accounts.begin(), accounts.end(),
                                                    [](const auto& entry) { return !empty(entry.second.usage); }));
   };
   const auto count_active_nested = [](const auto& accounts) {
      auto count = std::size_t{0};
      for (const auto& [_, per_peer] : accounts) {
         count += static_cast<std::size_t>(std::count_if(per_peer.begin(), per_peer.end(),
                                                         [](const auto& entry) { return !empty(entry.second.usage); }));
      }
      return count;
   };
   out.peers = aggregate(peers_);
   out.protocols = aggregate(protocols_);
   out.services = aggregate(services_);
   out.protocol_peers = aggregate_nested(protocol_peers_);
   out.service_peers = aggregate_nested(service_peers_);
   out.active_peer_scopes = count_active(peers_);
   out.active_protocol_scopes = count_active(protocols_);
   out.active_service_scopes = count_active(services_);
   out.active_protocol_peer_scopes = count_active_nested(protocol_peers_);
   out.active_service_peer_scopes = count_active_nested(service_peers_);
   out.dial_attempt_scopes = dial_attempts_by_peer_.size();
   out.relay_reservation_scopes = relay_reservations_by_peer_.size();
   out.malformed_scopes = malformed_by_peer_.size();
   return out;
}

resource_manager::state::ledger_attempt resource_manager::state::make_ledger_locked() noexcept {
   if (next_ledger_id_ == 0) {
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
   try {
      auto result = std::make_shared<ledger>();
      result->id = next_ledger_id_++;
      return {.reservation = std::move(result), .outcome = transition_result::accepted};
   } catch (...) {
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
}

bool resource_manager::state::reject_limit_locked(std::uint64_t& reason) noexcept {
   ++snapshot_.denied;
   ++reason;
   return false;
}

bool resource_manager::state::reject_invalid_transition_locked() noexcept {
   ++snapshot_.invalid_transitions;
   return false;
}

void resource_manager::state::record_runtime_failure_locked() noexcept {
   ++snapshot_.runtime_failures;
}

bool resource_manager::state::can_add_locked(const scope_account& account, const scope_limits& limits,
                                             const scope_totals& delta, memory_priority priority) const noexcept {
   if (!can_add(account.usage.memory, delta.memory, limits.max_memory)) {
      return false;
   }
   const auto new_memory = account.usage.memory + delta.memory;
   if (new_memory > priority_threshold(limits.max_memory, priority)) {
      return false;
   }
   return can_add(account.usage.file_descriptors, delta.file_descriptors, limits.max_file_descriptors) &&
          can_add(account.usage.inbound_connections, delta.inbound_connections, limits.max_inbound_connections) &&
          can_add(account.usage.outbound_connections, delta.outbound_connections, limits.max_outbound_connections) &&
          can_add(total_connections(account.usage), total_connections(delta), limits.max_connections) &&
          can_add(account.usage.inbound_streams, delta.inbound_streams, limits.max_inbound_streams) &&
          can_add(account.usage.outbound_streams, delta.outbound_streams, limits.max_outbound_streams) &&
          can_add(total_streams(account.usage), total_streams(delta), limits.max_streams);
}

bool resource_manager::state::can_remove_locked(const scope_account& account,
                                                const scope_totals& delta) const noexcept {
   return account.usage.memory >= delta.memory && account.usage.file_descriptors >= delta.file_descriptors &&
          account.usage.inbound_connections >= delta.inbound_connections &&
          account.usage.outbound_connections >= delta.outbound_connections &&
          account.usage.inbound_streams >= delta.inbound_streams &&
          account.usage.outbound_streams >= delta.outbound_streams;
}

bool resource_manager::state::can_add_to_current_scopes_locked(const ledger& value, const scope_totals& delta,
                                                               memory_priority priority,
                                                               memory_rejection* rejected) const noexcept {
   const auto allowed = [&](const scope_account& account, const scope_limits& limits, const char* scope) noexcept {
      if (can_add_locked(account, limits, delta, priority)) {
         return true;
      }
      if (rejected) {
         const auto threshold = priority_threshold(limits.max_memory, priority);
         const auto dimension = !can_add(account.usage.memory, delta.memory, limits.max_memory) ? "memory"
                                : account.usage.memory + delta.memory > threshold               ? "priority"
                                                                                                : "non_memory";
         *rejected = {.reason = "budget",
                      .scope = scope,
                      .dimension = dimension,
                      .usage = account.usage.memory,
                      .limit = limits.max_memory,
                      .threshold = threshold};
      }
      return false;
   };
   const auto missing = [&](const char* scope) noexcept {
      if (rejected) {
         *rejected = {.reason = "missing_scope", .scope = scope};
      }
      return false;
   };
   if (!allowed(system_, limits_.system, "system")) {
      return false;
   }
   if (value.transient && !allowed(transient_, limits_.transient, "transient")) {
      return false;
   }
   if (value.peer) {
      const auto peer = peers_.find(*value.peer);
      if (peer == peers_.end()) {
         return missing("peer");
      }
      if (!allowed(peer->second, limits_.peer, "peer")) {
         return false;
      }
   }
   if (value.protocol) {
      const auto protocol = protocols_.find(value.protocol->value);
      const auto peer = protocol_peers_.find(*value.peer);
      if (protocol == protocols_.end()) {
         return missing("protocol");
      }
      if (peer == protocol_peers_.end()) {
         return missing("protocol_peer");
      }
      const auto protocol_peer = peer->second.find(value.protocol->value);
      if (protocol_peer == peer->second.end()) {
         return missing("protocol_peer");
      }
      if (!allowed(protocol->second, limits_.protocol, "protocol") ||
          !allowed(protocol_peer->second, limits_.protocol_peer, "protocol_peer")) {
         return false;
      }
   }
   if (value.service) {
      const auto service = services_.find(*value.service);
      const auto peer = service_peers_.find(*value.peer);
      if (service == services_.end()) {
         return missing("service");
      }
      if (peer == service_peers_.end()) {
         return missing("service_peer");
      }
      const auto service_peer = peer->second.find(*value.service);
      if (service_peer == peer->second.end()) {
         return missing("service_peer");
      }
      if (!allowed(service->second, limits_.service, "service") ||
          !allowed(service_peer->second, limits_.service_peer, "service_peer")) {
         return false;
      }
   }
   const auto local = scope_account{.usage = value.usage};
   if (value.value_kind == ledger::kind::connection) {
      return allowed(local, limits_.connection, "connection");
   }
   return value.value_kind != ledger::kind::stream || allowed(local, limits_.stream, "stream");
}

bool resource_manager::state::can_remove_from_current_scopes_locked(const ledger& value,
                                                                    const scope_totals& delta) const noexcept {
   if (!can_remove_locked(system_, delta) || (value.transient && !can_remove_locked(transient_, delta))) {
      return false;
   }
   if (value.peer) {
      const auto peer = peers_.find(*value.peer);
      if (peer == peers_.end() || !can_remove_locked(peer->second, delta)) {
         return false;
      }
   }
   if (value.protocol) {
      const auto protocol = protocols_.find(value.protocol->value);
      const auto peer = protocol_peers_.find(*value.peer);
      if (protocol == protocols_.end() || peer == protocol_peers_.end()) {
         return false;
      }
      const auto protocol_peer = peer->second.find(value.protocol->value);
      if (protocol_peer == peer->second.end() || !can_remove_locked(protocol->second, delta) ||
          !can_remove_locked(protocol_peer->second, delta)) {
         return false;
      }
   }
   if (value.service) {
      const auto service = services_.find(*value.service);
      const auto peer = service_peers_.find(*value.peer);
      if (service == services_.end() || peer == service_peers_.end()) {
         return false;
      }
      const auto service_peer = peer->second.find(*value.service);
      if (service_peer == peer->second.end() || !can_remove_locked(service->second, delta) ||
          !can_remove_locked(service_peer->second, delta)) {
         return false;
      }
   }
   if (value.value_kind == ledger::kind::connection && !can_remove_locked(connections_, delta)) {
      return false;
   }
   return value.value_kind != ledger::kind::stream || can_remove_locked(streams_, delta);
}

void resource_manager::state::add_locked(scope_account& account, const scope_totals& delta) noexcept {
   account.usage.memory += delta.memory;
   account.usage.file_descriptors += delta.file_descriptors;
   account.usage.inbound_connections += delta.inbound_connections;
   account.usage.outbound_connections += delta.outbound_connections;
   account.usage.inbound_streams += delta.inbound_streams;
   account.usage.outbound_streams += delta.outbound_streams;
}

void resource_manager::state::remove_locked(scope_account& account, const scope_totals& delta) noexcept {
   account.usage.memory -= delta.memory;
   account.usage.file_descriptors -= delta.file_descriptors;
   account.usage.inbound_connections -= delta.inbound_connections;
   account.usage.outbound_connections -= delta.outbound_connections;
   account.usage.inbound_streams -= delta.inbound_streams;
   account.usage.outbound_streams -= delta.outbound_streams;
}

void resource_manager::state::add_to_current_scopes_locked(const ledger& value, const scope_totals& delta) noexcept {
   add_locked(system_, delta);
   if (value.transient) {
      add_locked(transient_, delta);
   }
   if (value.peer) {
      add_locked(peers_.find(*value.peer)->second, delta);
   }
   if (value.protocol) {
      add_locked(protocols_.find(value.protocol->value)->second, delta);
      add_locked(protocol_peers_.find(*value.peer)->second.find(value.protocol->value)->second, delta);
   }
   if (value.service) {
      add_locked(services_.find(*value.service)->second, delta);
      add_locked(service_peers_.find(*value.peer)->second.find(*value.service)->second, delta);
   }
   if (value.value_kind == ledger::kind::connection) {
      add_locked(connections_, delta);
   } else if (value.value_kind == ledger::kind::stream) {
      add_locked(streams_, delta);
   }
}

void resource_manager::state::remove_from_current_scopes_locked(const ledger& value,
                                                                const scope_totals& delta) noexcept {
   remove_locked(system_, delta);
   if (value.transient) {
      remove_locked(transient_, delta);
   }
   if (value.peer) {
      remove_locked(peers_.find(*value.peer)->second, delta);
   }
   if (value.protocol) {
      remove_locked(protocols_.find(value.protocol->value)->second, delta);
      remove_locked(protocol_peers_.find(*value.peer)->second.find(value.protocol->value)->second, delta);
   }
   if (value.service) {
      remove_locked(services_.find(*value.service)->second, delta);
      remove_locked(service_peers_.find(*value.peer)->second.find(*value.service)->second, delta);
   }
   if (value.value_kind == ledger::kind::connection) {
      remove_locked(connections_, delta);
   } else if (value.value_kind == ledger::kind::stream) {
      remove_locked(streams_, delta);
   }
}

void resource_manager::state::cleanup_scopes_locked(const ledger& value) noexcept {
   if (value.protocol) {
      if (const auto protocol = protocols_.find(value.protocol->value);
          protocol != protocols_.end() && empty(protocol->second.usage)) {
         protocols_.erase(protocol);
      }
      if (const auto peer = protocol_peers_.find(*value.peer); peer != protocol_peers_.end()) {
         if (const auto protocol = peer->second.find(value.protocol->value);
             protocol != peer->second.end() && empty(protocol->second.usage)) {
            peer->second.erase(protocol);
         }
         if (peer->second.empty()) {
            protocol_peers_.erase(peer);
         }
      }
   }
   if (value.service) {
      if (const auto service = services_.find(*value.service);
          service != services_.end() && empty(service->second.usage)) {
         services_.erase(service);
      }
      if (const auto peer = service_peers_.find(*value.peer); peer != service_peers_.end()) {
         if (const auto service = peer->second.find(*value.service);
             service != peer->second.end() && empty(service->second.usage)) {
            peer->second.erase(service);
         }
         if (peer->second.empty()) {
            service_peers_.erase(peer);
         }
      }
   }
   if (value.peer) {
      if (const auto peer = peers_.find(*value.peer); peer != peers_.end() && empty(peer->second.usage)) {
         peers_.erase(peer);
      }
   }
}

resource_manager::state::ledger_attempt resource_manager::state::reserve_lifecycle() noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (lifecycle_reserve_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
   return make_ledger_locked();
}

resource_manager::state::ledger_attempt
resource_manager::state::reserve_session(session_direction direction) noexcept {
   auto lock = std::scoped_lock{mutex_};
   const auto delta = connection_delta(direction);
   const auto local = scope_account{};
   if (!can_add_locked(system_, limits_.system, delta, memory_priority::always) ||
       !can_add_locked(transient_, limits_.transient, delta, memory_priority::always) ||
       !can_add_locked(local, limits_.connection, delta, memory_priority::always)) {
      static_cast<void>(reject_limit_locked(snapshot_.denied_connections));
      return {.outcome = transition_result::policy_rejected};
   }
   if (session_reserve_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
   auto result = make_ledger_locked();
   if (!result.reservation) {
      return result;
   }
   result.reservation->value_kind = ledger::kind::connection;
   result.reservation->direction = direction;
   result.reservation->usage = delta;
   result.reservation->transient = true;
   add_to_current_scopes_locked(*result.reservation, delta);
   return result;
}

resource_manager::state::ledger_attempt
resource_manager::state::reserve_stream(peer_id peer, session_direction direction) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (peer.value.empty()) {
      static_cast<void>(reject_invalid_transition_locked());
      return {.outcome = transition_result::invalid_transition};
   }
   if (stream_reserve_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
   const auto delta = stream_delta(direction);
   const auto local = scope_account{};
   auto peer_scope = peers_.end();
   auto peer_inserted = false;
   try {
      std::tie(peer_scope, peer_inserted) = peers_.try_emplace(peer);
      if (!can_add_locked(system_, limits_.system, delta, memory_priority::always) ||
          !can_add_locked(transient_, limits_.transient, delta, memory_priority::always) ||
          !can_add_locked(peer_scope->second, limits_.peer, delta, memory_priority::always) ||
          !can_add_locked(local, limits_.stream, delta, memory_priority::always)) {
         if (peer_inserted && empty(peer_scope->second.usage)) {
            peers_.erase(peer_scope);
         }
         static_cast<void>(reject_limit_locked(snapshot_.denied_streams));
         return {.outcome = transition_result::policy_rejected};
      }
      auto result = make_ledger_locked();
      if (!result.reservation) {
         if (peer_inserted && empty(peer_scope->second.usage)) {
            peers_.erase(peer_scope);
         }
         return result;
      }
      result.reservation->value_kind = ledger::kind::stream;
      result.reservation->direction = direction;
      result.reservation->usage = delta;
      result.reservation->peer = std::move(peer);
      result.reservation->transient = true;
      add_to_current_scopes_locked(*result.reservation, delta);
      return result;
   } catch (...) {
      if (peer_inserted && peer_scope != peers_.end() && empty(peer_scope->second.usage)) {
         peers_.erase(peer_scope);
      }
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
}

resource_manager::state::dial_attempt resource_manager::state::reserve_dial() noexcept {
   auto lock = std::scoped_lock{mutex_};
   return reserve_dial_locked(std::nullopt);
}

resource_manager::state::dial_attempt resource_manager::state::reserve_dial(peer_id peer) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (peer.value.empty()) {
      static_cast<void>(reject_invalid_transition_locked());
      return {.outcome = transition_result::invalid_transition};
   }
   return reserve_dial_locked(std::optional<peer_id>{std::move(peer)});
}

resource_manager::state::dial_attempt
resource_manager::state::reserve_dial_locked(std::optional<peer_id> peer) noexcept {
   if (snapshot_.active_dials >= limits_.max_dial_attempts) {
      static_cast<void>(reject_limit_locked(snapshot_.denied_dials));
      return {.outcome = transition_result::policy_rejected};
   }
   if (dial_reserve_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
   if (peer) {
      const auto found = dial_attempts_by_peer_.find(*peer);
      const auto attempts = found == dial_attempts_by_peer_.end() ? 0 : found->second;
      if (attempts >= limits_.max_dial_attempts_per_peer) {
         static_cast<void>(reject_limit_locked(snapshot_.denied_dials));
         return {.outcome = transition_result::policy_rejected};
      }
      if (dial_bind_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
         record_runtime_failure_locked();
         return {.outcome = transition_result::runtime_failure};
      }
   }
   try {
      auto result = std::make_shared<dial_ledger>();
      auto peer_scope = dial_attempts_by_peer_.end();
      if (peer) {
         result->peer = std::move(peer);
         peer_scope = dial_attempts_by_peer_.try_emplace(*result->peer).first;
      }
      ++snapshot_.active_dials;
      if (peer_scope != dial_attempts_by_peer_.end()) {
         ++peer_scope->second;
      }
      return {.reservation = std::move(result), .outcome = transition_result::accepted};
   } catch (...) {
      record_runtime_failure_locked();
      return {.outcome = transition_result::runtime_failure};
   }
}

bool resource_manager::state::dial_active(const std::shared_ptr<dial_ledger>& value) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return value && value->active;
}

bool resource_manager::state::dial_bound(const std::shared_ptr<dial_ledger>& value) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return value && value->active && value->peer.has_value();
}

resource_manager::transition_result resource_manager::state::bind_dial(const std::shared_ptr<dial_ledger>& value,
                                                                        peer_id peer) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!value || !value->active || value->peer || peer.value.empty()) {
      static_cast<void>(reject_invalid_transition_locked());
      return transition_result::invalid_transition;
   }
   const auto found = dial_attempts_by_peer_.find(peer);
   const auto attempts = found == dial_attempts_by_peer_.end() ? 0 : found->second;
   if (attempts >= limits_.max_dial_attempts_per_peer) {
      static_cast<void>(reject_limit_locked(snapshot_.denied_dials));
      return transition_result::policy_rejected;
   }
   try {
      if (dial_bind_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
         throw std::bad_alloc{};
      }
      auto next_peer = std::optional<peer_id>{};
      next_peer.emplace(std::move(peer));
      auto [entry, inserted] = dial_attempts_by_peer_.try_emplace(*next_peer);
      static_cast<void>(inserted);
      ++entry->second;
      value->peer.swap(next_peer);
      return transition_result::accepted;
   } catch (...) {
      record_runtime_failure_locked();
      return transition_result::runtime_failure;
   }
}

void resource_manager::state::release_dial(const std::shared_ptr<dial_ledger>& value) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!value || !value->active) {
      return;
   }
   value->active = false;
   if (snapshot_.active_dials > 0) {
      --snapshot_.active_dials;
   }
   if (!value->peer) {
      return;
   }
   if (const auto found = dial_attempts_by_peer_.find(*value->peer); found != dial_attempts_by_peer_.end()) {
      if (found->second > 1) {
         --found->second;
      } else {
         dial_attempts_by_peer_.erase(found);
      }
   }
   value->peer.reset();
}

resource_manager::transition_result resource_manager::state::reserve_relay(const peer_id& peer) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (peer.value.empty()) {
      static_cast<void>(reject_invalid_transition_locked());
      return transition_result::invalid_transition;
   }
   if (snapshot_.active_relay_reservations >= limits_.max_relay_reservations) {
      static_cast<void>(reject_limit_locked(snapshot_.denied_relays));
      return transition_result::policy_rejected;
   }
   if (relay_reserve_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
      record_runtime_failure_locked();
      return transition_result::runtime_failure;
   }
   try {
      auto [entry, inserted] = relay_reservations_by_peer_.try_emplace(peer);
      static_cast<void>(inserted);
      ++entry->second;
      ++snapshot_.active_relay_reservations;
      return transition_result::accepted;
   } catch (...) {
      record_runtime_failure_locked();
      return transition_result::runtime_failure;
   }
}

void resource_manager::state::release_relay(const peer_id& peer) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (snapshot_.active_relay_reservations > 0) {
      --snapshot_.active_relay_reservations;
   }
   if (const auto found = relay_reservations_by_peer_.find(peer); found != relay_reservations_by_peer_.end()) {
      if (found->second > 1) {
         --found->second;
      } else {
         relay_reservations_by_peer_.erase(found);
      }
   }
}

resource_manager::transition_result resource_manager::state::record_malformed(const peer_id& peer) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (peer.value.empty()) {
      static_cast<void>(reject_invalid_transition_locked());
      return transition_result::invalid_transition;
   }
   const auto found = malformed_by_peer_.find(peer);
   const auto messages = found == malformed_by_peer_.end() ? 0 : found->second;
   if (messages >= limits_.max_malformed_messages_per_peer) {
      static_cast<void>(reject_limit_locked(snapshot_.denied_malformed));
      return transition_result::policy_rejected;
   }
   try {
      if (malformed_record_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
         throw std::bad_alloc{};
      }
      auto [entry, inserted] = malformed_by_peer_.try_emplace(peer);
      static_cast<void>(inserted);
      ++entry->second;
      return transition_result::accepted;
   } catch (...) {
      record_runtime_failure_locked();
      return transition_result::runtime_failure;
   }
}

bool resource_manager::state::session_established(const std::shared_ptr<ledger>& value) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return value && value->parent_active && value->value_kind == ledger::kind::connection && value->peer.has_value();
}

bool resource_manager::state::stream_bound(const std::shared_ptr<ledger>& value) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return value && value->parent_active && value->value_kind == ledger::kind::stream && value->protocol.has_value();
}

bool resource_manager::state::stream_service_bound(const std::shared_ptr<ledger>& value) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return value && value->parent_active && value->value_kind == ledger::kind::stream && value->service.has_value();
}

resource_manager::transition_result resource_manager::state::establish_session(const std::shared_ptr<ledger>& value,
                                                                                session_scope scope) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!value || !value->parent_active || value->value_kind != ledger::kind::connection || value->peer ||
       !value->transient || scope.peer.value.empty() || scope.direction != value->direction) {
      static_cast<void>(reject_invalid_transition_locked());
      return transition_result::invalid_transition;
   }
   auto peer = peers_.end();
   auto peer_inserted = false;
   try {
      if (session_establish_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
         throw std::bad_alloc{};
      }
      auto next_peer = std::optional<peer_id>{};
      next_peer.emplace(std::move(scope.peer));
      std::tie(peer, peer_inserted) = peers_.try_emplace(*next_peer);
      if (!can_add_locked(peer->second, limits_.peer, value->usage, memory_priority::always)) {
         if (peer_inserted && empty(peer->second.usage)) {
            peers_.erase(peer);
         }
         static_cast<void>(reject_limit_locked(snapshot_.denied_scope_migrations));
         return transition_result::policy_rejected;
      }
      remove_locked(transient_, value->usage);
      add_locked(peer->second, value->usage);
      value->peer.swap(next_peer);
      value->transient = false;
      return transition_result::accepted;
   } catch (...) {
      if (peer_inserted && peer != peers_.end() && empty(peer->second.usage)) {
         peers_.erase(peer);
      }
      record_runtime_failure_locked();
      return transition_result::runtime_failure;
   }
}

resource_manager::transition_result
resource_manager::state::bind_protocol(const std::shared_ptr<ledger>& value, const protocol_id& protocol) noexcept {
   using transition_result = resource_manager::transition_result;
   auto lock = std::scoped_lock{mutex_};
   if (!value || !value->parent_active || value->value_kind != ledger::kind::stream || !value->peer ||
       value->protocol || !value->transient || protocol.value.empty()) {
      static_cast<void>(reject_invalid_transition_locked());
      return transition_result::invalid_transition;
   }
   auto protocol_scope = protocols_.end();
   auto protocol_peer = protocol_peers_.end();
   auto protocol_peer_scope = std::map<std::string, scope_account>::iterator{};
   auto protocol_inserted = false;
   auto protocol_peer_inserted = false;
   auto protocol_peer_scope_inserted = false;
   try {
      auto next_protocol = std::optional<protocol_id>{};
      next_protocol.emplace(protocol);
      std::tie(protocol_scope, protocol_inserted) = protocols_.try_emplace(next_protocol->value);
      std::tie(protocol_peer, protocol_peer_inserted) = protocol_peers_.try_emplace(*value->peer);
      std::tie(protocol_peer_scope, protocol_peer_scope_inserted) =
          protocol_peer->second.try_emplace(next_protocol->value);
      if (!can_add_locked(protocol_scope->second, limits_.protocol, value->usage, memory_priority::always) ||
          !can_add_locked(protocol_peer_scope->second, limits_.protocol_peer, value->usage, memory_priority::always)) {
         if (protocol_peer_scope_inserted) {
            protocol_peer->second.erase(protocol_peer_scope);
         }
         if (protocol_peer_inserted && protocol_peer->second.empty()) {
            protocol_peers_.erase(protocol_peer);
         }
         if (protocol_inserted && empty(protocol_scope->second.usage)) {
            protocols_.erase(protocol_scope);
         }
         static_cast<void>(reject_limit_locked(snapshot_.denied_scope_migrations));
         return transition_result::policy_rejected;
      }
      remove_locked(transient_, value->usage);
      add_locked(protocol_scope->second, value->usage);
      add_locked(protocol_peer_scope->second, value->usage);
      value->protocol.swap(next_protocol);
      value->transient = false;
      return transition_result::accepted;
   } catch (...) {
      if (protocol_peer_scope_inserted && protocol_peer != protocol_peers_.end()) {
         protocol_peer->second.erase(protocol_peer_scope);
      }
      if (protocol_peer_inserted && protocol_peer != protocol_peers_.end() && protocol_peer->second.empty()) {
         protocol_peers_.erase(protocol_peer);
      }
      if (protocol_inserted && protocol_scope != protocols_.end() && empty(protocol_scope->second.usage)) {
         protocols_.erase(protocol_scope);
      }
      record_runtime_failure_locked();
      return transition_result::runtime_failure;
   }
}

resource_manager::transition_result
resource_manager::state::bind_service(const std::shared_ptr<ledger>& value, std::string_view service) noexcept {
   auto lock = std::scoped_lock{mutex_};
   return bind_service_locked(value, service);
}

resource_manager::transition_result
resource_manager::state::bind_service_locked(const std::shared_ptr<ledger>& value, std::string_view service) noexcept {
   using transition_result = resource_manager::transition_result;
   if (!value || !value->parent_active || value->value_kind != ledger::kind::stream || !value->peer ||
       !value->protocol || value->service || service.empty()) {
      static_cast<void>(reject_invalid_transition_locked());
      return transition_result::invalid_transition;
   }
   auto service_scope = services_.end();
   auto service_peer = service_peers_.end();
   auto service_peer_scope = std::map<std::string, scope_account>::iterator{};
   auto service_inserted = false;
   auto service_peer_inserted = false;
   auto service_peer_scope_inserted = false;
   try {
      if (service_bind_prepare_failpoint.exchange(false, std::memory_order_relaxed)) {
         throw std::bad_alloc{};
      }
      auto next_service = std::optional<std::string>{};
      next_service.emplace(service);
      std::tie(service_scope, service_inserted) = services_.try_emplace(*next_service);
      std::tie(service_peer, service_peer_inserted) = service_peers_.try_emplace(*value->peer);
      std::tie(service_peer_scope, service_peer_scope_inserted) = service_peer->second.try_emplace(*next_service);
      if (!can_add_locked(service_scope->second, limits_.service, value->usage, memory_priority::always) ||
          !can_add_locked(service_peer_scope->second, limits_.service_peer, value->usage, memory_priority::always)) {
         if (service_peer_scope_inserted) {
            service_peer->second.erase(service_peer_scope);
         }
         if (service_peer_inserted && service_peer->second.empty()) {
            service_peers_.erase(service_peer);
         }
         if (service_inserted && empty(service_scope->second.usage)) {
            services_.erase(service_scope);
         }
         static_cast<void>(reject_limit_locked(snapshot_.denied_scope_migrations));
         return transition_result::policy_rejected;
      }
      add_locked(service_scope->second, value->usage);
      add_locked(service_peer_scope->second, value->usage);
      value->service.swap(next_service);
      return transition_result::accepted;
   } catch (...) {
      if (service_peer_scope_inserted && service_peer != service_peers_.end()) {
         service_peer->second.erase(service_peer_scope);
      }
      if (service_peer_inserted && service_peer != service_peers_.end() && service_peer->second.empty()) {
         service_peers_.erase(service_peer);
      }
      if (service_inserted && service_scope != services_.end() && empty(service_scope->second.usage)) {
         services_.erase(service_scope);
      }
      record_runtime_failure_locked();
      return transition_result::runtime_failure;
   }
}

bool resource_manager::state::reserve_memory(const std::shared_ptr<ledger>& value, std::uint64_t bytes,
                                             memory_priority priority) noexcept {
   auto rejected = memory_rejection{};
   auto reservation_memory = std::uint64_t{};
   auto system_memory = std::uint64_t{};
   auto denied_before = std::uint64_t{};
   auto denied_after = std::uint64_t{};
   auto invalid_before = std::uint64_t{};
   auto invalid_after = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{mutex_};
      denied_before = snapshot_.denied_memory;
      invalid_before = snapshot_.invalid_transitions;
      if (!value || !value->parent_active) {
         rejected = {.reason = value ? "inactive_parent" : "inactive_reservation", .scope = "reservation"};
         static_cast<void>(reject_invalid_transition_locked());
      } else {
         const auto delta = scope_totals{.memory = bytes};
         if (can_add_to_current_scopes_locked(*value, delta, priority, &rejected)) {
            add_to_current_scopes_locked(*value, delta);
            value->usage.memory += bytes;
            return true;
         }
         static_cast<void>(reject_limit_locked(snapshot_.denied_memory));
      }
      reservation_memory = value ? value->usage.memory : 0;
      system_memory = system_.usage.memory;
      denied_after = snapshot_.denied_memory;
      invalid_after = snapshot_.invalid_transitions;
   }
   // Values describe this exact admission attempt under mutex_, before any
   // concurrent ACK or teardown. Sinks may safely re-enter diagnostics here.
   wlog("forge.net.p2p.resources", "P2P memory reservation rejected",
        ("event", "p2p.memory_reservation_failed")("reason", rejected.reason)("scope", rejected.scope)(
            "dimension", rejected.dimension)("requested_bytes", bytes)("priority", static_cast<unsigned>(priority))(
            "scope_usage", rejected.usage)("scope_limit", rejected.limit)("scope_threshold", rejected.threshold)(
            "reservation_memory", reservation_memory)("system_memory", system_memory)(
            "denied_memory_before", denied_before)("denied_memory_after", denied_after)(
            "invalid_transitions_before", invalid_before)("invalid_transitions_after", invalid_after));
   return false;
}

bool resource_manager::state::reserve_file_descriptors(const std::shared_ptr<ledger>& value,
                                                       std::size_t count) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!value || !value->parent_active) {
      return reject_invalid_transition_locked();
   }
   const auto delta = scope_totals{.file_descriptors = count};
   if (!can_add_to_current_scopes_locked(*value, delta, memory_priority::always)) {
      return reject_limit_locked(snapshot_.denied_file_descriptors);
   }
   add_to_current_scopes_locked(*value, delta);
   value->usage.file_descriptors += count;
   return true;
}

void resource_manager::state::release_memory(const std::shared_ptr<ledger>& value, std::uint64_t bytes) noexcept {
   auto lock = std::scoped_lock{mutex_};
   const auto delta = scope_totals{.memory = bytes};
   if (!value || !can_remove_from_current_scopes_locked(*value, delta)) {
      return;
   }
   remove_from_current_scopes_locked(*value, delta);
   value->usage.memory -= bytes;
   cleanup_scopes_locked(*value);
}

void resource_manager::state::release_file_descriptors(const std::shared_ptr<ledger>& value,
                                                       std::size_t count) noexcept {
   auto lock = std::scoped_lock{mutex_};
   const auto delta = scope_totals{.file_descriptors = count};
   if (!value || !can_remove_from_current_scopes_locked(*value, delta)) {
      return;
   }
   remove_from_current_scopes_locked(*value, delta);
   value->usage.file_descriptors -= count;
   cleanup_scopes_locked(*value);
}

void resource_manager::state::release_parent(const std::shared_ptr<ledger>& value) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!value || !value->parent_active) {
      return;
   }
   if (value->value_kind == ledger::kind::lifecycle) {
      value->parent_active = false;
      return;
   }
   const auto delta = value->value_kind == ledger::kind::connection ? connection_delta(value->direction)
                                                                    : stream_delta(value->direction);
   if (!can_remove_from_current_scopes_locked(*value, delta)) {
      return;
   }
   remove_from_current_scopes_locked(*value, delta);
   value->usage.inbound_connections -= delta.inbound_connections;
   value->usage.outbound_connections -= delta.outbound_connections;
   value->usage.inbound_streams -= delta.inbound_streams;
   value->usage.outbound_streams -= delta.outbound_streams;
   value->parent_active = false;
   cleanup_scopes_locked(*value);
}

namespace detail {

void fail_next_service_bind_prepare_for_test() noexcept {
   service_bind_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_session_establish_prepare_for_test() noexcept {
   session_establish_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_dial_bind_prepare_for_test() noexcept {
   dial_bind_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_malformed_record_prepare_for_test() noexcept {
   malformed_record_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_lifecycle_reserve_prepare_for_test() noexcept {
   lifecycle_reserve_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_session_reserve_prepare_for_test() noexcept {
   session_reserve_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_dial_reserve_prepare_for_test() noexcept {
   dial_reserve_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_stream_reserve_prepare_for_test() noexcept {
   stream_reserve_prepare_failpoint.store(true, std::memory_order_relaxed);
}

void fail_next_relay_reserve_prepare_for_test() noexcept {
   relay_reserve_prepare_failpoint.store(true, std::memory_order_relaxed);
}

} // namespace detail

} // namespace forge::net::p2p
