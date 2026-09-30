module;

#include <forge/api/core/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <memory>
#include <utility>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.binding;
import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.exceptions;
import forge.api.core.handle;
import forge.api.core.registry;
import forge.api.core.types;
import forge.exceptions;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.types;

#include "details/resolver_protocol.hxx"

FORGE_API(::forge::plugins::net::p2p::resolver::detail::resolver_protocol,
          FORGE_API_CONTRACT("forge.plugins.net.p2p.resolver.protocol", 1, 0), FORGE_API_METHOD(query))

namespace forge::plugins::net::p2p::resolver::detail {

resolver_protocol::resolver_protocol(std::weak_ptr<plugin::impl> owner) : owner_{std::move(owner)} {}

resolver_protocol::~resolver_protocol() = default;

boost::asio::awaitable<response> resolver_protocol::query(::forge::plugins::net::p2p::resolver::query request) {
   auto owner = owner_.lock();
   if (!owner) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P API resolver plugin owner has expired");
   }
   co_return query_resolver_protocol(owner, request);
}

} // namespace forge::plugins::net::p2p::resolver::detail

namespace forge::plugins::net::p2p::resolver {

forge::api::core::binding_plan
make_resolver_protocol_plan(forge::api::core::registry& registry, std::weak_ptr<plugin::impl> owner) {
   registry.clear();
   registry.install<detail::resolver_protocol>(std::make_shared<detail::resolver_protocol>(std::move(owner)));

   return forge::api::core::binding()
       .serve(registry)
       .export_api<detail::resolver_protocol>(
           {.id = {"forge.plugins.net.p2p.resolver.protocol"}, .major = 1, .min_revision = 0})
       .build();
}

boost::asio::awaitable<std::vector<entry>>
query_resolver_peer(forge::plugins::net::p2p::node::api& p2p, forge::net::p2p::peer_id peer,
                    forge::net::p2p::protocol_id protocol,
                    forge::plugins::net::p2p::node::remote_options options) {
   auto remote = co_await p2p.remote<detail::resolver_protocol>(std::move(peer), std::move(protocol),
                                                                std::move(options));
   auto result = co_await remote->query(query{});
   co_return std::move(result.apis);
}

} // namespace forge::plugins::net::p2p::resolver
