module;

#include <chrono>
#include <cstddef>

export module forge.net.p2p.mdns_policy;

export namespace forge::net::p2p {

// These are future-service defaults and hard bounds, not a production support claim.
struct mdns_policy {
   bool enabled = false;
   bool ipv4_enabled = true;
   bool ipv6_enabled = true;
   std::chrono::seconds record_ttl{120};
   std::chrono::milliseconds query_interval{60'000};
   std::size_t max_interfaces = 32;
   std::size_t max_peers = 256;
   std::size_t max_addresses_per_peer = 16;
   std::size_t max_pending_packets = 64;
   std::size_t max_packet_size = 8932;
   std::size_t max_records_per_packet = 128;
};

void validate(const mdns_policy& value);

} // namespace forge::net::p2p
