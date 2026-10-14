module;

#include <forge/contract/intrinsics.hpp>
#include <forge/contract/types.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

module forge.execution.wasm.compiled_module;
import forge.execution.wasm.profile;
import forge.vm.wasm.interpret.host_function;

#include "details/intrinsics.hxx"

namespace forge::execution::wasm {

intrinsics::intrinsics(const invocation& call, const invocation_limits& limits) : _call(call), _limits(limits) {}

std::uint32_t intrinsics::action_data_size() const {
   return static_cast<std::uint32_t>(_call.data.size());
}

std::uint32_t intrinsics::read_action_data(std::span<char> destination) const {
   const auto count = std::min(destination.size(), _call.data.size());
   if (count != 0) {
      std::memcpy(destination.data(), _call.data.data(), count);
   }
   return static_cast<std::uint32_t>(count);
}

void intrinsics::set_action_return_value(std::span<const char> value) {
   if (value.size() > _limits.max_output_bytes) {
      throw exceptions::limit_exceeded{"Guest return value exceeds the invocation limit"};
   }
   _result.data.assign(value.begin(), value.end());
}

void intrinsics::eosio_assert_message(std::uint32_t condition, std::span<const char>) const {
   if (condition == 0) {
      throw exceptions::assertion_failed{"Guest assertion failed"};
   }
}

void intrinsics::prints_l(std::span<const char> value) {
   if (_limits.max_console_bytes == 0) {
      return;
   }
   if (value.size() > _limits.max_console_bytes - _result.console.size()) {
      throw exceptions::limit_exceeded{"Guest console exceeds the invocation limit"};
   }
   if (!value.empty()) {
      _result.console.append(value.data(), value.size());
   }
}

invocation_result intrinsics::result() && {
   return std::move(_result);
}

namespace {

template <typename Host> void register_profile() {
   namespace vm = forge::vm::wasm::interpret;
   using functions = vm::registered_host_functions<Host>;
   using forge_contract_size_t = vm::wasm_size_t;
   const auto signature = []<typename R, typename... A>(R (*)(A...)) {
      // C ABI pointers are wasm32 offsets; host spans are independently checked below.
      return vm::function_types_provider<typename functions::type_converter_t, R,
         std::tuple<std::conditional_t<std::is_pointer_v<A>, vm::wasm_size_t, A>...>>(
         std::index_sequence_for<A...>{});
   };
#define FORGE_EXECUTION_REGISTER(version, capability, header, feature, identifier, module_name, import_name, result, arguments) \
   if constexpr (allows_intrinsic(#module_name, #import_name)) { \
      using expected_signature = result (*) arguments; \
      const auto expected = signature(static_cast<expected_signature>(nullptr)); \
      functions::template add<&Host::identifier>(#module_name, #import_name); \
      const auto& actual = functions::mappings::get().host_functions.back(); \
      if (actual.params != expected.params || actual.ret != expected.ret) { \
         throw std::logic_error{"WASM adapter does not match the canonical SDK signature"}; \
      } \
   }
   FORGE_CONTRACT_INTRINSICS(FORGE_EXECUTION_REGISTER)
#undef FORGE_EXECUTION_REGISTER
}

} // namespace

void intrinsics::register_functions() {
   // A failed registration is terminal: never retry over partially populated mappings.
   static const auto failure = []() noexcept -> std::exception_ptr {
      try {
         register_profile<intrinsics>();
         return {};
      } catch (...) {
         return std::current_exception();
      }
   }();
   if (failure) std::rethrow_exception(failure);
}

} // namespace forge::execution::wasm
