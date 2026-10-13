module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.api.transport.connection;
import forge.api.transport.options;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.multiformats.multiaddr;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.net.p2p.topology;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.types;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::p2p::node {

boost::asio::awaitable<forge::net::p2p::node::session_info> plugin::api_impl::connect(
    forge::net::p2p::endpoint endpoint, forge::net::p2p::node::connect_options options) {
   return connect_owned(impl_, std::move(endpoint), std::move(options));
}
boost::asio::awaitable<forge::net::p2p::node::session_info> plugin::api_impl::connect(
    forge::multiformats::multiaddr address, forge::net::p2p::node::connect_options options) {
   return connect_owned(impl_, std::move(address), std::move(options));
}
boost::asio::awaitable<std::chrono::milliseconds> plugin::api_impl::ping(
    forge::net::p2p::peer_id peer, forge::net::p2p::node::open_options options) {
   return ping_owned(impl_, std::move(peer), std::move(options));
}

boost::asio::awaitable<forge::net::p2p::node::session_info> plugin::api_impl::connect_owned(
    std::shared_ptr<plugin::impl> owner, forge::net::p2p::endpoint endpoint,
    forge::net::p2p::node::connect_options options) {
   const auto node = owner->require_running_node();
   co_return co_await node->async_connect(std::move(endpoint), std::move(options));
}
boost::asio::awaitable<forge::net::p2p::node::session_info> plugin::api_impl::connect_owned(
    std::shared_ptr<plugin::impl> owner, forge::multiformats::multiaddr address,
    forge::net::p2p::node::connect_options options) {
   const auto node = owner->require_running_node();
   co_return co_await node->async_connect(std::move(address), std::move(options));
}
boost::asio::awaitable<std::chrono::milliseconds> plugin::api_impl::ping_owned(
    std::shared_ptr<plugin::impl> owner, forge::net::p2p::peer_id peer,
    forge::net::p2p::node::open_options options) {
   const auto node = owner->require_running_node();
   co_return co_await node->async_ping(std::move(peer), std::move(options));
}

bool plugin::api_impl::is_peer_protected(const forge::net::p2p::peer_id& peer) const {
   return impl_->require_running_node()->is_peer_protected(peer);
}
peer_contribution plugin::api_impl::protect_peer(forge::net::p2p::peer_id peer) {
   const auto node = impl_->require_running_node();
   return impl_->contributions->protect(std::move(peer));
}
peer_contribution plugin::api_impl::tag_peer(forge::net::p2p::peer_id peer, std::int64_t value) {
   const auto node = impl_->require_running_node();
   return impl_->contributions->tag(std::move(peer), value);
}

} // namespace forge::plugins::net::p2p::node
