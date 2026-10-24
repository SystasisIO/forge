#include <cstdint>
#include <span>
#include <vector>

import forge.raw.codec;

bool portable_limits_preserve_legacy_wire() {
   const auto value = forge::raw::unpack_limits{.max_container_elements = 4U,
                                               .max_total_container_elements = 6U,
                                               .max_bytes = 8U,
                                               .first_container_elements = 4U};
   const auto [per_container, total_elements, bytes_limit, first_container] = value;
   if (per_container != 4U || total_elements != 6U || bytes_limit != 8U || first_container != 4U) {
      return false;
   }
   const auto bytes = forge::raw::pack(value);
   const auto expected = forge::raw::pack(std::uint32_t{4}, std::uint32_t{6}, std::uint32_t{8}, std::uint32_t{4});
   if (bytes != expected || bytes.size() != 16U) {
      return false;
   }
   const auto decoded = forge::raw::unpack_exact<forge::raw::unpack_limits>(std::span<const std::uint8_t>{bytes}, {});
   auto legacy_stream = forge::raw::make_input_stream(bytes.data(), bytes.size(), {});
   auto legacy = forge::raw::unpack_limits{};
   forge::raw::unpack(legacy_stream, legacy);
   return legacy.max_container_elements == 4U && decoded.max_container_elements == 4U &&
          decoded.max_total_container_elements == 6U &&
          decoded.max_bytes == 8U && decoded.first_container_elements == 4U;
}
