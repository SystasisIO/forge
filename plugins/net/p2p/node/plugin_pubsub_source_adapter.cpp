module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
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

boost::asio::awaitable<forge::net::p2p::pubsub::message>
plugin::pubsub_source_adapter::async_publish_message(forge::net::p2p::pubsub::topic subject,
                                                     std::vector<std::uint8_t> data,
                                                     forge::net::p2p::pubsub::publish_options options) {
   const auto current = impl_->require_node();
   co_return co_await current->async_publish(std::move(subject), std::move(data), options);
}

boost::asio::awaitable<forge::net::p2p::pubsub::subscription>
plugin::pubsub_source_adapter::async_join_topic(forge::net::p2p::pubsub::topic subject,
                                                forge::net::p2p::pubsub::handler handler) {
   const auto current = impl_->require_node();
   co_return co_await current->async_subscribe(std::move(subject), std::move(handler));
}

boost::asio::awaitable<void> plugin::pubsub_source_adapter::async_leave_topic(forge::net::p2p::pubsub::topic subject) {
   const auto current = impl_->require_node();
   co_await current->async_unsubscribe(std::move(subject));
}

forge::net::p2p::pubsub::snapshot plugin::pubsub_source_adapter::snapshot() const {
   return impl_->require_node()->pubsub_snapshot();
}

} // namespace forge::plugins::net::p2p::node
