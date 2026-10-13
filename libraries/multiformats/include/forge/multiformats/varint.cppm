module;
#include <cstddef>
#include <cstdint>
#include <span>

export module forge.multiformats.varint;

import forge.multiformats.types;

export namespace forge::multiformats {

struct decoded_varint {
   std::uint64_t value = 0;
   std::size_t size = 0;
};

[[nodiscard]] bytes varint_encode(std::uint64_t value);
[[nodiscard]] std::size_t varint_encoded_size(std::uint64_t value) noexcept;
[[nodiscard]] decoded_varint varint_decode(std::span<const std::uint8_t> data);

} // namespace forge::multiformats
