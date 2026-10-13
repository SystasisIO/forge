#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace forge::test::libp2p_interop {

// Application fixture format, not a production GossipSub payload contract.
class forge_pubsub_partial {
 public:
   struct metadata {
      std::uint32_t revision = 0;
      std::uint8_t have = 0;
      std::uint8_t want = 0;
      bool operator==(const metadata&) const = default;
   };
   struct part {
      std::uint8_t index = 0;
      std::vector<std::uint8_t> data;
   };

   static constexpr std::uint8_t mask = 7;
   static std::vector<std::uint8_t> group(std::string_view token, std::uint32_t sequence = 1);
   static std::vector<std::uint8_t> expected_part(std::string_view token, std::uint8_t index);
   static std::vector<std::uint8_t> encode(const part&);
   static std::vector<std::uint8_t> encode(metadata);
   static part decode_part(std::span<const std::uint8_t>);
   static metadata decode_metadata(std::span<const std::uint8_t>);
   static std::vector<std::uint8_t> reconstruct(std::string_view token,
       const std::array<std::vector<std::uint8_t>, 3>& parts);
   static void self_test();
};

} // namespace forge::test::libp2p_interop
