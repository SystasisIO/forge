#include <cstdint>
#include <type_traits>
#include <vector>

import forge.api.core.types;
import forge.raw.codec;

static_assert(std::is_same_v<std::underlying_type_t<forge::api::core::status>, std::uint16_t>);
static_assert(static_cast<std::uint16_t>(forge::api::core::status::invalid_argument) == 400);
static_assert(forge::raw::shared_enum_wire_v<forge::api::core::status>);
static_assert(std::is_same_v<forge::raw::enum_wire_type_t<forge::api::core::status>, std::int64_t>);

int main() {
   const auto error = forge::api::core::error_payload{
      .error = "fixture",
      .message = "values-only consumer",
      .status_code = forge::api::core::status::invalid_argument,
      .identity = {.category = "fixture", .code = 7},
      .details_codec = forge::api::core::codec_id{.value = "raw"},
      .details = forge::api::core::bytes{1, 2},
   };
   const auto expected = forge::raw::bytes{0x90, 0x01, 0, 0, 0, 0, 0, 0};
   return !error.retryable && error.details->size() == 2 && error.identity.code == 7 &&
          forge::raw::pack(error.status_code) == expected &&
          forge::raw::unpack_exact<forge::api::core::status>(expected) == error.status_code ? 0 : 1;
}
