module;

#include <cstdint>
#include <string>
#include <string_view>

export module forge.net.quic.endpoint;

import forge.net.quic.exceptions;

export namespace forge::net::quic {

struct endpoint {
   enum class address_family {
      any,
      ipv4,
      ipv6,
   };

   std::string host;
   std::uint16_t port = 0;
   address_family family = address_family::any;
   std::string zone;

   [[nodiscard]] std::string authority() const;
};

[[nodiscard]] endpoint parse_endpoint(std::string_view value);

} // namespace forge::net::quic
