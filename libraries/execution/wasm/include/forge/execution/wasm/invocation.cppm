module;

#include <cstdint>
#include <span>
#include <string>

export module forge.execution.wasm.invocation;
export import forge.chain.protocol.values;
export import forge.raw.codec;

export namespace forge::execution::wasm {

struct invocation {
   forge::chain::protocol::account_name receiver;
   forge::chain::protocol::action_name action;
   // Borrowed only for the duration of the synchronous invocation.
   std::span<const std::uint8_t> data;
};

struct invocation_result {
   forge::raw::bytes data;
   std::string console;
};

} // namespace forge::execution::wasm
