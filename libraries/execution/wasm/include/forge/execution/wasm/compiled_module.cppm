module;

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>

export module forge.execution.wasm.compiled_module;
export import forge.execution.wasm.exceptions;
export import forge.execution.wasm.invocation;
export import forge.execution.wasm.limits;

export namespace forge::execution::wasm {

class compiled_module {
 public:
   explicit compiled_module(std::span<const std::uint8_t>, const compile_limits& = {});
   ~compiled_module();
   compiled_module(const compiled_module&) = delete;
   compiled_module& operator=(const compiled_module&) = delete;
   compiled_module(compiled_module&&) noexcept;
   compiled_module& operator=(compiled_module&&) noexcept;

   [[nodiscard]] invocation_result invoke(const invocation&, const invocation_limits& = {}, std::stop_token = {});
   [[nodiscard]] std::size_t bytes() const noexcept;

 private:
   struct impl;
   std::unique_ptr<impl> _impl;
};

} // namespace forge::execution::wasm
