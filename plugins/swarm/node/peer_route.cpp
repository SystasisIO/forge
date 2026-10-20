module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <chrono>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.net.p2p.address_resolution;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.net.p2p.dht;
import forge.multiformats.multiaddr;
import forge.api.transport.connection;
import forge.swarm.runtime.capabilities;
#include "details/capability_context.hxx"
#include "details/peer_connection.hxx"
#include "details/peer_route.hxx"

namespace forge::plugins::swarm::node {

peer_route::peer_route(std::shared_ptr<const capability_context> backend, forge::net::p2p::peer_id peer,
   std::vector<forge::multiformats::multiaddr> addresses) {
   const auto bounds = forge::net::p2p::address_resolution::limits{};
   if (!backend || !backend->node || peer.value.size() > 1024U || !forge::net::p2p::valid_peer_id(peer) ||
       addresses.size() > bounds.max_roots)
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm route needs a bounded peer and address context");
   for (const auto& address : addresses) {
      auto size = std::size_t{};
      if (address.components().size() > bounds.max_multiaddr_size)
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm route address exceeds native input bounds");
      for (const auto& component : address.components()) {
         if (component.value.size() > bounds.max_multiaddr_size - size)
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm route address exceeds native input bounds");
         size += component.value.size();
      }
      if (address.to_string().size() > bounds.max_multiaddr_size)
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm route address exceeds native input bounds");
   }
   auto key = peer.to_string();
   if (key.empty() || key.size() > 1024U)
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm route diagnostic key exceeds runtime bounds");
   _context = std::make_shared<const context>(context{std::move(backend), std::move(peer), std::move(addresses), std::move(key)});
}

std::string_view peer_route::key() const noexcept { return _context->key; }
boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> peer_route::async_connect(
   std::stop_token stop, std::chrono::steady_clock::time_point deadline) const {
   return connect_owned(_context, stop, deadline);
}
boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> peer_route::connect_owned(
   std::shared_ptr<const context> context, std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
   co_return co_await peer_connection::open(context->backend, context->peer, context->addresses, stop, deadline);
}

} // namespace forge::plugins::swarm::node
