#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace forge::db::object::detail {

struct sequence_record {
   static std::vector<std::byte> encode(std::uint64_t value);
   static std::uint64_t decode(std::span<const std::byte> bytes);
};

} // namespace forge::db::object::detail
