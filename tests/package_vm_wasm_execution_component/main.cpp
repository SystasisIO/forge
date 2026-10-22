#include <cstdint>
#include <type_traits>
#include <vector>

import forge.vm.wasm.execution.compiled_module;
import forge.vm.wasm.execution.profile;

static_assert(!std::is_copy_constructible_v<forge::vm::wasm::execution::compiled_module>);
static_assert(std::is_same_v<decltype(forge::vm::wasm::execution::invocation::receiver), forge::chain::protocol::account_name>);

int main() {
   // Minimal apply(i64,i64,i64)->void, no imports or memory.
   const auto code = std::vector<std::uint8_t>{0,0x61,0x73,0x6d,1,0,0,0,
      1,7,1,0x60,3,0x7e,0x7e,0x7e,0,
      3,2,1,0,
      7,9,1,5,'a','p','p','l','y',0,0,
      10,4,1,2,0,0x0b};
   auto module = forge::vm::wasm::execution::compiled_module{code};
   const auto result = module.invoke({});
   return result.data.empty() && result.console.empty() && module.bytes() > 0 ? 0 : 1;
}
