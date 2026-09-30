module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

#include <optional>
#include <utility>
#include <vector>

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
import forge.net.p2p.pubsub;
import forge.net.p2p.protocol;
import forge.net.p2p.node;
import forge.plugins.net.p2p.node.types;

export namespace forge::plugins::net::p2p::node {

class api : public forge::api::core::contract<api> {
 public:
   virtual ~api() = default;

   [[nodiscard]] virtual forge::net::p2p::peer_id local_peer() const = 0;
   [[nodiscard]] virtual std::optional<forge::net::p2p::endpoint> local_endpoint() const = 0;
   [[nodiscard]] virtual std::vector<forge::net::p2p::endpoint> local_endpoints() const = 0;
   [[nodiscard]] virtual info network_info() const = 0;

   [[nodiscard]] virtual forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol) = 0;
   [[nodiscard]] virtual forge::api::p2p::publication
   publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
               forge::api::transport::options options) = 0;
   virtual void publish_protocol(forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) = 0;

   virtual boost::asio::awaitable<forge::api::transport::connection>
   open_api_connection(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol, remote_options options = {}) = 0;

   template <typename Interface>
   boost::asio::awaitable<forge::api::core::handle<Interface>>
   remote(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol, remote_options options = {}) {
      auto connection = co_await open_api_connection(std::move(peer), std::move(protocol), options);
      co_return co_await connection.template get_remote_api<Interface>();
   }
};

class diagnostics_source : public forge::api::core::contract<diagnostics_source> {
 public:
   virtual ~diagnostics_source() = default;

   [[nodiscard]] virtual forge::net::p2p::diagnostics::snapshot
   snapshot(forge::net::p2p::diagnostics::options options = {}) const = 0;
};

class pubsub_source : public forge::api::core::contract<pubsub_source> {
 public:
   virtual ~pubsub_source() = default;

   virtual void enable(forge::net::p2p::pubsub::options options) = 0;
   [[nodiscard]] virtual forge::net::p2p::peer_id local_peer() const = 0;
   virtual boost::asio::awaitable<forge::net::p2p::pubsub::message>
   async_publish_message(forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data,
                         forge::net::p2p::pubsub::publish_options options) = 0;
   virtual boost::asio::awaitable<forge::net::p2p::pubsub::subscription>
   async_join_topic(forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler handler) = 0;
   virtual boost::asio::awaitable<void> async_leave_topic(forge::net::p2p::pubsub::topic subject) = 0;
   [[nodiscard]] virtual forge::net::p2p::pubsub::snapshot snapshot() const = 0;
};

} // namespace forge::plugins::net::p2p::node

FORGE_EXPORT_API(::forge::plugins::net::p2p::node::api, FORGE_API_CONTRACT("forge.plugins.net.p2p.node", 2, 0))
FORGE_EXPORT_API(::forge::plugins::net::p2p::node::diagnostics_source,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.node.diagnostics_source", 2, 0))
FORGE_EXPORT_API(::forge::plugins::net::p2p::node::pubsub_source,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.node.pubsub_source", 1, 0))
