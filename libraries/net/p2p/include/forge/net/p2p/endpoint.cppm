module;
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

export module forge.net.p2p.endpoint;

import forge.multiformats.multiaddr;
import forge.net.p2p.identity;
import forge.net.transport.endpoint;

export namespace forge::net::p2p {

struct endpoint {
   using host_kind = forge::net::transport::endpoint::host_kind;
   using protocol_kind = forge::net::transport::endpoint::protocol_kind;

   enum class encapsulation_kind {
      none,
      ws,
      wss,
   };

   struct circuit {
      peer_id target;
   };

   forge::net::transport::endpoint transport;
   encapsulation_kind encapsulation = encapsulation_kind::none;
   std::optional<peer_id> peer;
   std::optional<circuit> relayed;

   [[nodiscard]] forge::multiformats::multiaddr to_multiaddr() const;
   [[nodiscard]] std::string to_string() const;
   [[nodiscard]] bool is_direct_quic() const noexcept;
   [[nodiscard]] bool is_direct_tcp() const noexcept;
};

[[nodiscard]] endpoint parse_endpoint(std::string_view value);
[[nodiscard]] endpoint parse_endpoint(const forge::multiformats::multiaddr& value);

} // namespace forge::net::p2p
