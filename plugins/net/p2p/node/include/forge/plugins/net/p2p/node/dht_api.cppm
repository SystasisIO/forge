module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <chrono>
#include <cstdint>
#include <span>
#include <vector>

export module forge.plugins.net.p2p.node.dht_api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.chrono.timestamp;
import forge.net.p2p.dht;
import forge.net.p2p.identity;
import forge.net.p2p.ipns;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;

export namespace forge::plugins::net::p2p::node {

// Local-only contract: provider registrations and signing stay on the owning node.
class dht_api : public forge::api::core::contract<dht_api> {
 public:
   virtual ~dht_api() = default;

   virtual boost::asio::awaitable<forge::net::p2p::dht::query_result>
   find_peer(forge::net::p2p::protocol_id profile, forge::net::p2p::peer_id peer,
             forge::net::p2p::dht::query_options options = {}) = 0;
   virtual boost::asio::awaitable<forge::net::p2p::provider_registration>
   provide(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
           forge::net::p2p::dht::query_options options = {}) = 0;
   virtual boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>>
   find_providers(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
                  forge::net::p2p::dht::query_options options = {}) = 0;
   virtual boost::asio::awaitable<forge::net::p2p::dht::value_put_result>
   put_value(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::record value,
             forge::net::p2p::dht::query_options options = {}) = 0;
   virtual boost::asio::awaitable<forge::net::p2p::dht::value_get_result>
   get_value(forge::net::p2p::protocol_id profile, forge::net::p2p::dht::key key,
             forge::net::p2p::dht::query_options options = {}) = 0;
   [[nodiscard]] virtual forge::net::p2p::ipns::record
   create_ipns_record(std::span<const std::uint8_t> value, std::uint64_t sequence,
                       forge::chrono::timestamp eol, std::chrono::nanoseconds ttl,
                       forge::net::p2p::ipns::create_options options = {}) const = 0;
};

} // namespace forge::plugins::net::p2p::node

FORGE_EXPORT_API(::forge::plugins::net::p2p::node::dht_api,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.node.dht", 1, 0))
