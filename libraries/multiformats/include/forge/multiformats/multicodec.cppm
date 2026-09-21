module;
#include <cstdint>
#include <span>
#include <string_view>

export module forge.multiformats.multicodec;

import forge.multiformats.types;

export namespace forge::multiformats {

enum class multicodec_code : std::uint64_t {
   // Source: multiformats/multicodec table, core hash and libp2p-key codes.
   identity = 0x00,
   sha2_256 = 0x12,
   sha2_512 = 0x13,
   libp2p_key = 0x72,

   // Source: multiformats/multiaddr protocol table.
   ip4 = 0x04,
   tcp = 0x06,
   ip6 = 0x29,
   ip6zone = 0x2a,
   dns = 0x35,
   dns4 = 0x36,
   dns6 = 0x37,
   dnsaddr = 0x38,
   udp = 0x0111,
   p2p_circuit = 0x0122,
   p2p = 0x01a5,
   quic = 0x01cc,
   quic_v1 = 0x01cd,
   ws = 0x01dd,
   wss = 0x01de,
};

[[nodiscard]] constexpr std::uint64_t code_value(multicodec_code code) noexcept {
   return static_cast<std::uint64_t>(code);
}

[[nodiscard]] bytes multicodec_encode(multicodec_code code);
[[nodiscard]] multicodec_code multicodec_decode(std::span<const std::uint8_t> data, std::size_t& consumed);
[[nodiscard]] std::string_view protocol_name(multicodec_code code);
[[nodiscard]] multicodec_code parse_protocol_code(std::string_view name);

} // namespace forge::multiformats
