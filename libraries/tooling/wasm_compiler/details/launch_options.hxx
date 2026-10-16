#pragma once

#include <cstdint>

namespace forge::tooling::wasm_compiler::command {

// Private launch-only value, never a parallel compile/execution profile.
struct launch_options {
   vm::wasm::optimizing::compiler_protocol::exchange_id exchange;
   std::uint64_t max_request_bytes = 0;
   std::uint64_t max_response_bytes = 0;
   vm::wasm::optimizing::compiler_protocol::resource_limits limits;
};

} // namespace forge::tooling::wasm_compiler::command
