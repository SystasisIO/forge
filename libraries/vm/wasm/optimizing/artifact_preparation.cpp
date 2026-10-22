module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.vm.wasm.optimizing.artifact;

import forge.vm.wasm.optimizing.exceptions;

#include "details/native_image.hxx"
#include "details/artifact_impl.hxx"

namespace forge::vm::wasm::optimizing {

std::uint64_t artifact::byte_size() const noexcept {
   return _impl->owned_bytes + (is_prepared() ? _impl->native->byte_size() : 0);
}
std::uint64_t artifact::prepared_byte_size() const {
   return prepared_byte_size(default_preparation_limits());
}
artifact::preparation_limits artifact::default_preparation_limits() noexcept {
   return standalone_preparation_policy;
}
std::uint64_t artifact::prepared_byte_size(preparation_limits limits) const {
   if (!limits.resident_bytes || !limits.workspace_bytes || limits.resident_bytes > UINT64_MAX - _impl->owned_bytes)
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT preparation requires finite nonoverflowing ceilings");
   return is_prepared() ? byte_size() : _impl->owned_bytes + limits.resident_bytes;
}
void artifact::prepare() const {
   prepare(default_preparation_limits());
}
void artifact::prepare(preparation_limits limits, std::stop_token stop, std::shared_ptr<void> charge) const {
   static_cast<void>(prepared_byte_size(limits));
   if (stop.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "AOT preparation was cancelled");
   if (_impl->authority == origin::untrusted)
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "Untrusted native bytes have no compiler/cache provenance");
   if (!_impl->image.configuration.target.matches_host())
      FORGE_THROW_EXCEPTION(exceptions::unsupported_target, "AOT artifact does not match this execution host");
   const auto lock = std::scoped_lock{_impl->preparation};
   if (stop.stop_requested())
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "AOT preparation was cancelled while waiting");
   if (is_prepared()) {
      if (_impl->native->byte_size() > limits.resident_bytes)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "Prepared AOT exceeds the requested resident ceiling");
      if (charge && (charge.owner_before(_impl->lifetime->charge) || _impl->lifetime->charge.owner_before(charge)))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "Prepared AOT already belongs to another resident charge");
      return;
   }
   if (_impl->cleanup) {
      if (!_impl->cleanup->retry_cleanup())
         FORGE_THROW_EXCEPTION(exceptions::shutdown_incomplete, "Failed AOT preparation still owns native storage");
      _impl->cleanup.reset();
   }
   if (_impl->lifetime->charge) {
      if (charge && (charge.owner_before(_impl->lifetime->charge) || _impl->lifetime->charge.owner_before(charge)))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact,
                               "Failed AOT preparation belongs to another resident charge");
      if (limits.resident_bytes > _impl->charged_limits.resident_bytes ||
          limits.workspace_bytes > _impl->charged_limits.workspace_bytes)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT retry exceeds its retained reservation");
      charge = _impl->lifetime->charge;
   }
   if (charge) {
      _impl->lifetime->charge = charge;
      _impl->charged_limits = limits;
   }
   try {
      const auto allocation = interpret::prepared_module::preparation{limits, stop};
      if (charge)
         allocation.attach_resident_charge(charge);
      _impl->cleanup.emplace(allocation); // Reachable even if native construction/unmap throws.
      auto native = std::allocate_shared<native_image>(
          interpret::prepared_module::preparation::allocator<native_image>{allocation}, _impl->image, allocation);
      // Binding projections are gone; no refunded headroom remains allocatable.
      native->seal();
      if (stop.stop_requested())
         FORGE_THROW_EXCEPTION(exceptions::cancelled, "AOT preparation was cancelled before publication");
      _impl->lifetime->charge = std::move(charge);
      _impl->native = std::move(native);
      _impl->cleanup.reset();
      _impl->prepared.store(true, std::memory_order_release);
   } catch (const interpret::exceptions::canceled&) {
      FORGE_THROW_EXCEPTION(exceptions::cancelled, "AOT preparation was cancelled");
   } catch (const interpret::exceptions::execution_unavailable&) {
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT preparation resource admission failed");
   } catch (const interpret::exceptions::allocation&) {
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT preparation allocation failed");
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT preparation allocation failed");
   }
}
bool artifact::retry_cleanup() const {
   const auto lock = std::scoped_lock{_impl->preparation};
   if (!_impl->cleanup)
      return true;
   if (!_impl->cleanup->retry_cleanup())
      return false;
   _impl->cleanup.reset();
   return true;
}
bool artifact::is_prepared() const noexcept {
   return _impl->prepared.load(std::memory_order_acquire);
}
std::span<const std::byte> artifact::native_code() const {
   if (!is_prepared())
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "AOT artifact is not prepared");
   return _impl->native->code();
}
std::span<const native_function> artifact::functions() const {
   if (!is_prepared())
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "AOT artifact is not prepared");
   return _impl->native->functions();
}
std::shared_ptr<interpret::prepared_module> artifact::runtime_module() const {
   if (!is_prepared())
      FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable, "AOT artifact is not prepared");
   // The exported module pin must retain artifact storage and its resident
   // charge, not only the inner engine owner. It also prevents cache eviction.
   return {_impl, _impl->native->runtime().get()};
}

} // namespace forge::vm::wasm::optimizing
