module;

#include <forge/exceptions/macros.hpp>

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
import forge.exceptions;
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
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/diagnostics_policy.hxx"
#include "details/plugin_diagnostics_api_impl.hxx"

namespace forge::plugins::net::p2p::node {

plugin::diagnostics_api_impl::diagnostics_api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

forge::net::p2p::diagnostics::snapshot plugin::diagnostics_api_impl::take_snapshot(
    std::optional<forge::net::p2p::diagnostics::options> options) const {
   auto current = std::shared_ptr<forge::net::p2p::node>{};
   {
      const auto lock = std::scoped_lock{impl_->configuration_mutex};
      current = require_node();
      if (!options) {
         options = detail::configured_options(impl_->configuration->diagnostics);
      }
   }
   // Native owner and options are captured before delegation; no lifecycle mutex is held.
   return current->diagnostics(*options);
}

std::shared_ptr<forge::net::p2p::node> plugin::diagnostics_api_impl::require_node() const {
   auto current = impl_->node_snapshot();
   if (!current) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P diagnostics is not initialized");
   }
   return current;
}

forge::net::p2p::diagnostics::snapshot plugin::diagnostics_api_impl::snapshot() const {
   return take_snapshot();
}

forge::net::p2p::diagnostics::snapshot plugin::diagnostics_api_impl::snapshot(forge::net::p2p::diagnostics::options options) const {
   return take_snapshot(options);
}

forge::net::p2p::diagnostics::network_state plugin::diagnostics_api_impl::network() const {
   return take_snapshot().network;
}

forge::net::p2p::resource_manager::snapshot plugin::diagnostics_api_impl::resources() const {
   return take_snapshot().resources;
}

forge::net::p2p::pubsub::snapshot plugin::diagnostics_api_impl::pubsub() const {
   return take_snapshot().pubsub;
}

forge::net::p2p::dht::routing_status
plugin::diagnostics_api_impl::routing_status(const forge::net::p2p::protocol_id& profile) const {
   const auto source = require_node();
   return source->routing_status(profile);
}

forge::net::p2p::pubsub::score_snapshot plugin::diagnostics_api_impl::pubsub_scores() const {
   const auto source = require_node();
   return source->pubsub_scores();
}

std::vector<forge::net::p2p::diagnostics::peer> plugin::diagnostics_api_impl::peers(peer_filter value) const {
   return detail::filter_peers(take_snapshot(), value);
}

forge::net::p2p::diagnostics::peer plugin::diagnostics_api_impl::peer(forge::net::p2p::peer_id value) const {
   auto values = peers(peer_filter{.peer = std::move(value), .limit = 1});
   if (values.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::not_found, "P2P diagnostics peer was not found");
   }
   return std::move(values.front());
}

} // namespace forge::plugins::net::p2p::node
