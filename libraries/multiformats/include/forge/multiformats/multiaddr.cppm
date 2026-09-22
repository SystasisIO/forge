module;
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module forge.multiformats.multiaddr;

import forge.multiformats.types;
import forge.multiformats.multicodec;

export namespace forge::multiformats {

enum class protocol_code : std::uint64_t {
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

[[nodiscard]] constexpr std::uint64_t code_value(protocol_code code) noexcept {
   return static_cast<std::uint64_t>(code);
}

struct multiaddr_component {
   protocol_code code;
   std::string value;

   bool operator==(const multiaddr_component&) const = default;
};

class multiaddr {
 public:
   [[nodiscard]] static multiaddr parse(std::string_view value);
   [[nodiscard]] static multiaddr from_bytes(std::span<const std::uint8_t> data);

   [[nodiscard]] std::string to_string() const;
   [[nodiscard]] bytes to_bytes() const;
   [[nodiscard]] const std::vector<multiaddr_component>& components() const noexcept;
   [[nodiscard]] multiaddr encapsulate(const multiaddr& value) const;
   [[nodiscard]] multiaddr decapsulate(const multiaddr& value) const;

   void push(multiaddr_component value);

 private:
   std::vector<multiaddr_component> components_;
};

} // namespace forge::multiformats
