module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/libp2p_identity_material.hxx"
#include "details/node_impl.hxx"

namespace forge::net::p2p {

boost::asio::awaitable<relay::reservation::info> node::async_reserve_relay(peer_id relay_peer) {
   return async_reserve_relay(std::move(relay_peer), relay::reservation::options{});
}

boost::asio::awaitable<relay::reservation::info> node::async_reserve_relay(peer_id relay_peer,
                                                                           relay::reservation::options options) {
   auto self = impl_;
   if (self->private_network_enabled()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P private-network profile does not permit relay reservations");
   }
   co_return co_await self->request_relay_reservation(relay_peer, options, node::connect_options{}.timeout);
}

boost::asio::awaitable<std::vector<relay::reservation::info>> node::async_refresh_relay_candidates() {
   auto self = impl_;
   if (self->private_network_enabled()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P private-network profile does not permit relay refresh");
   }
   co_return co_await self->refresh_relay_candidates(std::nullopt, self->options.limits.topology.query_timeout);
}

boost::asio::awaitable<std::vector<discovery::result>> node::async_refresh_discovery() {
   auto self = impl_;
   co_return co_await self->topology_manager_value->async_refresh();
}

boost::asio::awaitable<void> node::async_cancel_relay(peer_id relay_peer) {
   auto self = impl_;
   if (self->private_network_enabled()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P private-network profile does not permit relay cancellation");
   }
   self->cancel_outbound_relay(relay_peer);
   co_return;
}

boost::asio::awaitable<hole_punch::status>
node::async_attempt_hole_punch(peer_id peer, std::optional<peer_id> relay_peer, std::chrono::milliseconds timeout) {
   auto self = impl_;
   if (self->private_network_enabled()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P private-network profile does not permit hole punching");
   }
   co_return co_await self->attempt_hole_punch(std::move(peer), std::move(relay_peer), timeout);
}

} // namespace forge::net::p2p
