module;

#include <array>
#include <string_view>

export module forge.vm.wasm.execution.profile;

export namespace forge::vm::wasm::execution {

// Policy selection only. Signatures come from the Contract SDK registry.
inline constexpr auto intrinsic_names = std::array<std::string_view, 5>{
    "action_data_size", "read_action_data", "set_action_return_value", "eosio_assert_message", "prints_l"};

constexpr bool allows_intrinsic(std::string_view module, std::string_view name) noexcept {
   if (module != "env") {
      return false;
   }
   for (const auto item : intrinsic_names) {
      if (item == name) {
         return true;
      }
   }
   return false;
}

} // namespace forge::vm::wasm::execution
