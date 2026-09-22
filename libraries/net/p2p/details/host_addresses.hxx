#pragma once

#include <optional>
#include <string>
#include <vector>

namespace forge::net::p2p::host_addresses {

enum class endpoint_scope {
   public_address,
   private_address,
   loopback,
   link_local,
   unroutable,
   dns,
};

enum class source_kind {
   authenticated,
   routed,
   third_party,
};

struct learning_context {
   source_kind source = source_kind::third_party;
   std::optional<forge::net::p2p::endpoint> remote_endpoint;
};

[[nodiscard]] endpoint_scope classify_endpoint_scope(const forge::net::p2p::endpoint& value);

// Interface scope belongs to the receiving host and must not cross a P2P trust boundary.
[[nodiscard]] bool has_interface_zone(const forge::net::p2p::endpoint& value) noexcept;
[[nodiscard]] bool has_interface_zone(const forge::multiformats::multiaddr& value) noexcept;

// Numeric observer identity only, not a routability or authentication check.
// IPv4 uses the address; IPv6 uses its /56 prefix (mapped IPv4 stays IPv4).
[[nodiscard]] std::optional<std::string> observer_group(const forge::net::p2p::endpoint& value);

[[nodiscard]] std::vector<forge::net::p2p::endpoint> merge_advertised(const std::vector<forge::net::p2p::endpoint>& configured,
                                                               const std::vector<forge::net::p2p::endpoint>& listened,
                                                               const peer_id& local);

[[nodiscard]] std::optional<forge::net::p2p::endpoint> learned(forge::net::p2p::endpoint value, const peer_id& peer);
[[nodiscard]] std::optional<forge::net::p2p::endpoint> learned(forge::net::p2p::endpoint value, const peer_id& peer,
                                                        learning_context context);
[[nodiscard]] std::vector<forge::net::p2p::endpoint>
sanitize_discovered_endpoints(std::vector<forge::net::p2p::endpoint> values, const peer_id& peer, learning_context context);

[[nodiscard]] std::optional<forge::multiformats::multiaddr>
learned(forge::multiformats::multiaddr value, const peer_id& peer, learning_context context);
[[nodiscard]] std::vector<forge::multiformats::multiaddr>
sanitize_discovered_addresses(std::vector<forge::multiformats::multiaddr> values, const peer_id& peer,
                             learning_context context);

} // namespace forge::net::p2p::host_addresses
