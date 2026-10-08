module;

#include <forge/exceptions/macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

module forge.multiformats.varint;

import forge.multiformats.exceptions;

namespace forge::multiformats {

std::size_t varint_encoded_size(std::uint64_t value) noexcept {
   return value == 0 ? 1 : (std::bit_width(value) + 6) / 7;
}

bytes varint_encode(std::uint64_t value) {
   auto out = bytes{};
   do {
      auto byte = static_cast<std::uint8_t>(value & 0x7fU);
      value >>= 7U;
      if (value != 0) {
         byte |= 0x80U;
      }
      out.push_back(byte);
   } while (value != 0);
   return out;
}

decoded_varint varint_decode(std::span<const std::uint8_t> data) {
   std::uint64_t value = 0;
   std::uint32_t shift = 0;
   for (std::size_t index = 0; index < data.size(); ++index) {
      const auto byte = data[index];
      const auto payload = static_cast<std::uint8_t>(byte & 0x7fU);
      if (shift >= 64 || (shift == 63 && payload > 1)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_format, "multiformats varint overflows uint64");
      }

      value |= static_cast<std::uint64_t>(payload) << shift;
      if ((byte & 0x80U) == 0) {
         const auto encoded = varint_encode(value);
         if (encoded.size() != index + 1) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_format, "multiformats varint is not minimally encoded");
         }
         return {.value = value, .size = index + 1};
      }
      shift += 7;
   }

   FORGE_THROW_EXCEPTION(exceptions::invalid_format, "unterminated multiformats varint");
}

} // namespace forge::multiformats
