module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.asio.runtime;
import forge.asio.task;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.types;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/plugin_pubsub_source_adapter.hxx"

namespace forge::plugins::net::p2p::node {

plugin::pubsub_source_adapter::pubsub_source_adapter(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

void plugin::pubsub_source_adapter::enable(forge::net::p2p::pubsub::options options) {
   impl_->enable_pubsub(std::move(options));
}

forge::net::p2p::peer_id plugin::pubsub_source_adapter::local_peer() const {
   return impl_->require_node()->local_peer();
}

forge::net::p2p::pubsub::snapshot plugin::pubsub_source_adapter::snapshot() const {
   return impl_->require_node()->pubsub_snapshot();
}

boost::asio::awaitable<forge::net::p2p::pubsub::message> plugin::pubsub_source_adapter::async_publish_message(forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data, forge::net::p2p::pubsub::publish_options options) {
   return publish_owned(impl_, std::move(subject), std::move(data), options);
}

boost::asio::awaitable<forge::net::p2p::pubsub::message> plugin::pubsub_source_adapter::publish_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject, std::vector<std::uint8_t> data, forge::net::p2p::pubsub::publish_options options) {
   const auto current = state->require_running_node();
   co_return co_await current->async_publish(std::move(subject), std::move(data), options);
}

boost::asio::awaitable<forge::net::p2p::pubsub::subscription> plugin::pubsub_source_adapter::async_join_topic(forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback) {
   return join_owned(impl_, std::move(subject), std::move(callback));
}

boost::asio::awaitable<forge::net::p2p::pubsub::subscription> plugin::pubsub_source_adapter::join_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback) {
   const auto current = state->require_running_node();
   co_return co_await current->async_subscribe(std::move(subject), std::move(callback));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::async_leave_topic(forge::net::p2p::pubsub::topic subject) {
   return leave_owned(impl_, std::move(subject));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::leave_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject) {
   const auto current = state->require_node();
   co_await current->async_unsubscribe(std::move(subject));
}

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::pubsub_source_adapter::async_enable_partial(forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback, forge::net::p2p::pubsub::partial_options options) {
   return enable_partial_owned(impl_, std::move(subject), std::move(callback), std::move(options));
}

boost::asio::awaitable<forge::net::p2p::pubsub::partial_topic> plugin::pubsub_source_adapter::enable_partial_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::topic subject, forge::net::p2p::pubsub::handler callback, forge::net::p2p::pubsub::partial_options options) {
   const auto current = state->require_running_node();
   co_return co_await current->async_subscribe(std::move(subject), std::move(callback), std::move(options));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::async_disable_partial(forge::net::p2p::pubsub::partial_topic token) {
   return disable_partial_owned(impl_, std::move(token));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::disable_partial_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token) {
   const auto current = state->require_running_node();
   co_await current->async_disable_partial(std::move(token));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::async_advertise_partial(forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
   return advertise_owned(impl_, std::move(token), std::move(group));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::advertise_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
   const auto current = state->require_running_node();
   co_await current->async_advertise_partial(std::move(token), std::move(group));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::async_forget_partial(forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
   return forget_owned(impl_, std::move(token), std::move(group));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::forget_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token, std::vector<std::uint8_t> group) {
   const auto current = state->require_running_node();
   co_await current->async_forget_partial(std::move(token), std::move(group));
}

boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>> plugin::pubsub_source_adapter::async_partial_peers(forge::net::p2p::pubsub::partial_topic token) {
   return peers_owned(impl_, std::move(token));
}

boost::asio::awaitable<std::vector<forge::net::p2p::peer_id>> plugin::pubsub_source_adapter::peers_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token) {
   const auto current = state->require_running_node();
   co_return co_await current->async_partial_peers(std::move(token));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::async_send_partial(forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer, forge::net::p2p::pubsub::partial_message value, std::stop_token stop) {
   return send_owned(impl_, std::move(token), std::move(peer), std::move(value), stop);
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::send_owned(
    std::shared_ptr<plugin::impl> state, forge::net::p2p::pubsub::partial_topic token, forge::net::p2p::peer_id peer, forge::net::p2p::pubsub::partial_message value, std::stop_token stop) {
   const auto current = state->require_running_node();
   co_await current->async_send_partial(std::move(token), std::move(peer), std::move(value), stop);
}

} // namespace forge::plugins::net::p2p::node
