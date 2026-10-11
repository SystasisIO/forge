module;

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
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
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;
import forge.plugins.net.p2p.node.host_event_source;
import forge.plugins.net.p2p.node.types;

#include "details/plugin_impl.hxx"
#include "details/plugin_host_event_source_adapter.hxx"

namespace forge::plugins::net::p2p::node {

plugin::host_event_source_adapter::host_event_source_adapter(std::shared_ptr<plugin::impl> impl)
    : impl_{std::move(impl)} {}

forge::net::p2p::host_event plugin::host_event_source_adapter::reachability_status() const {
   const auto current = impl_->require_running_node();
   return current->reachability_status();
}

forge::net::p2p::host_event_subscription plugin::host_event_source_adapter::host_events() const {
   const auto current = impl_->require_running_node();
   return current->host_events();
}

} // namespace forge::plugins::net::p2p::node
