module;

#include <chrono>
#include <cstddef>

export module forge.vm.wasm.execution.limits;
export import forge.vm.wasm.interpret.options;

export namespace forge::vm::wasm::execution {

struct compile_limits {
   compile_limits();
   std::size_t max_module_bytes = 4 * 1024 * 1024;
   forge::vm::wasm::interpret::options vm;
};

struct invocation_limits {
   std::chrono::milliseconds budget{100};
   std::size_t max_input_bytes = 2 * 1024 * 1024;
   std::size_t max_output_bytes = 2 * 1024 * 1024;
   // Zero discards guest console; guest strings may contain private input.
   std::size_t max_console_bytes = 0;
};

void validate(const compile_limits&);
void validate(const invocation_limits&);

} // namespace forge::vm::wasm::execution
