#pragma once

namespace forge::vm::wasm::execution {

struct compiled_module::impl {
   using backend =
       forge::vm::wasm::interpret::backend<forge::vm::wasm::interpret::registered_host_functions<intrinsics>,
                                           forge::vm::wasm::interpret::interpreter,
                                           forge::vm::wasm::interpret::options>;
   impl(std::span<const std::uint8_t>, const compile_limits&);
   invocation_result invoke(const invocation&, const invocation_limits&, std::stop_token);

   compile_limits limits;
   std::unique_ptr<backend> engine;
   std::size_t bytes = 0;
};

} // namespace forge::vm::wasm::execution
