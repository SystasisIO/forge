module;

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.swarm.runtime.engine;

import forge.asio.notification;

#include "details/resource_budget.hxx"

namespace forge::swarm::runtime::detail {

resource_budget::resource_budget(resource_limits global, resource_limits operation, resource_limits peer)
   : _limits{global}, _operation_limits{operation}, _peer_limits{peer} {}
bool resource_budget::fits(resource_limits used, resource_limits amount, resource_limits limit) {
   return used.calls <= limit.calls && amount.calls <= limit.calls - used.calls &&
          used.items <= limit.items && amount.items <= limit.items - used.items &&
          used.bytes <= limit.bytes && amount.bytes <= limit.bytes - used.bytes &&
          used.pieces <= limit.pieces && amount.pieces <= limit.pieces - used.pieces;
}
void resource_budget::add(resource_limits& target, resource_limits value) {
   target.calls += value.calls; target.items += value.items; target.bytes += value.bytes; target.pieces += value.pieces;
}
void resource_budget::subtract(resource_limits& target, resource_limits value) {
   target.calls -= value.calls; target.items -= value.items; target.bytes -= value.bytes; target.pieces -= value.pieces;
}
resource_budget::ticket resource_budget::reserve(std::uint64_t operation, std::string peer, resource_limits amount) {
   auto result = try_reserve(operation, std::move(peer), amount);
   if (!result) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm resource reservation exhausted");
   }
   return std::move(*result);
}
void resource_budget::validate_capacity(resource_limits amount) const {
   const auto zero = resource_limits{0, 0, 0, 0};
   if (!fits(zero, amount, _limits) || !fits(zero, amount, _operation_limits) || !fits(zero, amount, _peer_limits)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm work exceeds configured resource capacity");
   }
}
bool resource_budget::available_locked(std::uint64_t operation, const std::string& peer, resource_limits amount) const {
   const auto op = _operations.contains(operation) ? _operations.at(operation) : resource_limits{0, 0, 0, 0};
   const auto remote = _peers.contains(peer) ? _peers.at(peer) : resource_limits{0, 0, 0, 0};
   return fits(_used, amount, _limits) && fits(op, amount, _operation_limits) && fits(remote, amount, _peer_limits);
}
bool resource_budget::available(std::uint64_t operation, const std::string& peer, resource_limits amount) const {
   auto lock = std::scoped_lock{_mutex};
   validate_capacity(amount);
   return available_locked(operation, peer, amount);
}
std::optional<resource_budget::ticket> resource_budget::try_reserve(
   std::uint64_t operation, std::string peer, resource_limits amount) {
   auto lock = std::scoped_lock{_mutex};
   validate_capacity(amount);
   if (!available_locked(operation, peer, amount)) return std::nullopt;
   // Allocate map entries before committing counters.
   auto [oi, inserted] = _operations.try_emplace(operation, resource_limits{0, 0, 0, 0});
   try { _peers.try_emplace(peer, resource_limits{0, 0, 0, 0}); }
   catch (...) { if (inserted) _operations.erase(oi); throw; }
   add(_used, amount); add(_operations.at(operation), amount); add(_peers.at(peer), amount);
   return ticket{shared_from_this(), operation, std::move(peer), amount};
}
void resource_budget::release(std::uint64_t operation, const std::string& peer, resource_limits amount) noexcept {
   {
      auto lock = std::scoped_lock{_mutex};
      subtract(_used, amount); subtract(_operations.at(operation), amount); subtract(_peers.at(peer), amount);
      const auto empty = [](resource_limits value) { return !value.calls && !value.items && !value.bytes && !value.pieces; };
      if (empty(_operations.at(operation))) _operations.erase(operation);
      if (empty(_peers.at(peer))) _peers.erase(peer);
   }
   changed.notify();
}
resource_budget::ticket::ticket() = default;
resource_budget::ticket::ticket(std::shared_ptr<resource_budget> owner, std::uint64_t operation,
   std::string peer, resource_limits amount) : _owner{std::move(owner)}, _operation{operation}, _peer{std::move(peer)}, _amount{amount} {}
resource_budget::ticket::~ticket() { release(); }
resource_budget::ticket::ticket(ticket&& other) noexcept
   : _owner{std::exchange(other._owner, {})}, _operation{other._operation}, _peer{std::move(other._peer)}, _amount{other._amount} {}
resource_budget::ticket& resource_budget::ticket::operator=(ticket&& other) noexcept {
   if (this != &other) {
      release(); _owner = std::exchange(other._owner, {}); _operation = other._operation;
      _peer = std::move(other._peer); _amount = other._amount;
   }
   return *this;
}
void resource_budget::ticket::release() noexcept {
   if (auto owner = std::exchange(_owner, {})) owner->release(_operation, _peer, _amount);
}
resource_budget::ticket resource_budget::ticket::split(resource_limits amount) {
   if (!_owner || !fits({0, 0, 0, 0}, amount, _amount)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm ticket split exceeds reservation");
   }
   // Copy the peer key before changing ownership; allocation failure leaves
   // the original reservation intact. Splitting does not free or notify capacity.
   auto result = ticket{_owner, _operation, _peer, amount};
   subtract(_amount, amount);
   return result;
}
void resource_budget::ticket::rebind(std::string peer) {
   if (!_owner || peer == _peer) return;
   {
      auto lock = std::scoped_lock{_owner->_mutex};
      const auto destination = _owner->_peers.contains(peer) ? _owner->_peers.at(peer) : resource_limits{0, 0, 0, 0};
      if (!fits(destination, _amount, _owner->_peer_limits)) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "Swarm authenticated remote peer limit exhausted");
      }
      auto [target, inserted] = _owner->_peers.try_emplace(peer, destination);
      add(target->second, _amount);
      auto old = _owner->_peers.find(_peer);
      subtract(old->second, _amount);
      if (!old->second.calls && !old->second.items && !old->second.bytes && !old->second.pieces)
         _owner->_peers.erase(old);
      _peer = std::move(peer);
   }
   _owner->changed.notify();
}

} // namespace forge::swarm::runtime::detail
