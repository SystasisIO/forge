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
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.plugins.net.p2p.node.types;
import forge.api.transport.connection;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.multiformats.multiaddr;
import forge.api.auth.authenticated_caller;
import forge.asio.notification;
import forge.swarm.runtime.capabilities;
#include "details/capability_context.hxx"
#include "details/capability_call.hxx"
#include "details/peer_connection.hxx"

namespace forge::plugins::swarm::node {

boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::connection>> peer_connection::open(
   std::shared_ptr<const capability_context> context, forge::net::p2p::peer_id peer,
   std::vector<forge::multiformats::multiaddr> addresses, std::stop_token stop,
   std::chrono::steady_clock::time_point deadline) {
   auto result = std::make_unique<peer_connection>();
   (void)co_await capability_call<int>::run(open_native(result.get(), context, std::move(peer),
      std::move(addresses), deadline), stop, context->lifetime, deadline,
      [owner = result.get()] { return owner->async_close(); });
   co_return result;
}

boost::asio::awaitable<int> peer_connection::open_native(peer_connection* owner,
   std::shared_ptr<const capability_context> context, forge::net::p2p::peer_id peer,
   std::vector<forge::multiformats::multiaddr> addresses, std::chrono::steady_clock::time_point deadline) {
   auto options = forge::plugins::net::p2p::node::remote_options{};
   options.require_authenticated_peer = true;
   options.address_hints = std::move(addresses);
   options.open_deadline = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
   if (options.open_deadline <= std::chrono::milliseconds::zero()) options.open_deadline = std::chrono::milliseconds{1};
   // Setup is bounded by the capability deadline; exchange lifetime is not.
   options.deadline = std::chrono::milliseconds::zero();
   owner->_native = co_await context->node->open_api_connection(peer, context->application, std::move(options));
   owner->_identity = {forge::api::auth::caller_source::p2p_peer, forge::crypto::digest::sha256::hash(peer.to_bytes())};
   owner->_remote = co_await owner->_native.get_remote_api<forge::swarm::api::peer>();
   co_return 0;
}

forge::api::core::handle<forge::swarm::api::peer> peer_connection::remote() const { return _remote; }
forge::api::auth::authenticated_caller peer_connection::remote_identity() const { return _identity; }
void peer_connection::cancel() noexcept { _native.cancel(); }
boost::asio::awaitable<void> peer_connection::async_close() { return _native.async_close(); }

} // namespace forge::plugins::swarm::node
