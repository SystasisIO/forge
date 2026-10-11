module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
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
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.topology;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.plugins.net.p2p.node.events_api;

#include "details/plugin_impl.hxx"
#include "details/plugin_events_api_impl.hxx"

namespace forge::plugins::net::p2p::node {

plugin::events_api_impl::events_api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

forge::net::p2p::host_event plugin::events_api_impl::reachability_status() const {
   const auto current = impl_->require_running_node();
   return current->reachability_status();
}

forge::net::p2p::host_event_subscription plugin::events_api_impl::host_events() const {
   const auto current = impl_->require_running_node();
   return current->host_events();
}

} // namespace forge::plugins::net::p2p::node
