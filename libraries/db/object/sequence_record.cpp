#include "details/sequence_record.hxx"

#include <forge/exceptions/macros.hpp>

import forge.db.object.exceptions;

namespace forge::db::object::detail {

std::vector<std::byte> sequence_record::encode(std::uint64_t value) {
   auto bytes = std::vector<std::byte>{};
   bytes.reserve(8U);
   for (auto shift = 56; shift >= 0; shift -= 8) {
      bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
   }
   return bytes;
}

std::uint64_t sequence_record::decode(std::span<const std::byte> bytes) {
   if (bytes.size() != 8U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_descriptor, "db object id sequence record has invalid size");
   }
   auto value = std::uint64_t{0};
   for (const auto byte : bytes) {
      value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
   }
   return value;
}

} // namespace forge::db::object::detail
