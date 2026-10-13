module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/ip/address.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.net.p2p.exceptions;
import forge.net.p2p.pubsub;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;

#include "details/pubsub_policy.hxx"

namespace forge::plugins::net::p2p::node::detail {
namespace {

void patch_double(double& target, const std::optional<double>& value) {
   if (value) { target = *value; }
}

forge::net::p2p::pubsub::topic_score_params topic_params_for(
    const pubsub_config::scoring_config::topic_config& settings) {
   auto out = forge::net::p2p::pubsub::topic_score_params{};
   patch_double(out.topic_weight, settings.topic_weight);
   patch_double(out.time_in_mesh_weight, settings.time_in_mesh_weight);
   patch_time(out.time_in_mesh_quantum, settings.time_in_mesh_quantum_ms);
   patch_double(out.time_in_mesh_cap, settings.time_in_mesh_cap);
   patch_double(out.first_message_deliveries_weight, settings.first_message_deliveries_weight);
   patch_double(out.first_message_deliveries_decay, settings.first_message_deliveries_decay);
   patch_double(out.first_message_deliveries_cap, settings.first_message_deliveries_cap);
   patch_double(out.mesh_message_deliveries_weight, settings.mesh_message_deliveries_weight);
   patch_double(out.mesh_message_deliveries_decay, settings.mesh_message_deliveries_decay);
   patch_double(out.mesh_message_deliveries_cap, settings.mesh_message_deliveries_cap);
   patch_double(out.mesh_message_deliveries_threshold, settings.mesh_message_deliveries_threshold);
   patch_time(out.mesh_message_deliveries_window, settings.mesh_message_deliveries_window_ms);
   patch_time(out.mesh_message_deliveries_activation, settings.mesh_message_deliveries_activation_ms);
   patch_double(out.mesh_failure_penalty_weight, settings.mesh_failure_penalty_weight);
   patch_double(out.mesh_failure_penalty_decay, settings.mesh_failure_penalty_decay);
   patch_double(out.invalid_message_deliveries_weight, settings.invalid_message_deliveries_weight);
   patch_double(out.invalid_message_deliveries_decay, settings.invalid_message_deliveries_decay);
   return out;
}

} // namespace

std::optional<forge::net::p2p::pubsub::scoring_params> pubsub_scoring_for(
    const std::optional<pubsub_config::scoring_config>& settings) {
   if (!settings) { return std::nullopt; }
   const auto& value = *settings;
   auto out = forge::net::p2p::pubsub::scoring_params{};
   for (const auto& topic : value.topics) {
      if (!out.topics.emplace(forge::net::p2p::pubsub::topic{topic.topic}, topic_params_for(topic)).second) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_config, "duplicate P2P PubSub scoring topic");
      }
   }
   patch_double(out.topic_score_cap, value.topic_score_cap);
   patch_double(out.ip_colocation_factor_weight, value.ip_colocation_factor_weight);
   patch_double(out.ip_colocation_factor_threshold, value.ip_colocation_factor_threshold);
   if (value.ip_colocation_factor_allowlist) {
      for (const auto& text : *value.ip_colocation_factor_allowlist) {
         auto error = boost::system::error_code{};
         const auto address = boost::asio::ip::make_address(text, error);
         if (error) {
            FORGE_THROW_EXCEPTION(exceptions::invalid_config, "P2P PubSub scoring IP allowlist requires numeric addresses");
         }
         out.ip_colocation_factor_allowlist.push_back(address);
      }
   }
   patch_double(out.behaviour_penalty_weight, value.behaviour_penalty_weight);
   patch_double(out.behaviour_penalty_threshold, value.behaviour_penalty_threshold);
   patch_double(out.behaviour_penalty_decay, value.behaviour_penalty_decay);
   patch_time(out.decay_interval, value.decay_interval_ms);
   patch_double(out.decay_to_zero, value.decay_to_zero);
   patch_time(out.retain_score, value.retain_score_ms);
   patch_time(out.seen_message_ttl, value.seen_message_ttl_ms);
   patch_double(out.thresholds.gossip_threshold, value.thresholds.gossip_threshold);
   patch_double(out.thresholds.publish_threshold, value.thresholds.publish_threshold);
   patch_double(out.thresholds.graylist_threshold, value.thresholds.graylist_threshold);
   patch_double(out.thresholds.accept_px_threshold, value.thresholds.accept_px_threshold);
   patch_double(out.thresholds.opportunistic_graft_threshold, value.thresholds.opportunistic_graft_threshold);
   patch_size(out.limits.max_connected_peers, value.limits.max_connected_peers);
   patch_size(out.limits.max_retained_peers, value.limits.max_retained_peers);
   patch_size(out.limits.max_messages, value.limits.max_messages);
   patch_size(out.limits.max_delivery_peers, value.limits.max_delivery_peers);
   try {
      // Validate supplied policy even when disabled; enabled alone decides whether it is installed.
      forge::net::p2p::pubsub::validate(out);
   } catch (const forge::net::p2p::exceptions::invalid_options& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_config, error.what());
   }
   return value.enabled ? std::optional{std::move(out)} : std::nullopt;
}

} // namespace forge::plugins::net::p2p::node::detail
