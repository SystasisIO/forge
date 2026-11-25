module;

#include <string>
#include <variant>

export module forge.api.chain.destination;

export import forge.net.p2p.identity;

export namespace forge::api::chain {

struct http_endpoint {
   // Stable name of a configured HTTP endpoint.
   std::string name;

   bool operator==(const http_endpoint&) const = default;
};

using destination = std::variant<::forge::net::p2p::peer_id, http_endpoint>;

} // namespace forge::api::chain
