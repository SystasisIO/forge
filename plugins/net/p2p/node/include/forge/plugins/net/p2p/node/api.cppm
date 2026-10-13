module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <optional>
#include <chrono>
#include <cstdint>
#include <utility>
#include <vector>
#include <stop_token>

export module forge.plugins.net.p2p.node.api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.p2p.publication;
import forge.api.transport.exceptions;
import forge.api.transport.options;
import forge.api.transport.client;
import forge.api.transport.connection;
import forge.api.transport.server;
import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht;
import forge.net.p2p.pubsub;
import forge.net.p2p.protocol;
import forge.net.p2p.node;
import forge.multiformats.multiaddr;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.net.p2p.node.types;

export namespace forge::plugins::net::p2p::node {

class api : public forge::api::core::contract<api> {
 public:
   virtual ~api() = default;

   [[nodiscard]] virtual forge::net::p2p::peer_id local_peer() const = 0;
   [[nodiscard]] virtual std::optional<forge::net::p2p::endpoint> local_endpoint() const = 0;
   [[nodiscard]] virtual std::vector<forge::net::p2p::endpoint> local_endpoints() const = 0;
   [[nodiscard]] virtual info network_info() const = 0;

   virtual boost::asio::awaitable<forge::net::p2p::node::session_info>
   connect(forge::net::p2p::endpoint endpoint, forge::net::p2p::node::connect_options options = {}) = 0;
   virtual boost::asio::awaitable<forge::net::p2p::node::session_info>
   connect(forge::multiformats::multiaddr address, forge::net::p2p::node::connect_options options = {}) = 0;
   virtual boost::asio::awaitable<std::chrono::milliseconds>
   ping(forge::net::p2p::peer_id peer, forge::net::p2p::node::open_options options = {}) = 0;
   [[nodiscard]] virtual bool is_peer_protected(const forge::net::p2p::peer_id& peer) const = 0;
   // Each token removes only its own nonempty native key, including after stop.
   // Both signs are bounded separately so every token close order is safe.
   [[nodiscard]] virtual peer_contribution protect_peer(forge::net::p2p::peer_id peer) = 0;
   [[nodiscard]] virtual peer_contribution tag_peer(forge::net::p2p::peer_id peer, std::int64_t value) = 0;

   [[nodiscard]] virtual forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol) = 0;
   [[nodiscard]] virtual forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
               forge::api::transport::options options) = 0;

   virtual boost::asio::awaitable<forge::api::transport::connection>
   open_api_connection(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol, remote_options options = {}) = 0;

   template <typename Interface>
   boost::asio::awaitable<forge::api::core::handle<Interface>>
   remote(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol, remote_options options = {}) {
      return remote_owned<Interface>(open_api_connection(std::move(peer), std::move(protocol), options));
   }

 private:
   template <typename Interface>
   static boost::asio::awaitable<forge::api::core::handle<Interface>>
   remote_owned(boost::asio::awaitable<forge::api::transport::connection> operation) {
      auto connection = co_await std::move(operation);
      co_return co_await connection.template get_remote_api<Interface>();
   }
};

} // namespace forge::plugins::net::p2p::node

FORGE_EXPORT_API(::forge::plugins::net::p2p::node::api, FORGE_API_CONTRACT("forge.plugins.net.p2p.node", 3, 0))
