module;

#include <cstdint>
#include <string>

export module forge.net.transport.endpoint;

export namespace forge::net::transport {

struct endpoint {
   enum class host_kind {
      ip4,
      ip6,
      dns,
      dns4,
      dns6,
   };

   enum class protocol_kind {
      quic_v1,
      tcp,
   };

   host_kind host_type = host_kind::ip4;
   protocol_kind protocol = protocol_kind::quic_v1;
   std::string host;
   std::uint16_t port = 0;
   std::string zone;

   [[nodiscard]] std::string authority() const;
};

} // namespace forge::net::transport
