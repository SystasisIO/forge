module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <cstddef>
#include <exception>
#include <limits>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>

module forge.plugins.net.p2p.node.plugin;

import forge.api.transport.options;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dht;
import forge.net.p2p.connection_gater;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.configuration_api;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.topology;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/configuration_policy.hxx"
#include "details/protocol_registry.hxx"

#include "details/configuration_api_impl.hxx"

namespace forge::plugins::net::p2p::node {

plugin::configuration_api_impl::configuration_api_impl(std::shared_ptr<plugin::impl> state)
    : state_(std::move(state)) {}

void plugin::configuration_api_impl::set_connection_gater(std::shared_ptr<forge::net::p2p::connection_gater> gater) {
   const auto previous = state_->configuration_candidate_base();
   if (!gater) { FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P connection gater must not be null"); }
   if (previous->options.connection_gater) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P connection gater is already configured");
   }
   auto candidate = std::make_shared<detail::configuration_snapshot>(*previous);
   candidate->options.connection_gater = std::move(gater);
   state_->commit_configuration(previous, std::move(candidate));
}

void plugin::configuration_api_impl::set_pubsub_options(forge::net::p2p::pubsub::options options) {
   const auto previous = state_->configuration_candidate_base();
   if (!previous->pubsub.enabled) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub contribution requires pubsub.enabled");
   }
   // Native callback copies, validation and retired-owner destruction occur outside the config lock.
   auto candidate = std::make_shared<detail::configuration_snapshot>(*previous);
   candidate->options.limits.pubsub = std::move(options);
   candidate->pubsub_contributed = true;
   state_->commit_configuration(previous, std::move(candidate));
}

void plugin::configuration_api_impl::set_dht_value_policies(forge::net::p2p::protocol_id profile,
    std::vector<forge::net::p2p::dht::value_policy> policies) {
   const auto previous = state_->configuration_candidate_base();
   const auto found = std::find_if(previous->dht_profiles.begin(), previous->dht_profiles.end(),
                                   [&](const auto& value) { return value.protocol == profile; });
   if (found == previous->dht_profiles.end() || found->kind != forge::net::p2p::dht::profile_kind::custom ||
       !found->capabilities.values) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, "DHT contribution requires a configured custom values profile");
   }
   if (!found->value_policies.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "DHT value policies are already configured");
   }
   auto candidate = std::make_shared<detail::configuration_snapshot>(*previous);
   auto& installed = candidate->dht_profiles[static_cast<std::size_t>(found - previous->dht_profiles.begin())];
   installed.value_policies = std::move(policies);
   try {
      forge::net::p2p::validate(installed);
   } catch (const forge::net::p2p::exceptions::invalid_options& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, error.what());
   }
   state_->commit_configuration(previous, std::move(candidate));
}

} // namespace forge::plugins::net::p2p::node

