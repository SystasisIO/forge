module;

#include <chrono>

#include <forge/exceptions/macros.hpp>

module forge.net.p2p.mdns_policy;

import forge.net.p2p.exceptions;

namespace forge::net::p2p {

void validate(const mdns_policy& value) {
   constexpr auto minimum_packet_size = std::size_t{512};
   constexpr auto maximum_packet_size = std::size_t{8932};
   constexpr auto minimum_ttl = std::chrono::seconds{2};
   constexpr auto maximum_ttl = std::chrono::hours{1};

   if (value.max_interfaces == 0 || value.max_peers == 0 || value.max_addresses_per_peer == 0 ||
       value.max_pending_packets == 0 || value.max_records_per_packet == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS resource bounds must be non-zero");
   }
   if (value.max_packet_size < minimum_packet_size || value.max_packet_size > maximum_packet_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS packet size must be between 512 and 8932 bytes");
   }
   if (value.record_ttl < minimum_ttl || value.record_ttl > maximum_ttl) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS record TTL must be between two seconds and one hour");
   }

   // Convert only after bounding TTL, then compare within the common duration representation.
   const auto half_ttl = std::chrono::duration_cast<std::chrono::milliseconds>(value.record_ttl) / 2;
   if (value.query_interval < std::chrono::seconds{1} || value.query_interval > half_ttl) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS query interval must be at least one second and at most half the TTL");
   }
   if (value.enabled && !value.ipv4_enabled && !value.ipv6_enabled) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "enabled mDNS requires IPv4 or IPv6");
   }
}

} // namespace forge::net::p2p
