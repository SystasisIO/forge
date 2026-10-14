module;

#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <utility>

module forge.execution.wasm.compiled_module;
import forge.vm.wasm.interpret.backend;

#include "details/intrinsics.hxx"
#include "details/compiled_module_impl.hxx"

namespace forge::execution::wasm {

compiled_module::compiled_module(std::span<const std::uint8_t> code, const compile_limits& limits)
   : _impl(std::make_unique<impl>(code, limits)) {}
compiled_module::~compiled_module() = default;
compiled_module::compiled_module(compiled_module&&) noexcept = default;
compiled_module& compiled_module::operator=(compiled_module&&) noexcept = default;

invocation_result compiled_module::invoke(const invocation& call, const invocation_limits& limits, std::stop_token stop) {
   if (!_impl) {
      throw exceptions::unusable{"Compiled module has been moved from"};
   }
   return _impl->invoke(call, limits, stop);
}

std::size_t compiled_module::bytes() const noexcept {
   return _impl ? _impl->bytes : 0;
}

} // namespace forge::execution::wasm
