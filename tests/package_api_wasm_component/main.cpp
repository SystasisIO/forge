#include <cstdint>
#include <type_traits>
#include <variant>
#include <vector>

import forge.api.wasm.step;
import forge.api.wasm.http_command;
import forge.api.wasm.context;
import forge.api.wasm.codec;
import forge.raw.codec;

static_assert(std::is_same_v<decltype(forge::api::wasm::payload::data), forge::raw::bytes>);
static_assert(std::is_same_v<decltype(forge::api::wasm::http_command::method), forge::net::http::method>);
static_assert(std::is_same_v<std::variant_alternative_t<2, decltype(forge::api::wasm::step::result)>,
                            forge::api::core::error_payload>);

int main() {
   const auto value = forge::api::wasm::step{forge::api::wasm::completed{{"answer", {42}}}};
   const auto expected = forge::raw::bytes{1, 6, 'a', 'n', 's', 'w', 'e', 'r', 1, 42};
   if (forge::raw::pack(value) != expected) {
      return 1;
   }
   const auto decoded = forge::raw::unpack_exact<forge::api::wasm::step>(expected);
   forge::api::wasm::validate(decoded);
   try {
      forge::api::wasm::validate(forge::api::wasm::payload{"", {}});
      return 3;
   } catch (const forge::api::wasm::exceptions::invalid_value&) {
   }
   const auto& output = std::get<forge::api::wasm::completed>(decoded.result).output;
   return output.type == "answer" && output.data == forge::raw::bytes{42} ? 0 : 2;
}
