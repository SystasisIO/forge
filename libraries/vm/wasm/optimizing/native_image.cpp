module;

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.vm.wasm.optimizing.artifact;

import forge.vm.wasm.optimizing.exceptions;

#include "details/native_image.hxx"

namespace forge::vm::wasm::optimizing {

native_image::native_image(const artifact_payload& payload, const interpret::prepared_module::preparation& allocation)
    : _allocation(allocation),
      _functions(interpret::prepared_module::preparation::allocator<native_function>{allocation}) {
#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION != 220108
   FORGE_THROW_EXCEPTION(exceptions::compiler_unavailable,
                         "Bounded AOT binding projection requires the reviewed libc++ allocation contract");
#endif
   // These default-allocator members are copied once, without growth. The
   // reviewed libc++ copy constructors allocate <= source capacity + NUL for
   // strings and exactly size() elements for vectors. Reserve before copying.
   auto projection_bytes = std::uint64_t{};
   for (const auto& source : payload.configuration.imports) {
      const auto bytes = source.module.capacity() + 1 + source.name.capacity() + 1 +
                         (source.signature.params.size() + source.signature.ret.size()) * sizeof(interpret::value_type);
      if (bytes > UINT64_MAX - projection_bytes)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT binding projection size overflow");
      projection_bytes += bytes;
   }
   allocation.reserve(projection_bytes);
   struct projection_reservation {
      const interpret::prepared_module::preparation& owner;
      std::uint64_t bytes;
      ~projection_reservation() {
         owner.release(bytes);
      }
   } projection{allocation, projection_bytes};
   auto bindings = std::vector<interpret::host_binding,
                               interpret::prepared_module::preparation::allocator<interpret::host_binding>>{
       payload.configuration.imports.size(),
       interpret::prepared_module::preparation::allocator<interpret::host_binding>{allocation}};
   for (std::uint32_t i = 0; i < payload.configuration.imports.size(); ++i) {
      const auto& source = payload.configuration.imports[i];
      const auto callback = [i](void* host, interpret::execution_interface interface) {
         if (!host)
            FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT invocation host dispatcher is missing");
         using dispatch = std::function<void(std::uint32_t, interpret::execution_interface)>;
         (*static_cast<dispatch*>(host))(i, std::move(interface));
      };
#if defined(_LIBCPP_VERSION) && _LIBCPP_VERSION == 220108
#if defined(_LIBCPP_ABI_OPTIMIZED_FUNCTION)
      static_assert(std::__function::__use_small_storage<decltype(callback)>::value);
#else
      static_assert(sizeof(std::__function::__func<decltype(callback), void(void*, interpret::execution_interface)>) <=
                    3 * sizeof(void*));
      static_assert(std::is_nothrow_copy_constructible_v<decltype(callback)>);
#endif
#endif
      bindings[i] = {source.module, source.name, source.signature, callback};
   }
   _module = std::allocate_shared<interpret::prepared_module>(
       interpret::prepared_module::preparation::allocator<interpret::prepared_module>{allocation}, payload.source,
       payload.aot, payload.configuration.execution, std::span{bindings},
       interpret::prepared_module::native_origin::trusted_compiler, allocation);
   if (_module->function_count() != payload.functions.size())
      FORGE_THROW_EXCEPTION(exceptions::invalid_artifact,
                            "AOT native function count does not match its source metadata");
   _functions.resize(payload.functions.size());
   for (std::uint32_t i = 0; i < payload.functions.size(); ++i) {
      const auto frame = _module->native_function_stack_bytes(i);
      if (frame > payload.configuration.limits.max_function_stack_bytes)
         FORGE_THROW_EXCEPTION(exceptions::compile_limit, "AOT native function exceeds the physical frame budget");
      const auto& source = payload.functions[i];
      const auto type = _module->function_signature(i);
      const auto& expected = payload.types[source.type_index];
      if (!std::ranges::equal(type.params, expected.params) || !std::ranges::equal(type.results, expected.ret))
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact,
                               "AOT function type differs from checked artifact metadata");
      _functions[i] = {frame, source.type_index};
   }
   for (const auto& exported : payload.exports)
      if (_module->get_exported_function(exported.name) != exported.function_index)
         FORGE_THROW_EXCEPTION(exceptions::invalid_artifact, "AOT export differs from checked artifact metadata");
}

void native_image::seal() {
   _bytes = _allocation.seal();
}

std::span<const std::byte> native_image::code() const noexcept {
   return _module->native_code();
}
std::span<const native_function> native_image::functions() const noexcept {
   return _functions;
}
std::uint64_t native_image::byte_size() const noexcept {
   return _bytes;
}
std::shared_ptr<interpret::prepared_module> native_image::runtime() const noexcept {
   return _module;
}

} // namespace forge::vm::wasm::optimizing
