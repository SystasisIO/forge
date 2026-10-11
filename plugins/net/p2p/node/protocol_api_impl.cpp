module;

#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.api.transport.options;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.net.p2p.node;
import forge.net.p2p.topology;
import forge.net.p2p.protocol;
import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.net.p2p.stream;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.net.p2p.peer_store;
import forge.net.p2p.dht.record_store;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/protocol_api_impl.hxx"
#include "details/protocol_registry.hxx"

namespace forge::plugins::net::p2p::node {
plugin::protocol_api_impl::protocol_api_impl(std::shared_ptr<plugin::impl> owner) : owner_{std::move(owner)} {}

protocol_registration plugin::protocol_api_impl::register_protocol(forge::net::p2p::protocol_id protocol,
    forge::net::p2p::node::protocol_handler handler) {
   if (owner_->stop_requested.load()) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol admission is closed"); }
   return owner_->protocols->register_raw(std::move(protocol), std::move(handler));
}
protocol_registration plugin::protocol_api_impl::replace_protocol(const protocol_registration& registration,
    forge::net::p2p::protocol_id protocol, forge::net::p2p::node::protocol_handler handler) {
   if (owner_->stop_requested.load()) { FORGE_THROW_EXCEPTION(exceptions::route_conflict, "protocol admission is closed"); }
   return owner_->protocols->register_raw(std::move(protocol), std::move(handler), &registration);
}
boost::asio::awaitable<forge::net::p2p::stream> plugin::protocol_api_impl::open(forge::net::p2p::peer_id peer,
    forge::net::p2p::protocol_id protocol, forge::net::p2p::node::open_options options) {
   return open_owned(owner_, std::move(peer), std::move(protocol), std::move(options));
}
boost::asio::awaitable<forge::net::p2p::stream> plugin::protocol_api_impl::open_owned(
    std::shared_ptr<plugin::impl> owner, forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol,
    forge::net::p2p::node::open_options options) {
   const auto node = owner->require_running_node();
   co_return co_await node->async_open_protocol_stream(std::move(peer), std::move(protocol), std::move(options));
}
}
