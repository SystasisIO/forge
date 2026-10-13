module;

#include <boost/asio/awaitable.hpp>
#include <forge/api/core/macros.hpp>

export module forge.plugins.net.p2p.node.protocol_api;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.identity;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.protocol_registration;

export namespace forge::plugins::net::p2p::node {
class protocol_api : public forge::api::core::contract<protocol_api> {
 public:
   virtual ~protocol_api() = default;
   [[nodiscard]] virtual protocol_registration register_protocol(
       forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) = 0;
   [[nodiscard]] virtual protocol_registration replace_protocol(const protocol_registration& owner,
       forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) = 0;
   virtual boost::asio::awaitable<forge::net::p2p::stream> open(
       forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol,
       forge::net::p2p::node::open_options options = {}) = 0;
};
}

FORGE_EXPORT_API(::forge::plugins::net::p2p::node::protocol_api,
                 FORGE_API_CONTRACT("forge.plugins.net.p2p.node.protocol", 1, 0))
