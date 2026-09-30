module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.diagnostics.plugin;

import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.app.plugin;
import forge.app.plugin_context;
import forge.config.core.component;
import forge.config.core.decode;
import forge.exceptions;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.identify;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.dht;
import forge.net.p2p.rendezvous;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.hole_punch;
import forge.net.p2p.protocol;
import forge.net.p2p.message;
import forge.net.p2p.scoring;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.node;
import forge.api.p2p.binding;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.diagnostics.api;
import forge.plugins.net.p2p.diagnostics.exceptions;
import forge.plugins.net.p2p.diagnostics.types;

#include "details/config.hxx"
#include "details/api_impl.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::diagnostics {

plugin::plugin() : impl_{std::make_shared<impl>()} {}
plugin::~plugin() = default;

forge::app::plugin_id plugin::id() const {
   return forge::app::plugin_id{.value = "forge.plugins.net.p2p.diagnostics"};
}

std::string plugin::version() const {
   return "2.0.0";
}

std::optional<forge::config::core::component_descriptor> plugin::describe_config() const {
   return forge::config::core::describe_component<config>("plugins.net.p2p.diagnostics");
}

boost::asio::awaitable<void> plugin::configure(forge::config::core::component_view view) {
   auto config = decode_config(view);
   validate_config(config);
   impl_->settings = std::move(config);
   co_return;
}

boost::asio::awaitable<void> plugin::provide(forge::api::core::provider& provider) {
   provider.install<api>(std::make_shared<api_impl>(impl_));
   co_return;
}

boost::asio::awaitable<void> plugin::initialize(forge::app::plugin_context& context) {
   impl_->source = context.apis()
                      .get<forge::plugins::net::p2p::node::diagnostics_source>(
                         {.id = {"forge.plugins.net.p2p.node.diagnostics_source"}, .major = 2, .min_revision = 0})
                      .shared();
   impl_->initialized = true;
   impl_->stopping = false;
   co_return;
}

boost::asio::awaitable<void> plugin::startup() {
   co_return;
}

void plugin::request_stop() noexcept {
   impl_->stopping = true;
}

boost::asio::awaitable<void> plugin::shutdown() {
   impl_->stopping = true;
   impl_->initialized = false;
   impl_->source = nullptr;
   co_return;
}

forge::app::plugin_descriptor descriptor() {
   return forge::app::plugin_descriptor{
      .id = forge::app::plugin_id{.value = "forge.plugins.net.p2p.diagnostics"},
      .dependencies = {forge::app::plugin_id{.value = "forge.plugins.net.p2p.node"}},
      .factory = [] {
         return std::make_unique<plugin>();
      },
   };
}

} // namespace forge::plugins::net::p2p::diagnostics
