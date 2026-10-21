module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.net.p2p.provider_registration;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.net.p2p.dht;
import forge.multiformats.multiaddr;
import forge.asio.notification;
import forge.swarm.runtime.capabilities;
#include "details/capability_context.hxx"
#include "details/capability_call.hxx"
#include "details/peer_capabilities.hxx"
#include "details/peer_route.hxx"
#include "details/provider_registration.hxx"
#include "details/provider_key.hxx"

namespace forge::plugins::swarm::node {

peer_capabilities::peer_capabilities(std::shared_ptr<const capability_context> context) : _context{std::move(context)} {}
std::unique_ptr<forge::swarm::runtime::capabilities::route> peer_capabilities::make_route(
   forge::net::p2p::peer_id peer, std::vector<forge::multiformats::multiaddr> addresses) const {
   return std::make_unique<peer_route>(_context, std::move(peer), std::move(addresses));
}
boost::asio::awaitable<std::vector<std::unique_ptr<forge::swarm::runtime::capabilities::route>>>
peer_capabilities::async_lookup(forge::swarm::core::swarm_id id, std::string realm, std::size_t limit,
   std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
   return lookup_owned(_context, id, std::move(realm), limit, stop, deadline);
}
boost::asio::awaitable<std::vector<std::unique_ptr<forge::swarm::runtime::capabilities::route>>>
peer_capabilities::lookup_owned(std::shared_ptr<const capability_context> context, forge::swarm::core::swarm_id id,
   std::string realm, std::size_t limit, std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
   forge::swarm::protocol::validate_realm(realm);
   if (limit == 0U || limit > forge::net::p2p::dht::options{}.max_query_peers)
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "Swarm discovery exceeds bounded query context");
   auto peers = co_await capability_call<std::vector<forge::net::p2p::dht::peer>>::run(
      lookup_native(context, provider_key(id, std::move(realm)), limit, deadline), stop, context->lifetime, deadline);
   if (peers.size() > limit)
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "DHT provider result exceeds requested route bound");
   auto result = std::vector<std::unique_ptr<route>>{};
   result.reserve(peers.size());
   for (auto& peer : peers) result.push_back(std::make_unique<peer_route>(context, std::move(peer.id), std::move(peer.endpoints)));
   co_return result;
}
boost::asio::awaitable<std::vector<forge::net::p2p::dht::peer>> peer_capabilities::lookup_native(
   std::shared_ptr<const capability_context> context, forge::net::p2p::dht::key key, std::size_t limit,
   std::chrono::steady_clock::time_point deadline) {
   auto options = forge::net::p2p::dht::query_options{};
   options.requested_count = limit;
   options.timeout = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
   if (options.timeout <= std::chrono::milliseconds::zero()) options.timeout = std::chrono::milliseconds{1};
   co_return co_await context->dht->find_providers(context->profile, std::move(key), options);
}
boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::registration>>
peer_capabilities::async_register(forge::swarm::core::swarm_id id, std::string realm,
   std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
   forge::swarm::protocol::validate_realm(realm);
   return provider_registration::open(_context, provider_key(id, std::move(realm)), stop, deadline);
}

} // namespace forge::plugins::swarm::node
