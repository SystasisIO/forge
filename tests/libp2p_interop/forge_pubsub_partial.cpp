#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

import forge.codec.hex;

#include "forge_pubsub_partial.hxx"

namespace forge::test::libp2p_interop {
namespace {
void validate_token(std::string_view token) {
   if (token.size() != 32 || !std::ranges::all_of(token, [](char value) {
          return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
       })) {
      throw std::runtime_error{"partial fixture requires a canonical token"};
   }
}

void append_u32(std::vector<std::uint8_t>& output, std::uint32_t value) {
   for (const auto shift : {24, 16, 8, 0}) { output.push_back(static_cast<std::uint8_t>(value >> shift)); }
}

void validate_metadata(forge_pubsub_partial::metadata value) {
   if (!value.revision || value.have > forge_pubsub_partial::mask || value.want > forge_pubsub_partial::mask ||
       (value.have & value.want) != 0) {
      throw std::runtime_error{"invalid partial fixture metadata"};
   }
}
} // namespace

std::vector<std::uint8_t> forge_pubsub_partial::group(std::string_view token, std::uint32_t sequence) {
   validate_token(token);
   if (!sequence) { throw std::runtime_error{"invalid partial fixture sequence"}; }
   auto output = forge::codec::hex::decode(token);
   append_u32(output, sequence);
   return output;
}

std::vector<std::uint8_t> forge_pubsub_partial::expected_part(std::string_view token, std::uint8_t index) {
   validate_token(token);
   if (index >= 3) { throw std::runtime_error{"invalid partial fixture index"}; }
   const auto text = "forge-pr12:" + std::string{token} + ":part-" + std::to_string(index);
   return {text.begin(), text.end()};
}

std::vector<std::uint8_t> forge_pubsub_partial::encode(const part& value) {
   if (value.index >= 3 || value.data.empty() || value.data.size() > 256) {
      throw std::runtime_error{"invalid partial fixture part"};
   }
   auto output = std::vector<std::uint8_t>{1, value.index,
       static_cast<std::uint8_t>(value.data.size() >> 8), static_cast<std::uint8_t>(value.data.size())};
   output.insert(output.end(), value.data.begin(), value.data.end());
   return output;
}

std::vector<std::uint8_t> forge_pubsub_partial::encode(metadata value) {
   validate_metadata(value);
   auto output = std::vector<std::uint8_t>{1};
   append_u32(output, value.revision);
   output.push_back(value.have);
   output.push_back(value.want);
   return output;
}

forge_pubsub_partial::part forge_pubsub_partial::decode_part(std::span<const std::uint8_t> bytes) {
   if (bytes.size() < 5 || bytes.size() > 260 || bytes[0] != 1 || bytes[1] >= 3 ||
       (static_cast<std::size_t>(bytes[2]) << 8 | bytes[3]) != bytes.size() - 4) {
      throw std::runtime_error{"invalid partial fixture part encoding"};
   }
   return {bytes[1], {bytes.begin() + 4, bytes.end()}};
}

forge_pubsub_partial::metadata forge_pubsub_partial::decode_metadata(std::span<const std::uint8_t> bytes) {
   if (bytes.size() != 7 || bytes[0] != 1) {
      throw std::runtime_error{"invalid partial fixture metadata encoding"};
   }
   auto revision = std::uint32_t{};
   for (std::size_t index = 1; index != 5; ++index) { revision = (revision << 8) | bytes[index]; }
   auto value = metadata{revision, bytes[5], bytes[6]};
   validate_metadata(value);
   return value;
}

std::vector<std::uint8_t> forge_pubsub_partial::reconstruct(std::string_view token,
    const std::array<std::vector<std::uint8_t>, 3>& parts) {
   validate_token(token);
   auto decoded = std::array<std::vector<std::uint8_t>, 3>{};
   for (const auto& bytes : parts) {
      auto value = decode_part(bytes);
      if (!decoded[value.index].empty() || value.data != expected_part(token, value.index)) {
         throw std::runtime_error{"duplicate or corrupt partial fixture part"};
      }
      decoded[value.index] = std::move(value.data);
   }
   auto output = std::vector<std::uint8_t>{};
   for (const auto& data : decoded) { output.insert(output.end(), data.begin(), data.end()); }
   return output;
}

void forge_pubsub_partial::self_test() {
   constexpr auto token = std::string_view{"00112233445566778899aabbccddeeff"};
   auto check = [](bool value) {
      if (!value) { throw std::runtime_error{"partial fixture golden mismatch"}; }
   };
   auto rejects = [&](auto operation) {
      try { operation(); }
      catch (const std::runtime_error&) { return; }
      check(false);
   };
   check(forge::codec::hex::encode(group(token)) == "00112233445566778899aabbccddeeff00000001");
   const auto meta = metadata{0x01020304, 5, 2};
   check(forge::codec::hex::encode(encode(meta)) == "01010203040502");
   check(decode_metadata(encode(meta)) == meta);
   check(forge::codec::hex::encode(encode(part{2, {'a', 'b', 'c'}})) == "01020003616263");
   auto parts = std::array<std::vector<std::uint8_t>, 3>{};
   auto expected = std::vector<std::uint8_t>{};
   for (std::uint8_t index = 0; index != 3; ++index) {
      const auto data = expected_part(token, index);
      parts[2 - index] = encode(part{index, data});
      expected.insert(expected.end(), data.begin(), data.end());
   }
   check(reconstruct(token, parts) == expected);
   parts[0] = parts[1];
   rejects([&] { static_cast<void>(reconstruct(token, parts)); });
   rejects([&] { static_cast<void>(group("00112233445566778899AABBCCDDEEFF")); });
   rejects([&] { static_cast<void>(group(token, 0)); });
   for (const auto value : {metadata{0, 1, 2}, metadata{1, 8, 0}, metadata{1, 1, 1}}) {
      rejects([&] { static_cast<void>(encode(value)); });
   }
   for (const auto& bytes : {std::vector<std::uint8_t>{}, std::vector<std::uint8_t>{1, 0, 0, 0},
          std::vector<std::uint8_t>{2, 0, 0, 1, 1}, std::vector<std::uint8_t>{1, 3, 0, 1, 1},
          std::vector<std::uint8_t>{1, 0, 0, 2, 1}}) {
      rejects([&] { static_cast<void>(decode_part(bytes)); });
   }
   for (const auto& bytes : {std::vector<std::uint8_t>{1}, std::vector<std::uint8_t>{2, 0, 0, 0, 1, 0, 7},
          std::vector<std::uint8_t>{1, 0, 0, 0, 0, 0, 7}, std::vector<std::uint8_t>{1, 0, 0, 0, 1, 8, 0}}) {
      rejects([&] { static_cast<void>(decode_metadata(bytes)); });
   }
}

} // namespace forge::test::libp2p_interop
