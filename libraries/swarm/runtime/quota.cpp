module;

#include <chrono>
#include <algorithm>
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <stop_token>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.swarm.runtime.quota;

#include "details/quota_impl.hxx"

namespace forge::swarm::runtime {

quota::quota(options value) : _impl{std::make_shared<impl>(std::move(value))} {}
quota::~quota() = default;

bool quota::permits(core::swarm_id content, const forge::api::auth::authenticated_caller& caller) const {
   auto lock = std::scoped_lock{_impl->mutex};
   return !_impl->state.revoked && std::chrono::steady_clock::now() < _impl->config.expires &&
          content == _impl->config.content && caller == _impl->config.requester && caller.transport_authenticated();
}

quota::reservation quota::reserve(std::uint64_t bytes) {
   auto lock = std::scoped_lock{_impl->mutex};
   auto& requested = _impl->state.traffic.requested;
   requested += std::min(bytes, std::numeric_limits<std::uint64_t>::max() - requested);
   if (_impl->state.revoked || std::chrono::steady_clock::now() >= _impl->config.expires ||
       bytes > _impl->state.available) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm grant expired, revoked or exhausted");
   }
   _impl->state.available -= bytes;
   _impl->state.held += bytes;
   return reservation{_impl, bytes};
}

quota::status quota::inspect() const {
   auto lock = std::scoped_lock{_impl->mutex};
   return _impl->state;
}
std::stop_token quota::stop_token() const noexcept { return _impl->stop.get_token(); }
std::chrono::steady_clock::time_point quota::expires() const noexcept { return _impl->config.expires; }
void quota::revoke() noexcept {
   {
      auto lock = std::scoped_lock{_impl->mutex};
      _impl->state.revoked = true;
   }
   // Synchronous stop callbacks may enter runtime/transport, never under quota lock.
   _impl->stop.request_stop();
}

quota::reservation::reservation() = default;
quota::reservation::reservation(std::shared_ptr<impl> owner, std::uint64_t bytes)
   : _owner{std::move(owner)}, _bytes{bytes} {}
quota::reservation::~reservation() { finalize(); }
quota::reservation::reservation(reservation&& other) noexcept
   : _owner{std::exchange(other._owner, {})}, _bytes{other._bytes}, _attempted{other._attempted},
     _queued{other._queued}, _sent{other._sent} {}
quota::reservation& quota::reservation::operator=(reservation&& other) noexcept {
   if (this != &other) {
      finalize();
      _owner = std::exchange(other._owner, {});
      _bytes = other._bytes;
      _attempted = other._attempted;
      _queued = other._queued;
      _sent = other._sent;
   }
   return *this;
}
void quota::reservation::queue() {
   if (!_owner || _attempted) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Swarm quota reservation is not queueable");
   }
   auto lock = std::scoped_lock{_owner->mutex};
   if (_owner->state.revoked || std::chrono::steady_clock::now() >= _owner->config.expires) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "Swarm grant no longer permits output");
   }
   _attempted = true;
   _owner->state.traffic.attempted += _bytes;
}
void quota::reservation::queued() {
   if (!_owner || !_attempted || _queued) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Swarm queue fact without attempted reservation");
   }
   auto lock = std::scoped_lock{_owner->mutex};
   _queued = true;
   _owner->state.traffic.queued += _bytes;
}
void quota::reservation::sent() {
   if (!_owner || !_queued || _sent) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error, "Swarm send fact without queued reservation");
   }
   auto lock = std::scoped_lock{_owner->mutex};
   _sent = true;
   _owner->state.traffic.sent += _bytes;
}
void quota::reservation::finalize() noexcept {
   if (auto owner = std::exchange(_owner, {})) {
      auto lock = std::scoped_lock{owner->mutex};
      owner->state.held -= _bytes;
      ++owner->state.finalized;
      if (_attempted && !_queued)
         owner->state.traffic.uncertain += _bytes;
      if (!_attempted)
         owner->state.available += _bytes;
   }
}

} // namespace forge::swarm::runtime
