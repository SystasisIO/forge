module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.net.p2p.identity;
import forge.net.p2p.pubsub;

#include "details/pubsub_peer_score.hxx"

namespace forge::net::p2p::detail {

double pubsub_peer_score::topic_score(const topic_state& state, const pubsub::topic_score_params& p) const {
   auto value = 0.0;
   if (state.in_mesh) {
      const auto quanta = std::chrono::duration<double>{state.mesh_time}.count() /
                          std::chrono::duration<double>{p.time_in_mesh_quantum}.count();
      value = weighted(std::min(quanta, p.time_in_mesh_cap), p.time_in_mesh_weight);
   }
   value = add(value, weighted(state.first_deliveries, p.first_message_deliveries_weight));
   if (state.mesh_active && state.mesh_deliveries < p.mesh_message_deliveries_threshold) {
      value = add(value, weighted_square(p.mesh_message_deliveries_threshold - state.mesh_deliveries,
                                         p.mesh_message_deliveries_weight));
   }
   value = add(value, weighted(state.mesh_failure, p.mesh_failure_penalty_weight));
   value = add(value, weighted_square(state.invalid_deliveries, p.invalid_message_deliveries_weight));
   return weighted(value, p.topic_weight);
}

double pubsub_peer_score::ip_factor(const peer_state& state) const {
   if (_params.ip_colocation_factor_weight == 0.0) {
      return 0.0;
   }
   auto factor = 0.0;
   for (const auto& ip : state.ips) {
      if (std::any_of(_params.ip_colocation_factor_allowlist.begin(),
                      _params.ip_colocation_factor_allowlist.end(),
                      [&](const auto& allowed) { return canonical_ip(allowed) == ip; })) {
         continue;
      }
      const auto peers = _ip_peers.find(ip)->second;
      const auto excess = static_cast<double>(peers) - _params.ip_colocation_factor_threshold;
      if (excess > 0.0) {
         factor = add(factor, squared(excess));
      }
   }
   return factor;
}

pubsub_peer_score::score_parts pubsub_peer_score::calculate(const peer_state& state) const {
   auto parts = score_parts{};
   for (const auto& [subject, topic] : state.topics) {
      parts.topics = add(parts.topics, topic_score(topic, _params.topics.at(subject)));
   }
   if (_params.topic_score_cap > 0.0 && parts.topics > _params.topic_score_cap) {
      parts.topics = _params.topic_score_cap;
   }
   parts.application = weighted(state.application_score, _params.app_specific_weight);
   parts.ip_factor = ip_factor(state);
   parts.ip_score = weighted(parts.ip_factor, _params.ip_colocation_factor_weight);
   const auto excess = std::max(0.0, state.behaviour_penalty - _params.behaviour_penalty_threshold);
   parts.behaviour = weighted_square(excess, _params.behaviour_penalty_weight);
   parts.total = add(add(add(parts.topics, parts.application), parts.ip_score), parts.behaviour);
   return parts;
}

double pubsub_peer_score::score_locked(const peer_state& state) const {
   return calculate(state).total;
}

pubsub::peer_score_snapshot pubsub_peer_score::inspect_locked(const peer_id& peer, const peer_state& state) const {
   const auto parts = calculate(state);
   auto result = pubsub::peer_score_snapshot{};
   result.peer = peer;
   result.connected = state.connected;
   if (!state.connected) {
      result.retain_until = state.retain_until;
   }
   result.value = parts.total;
   result.topic_score = parts.topics;
   result.app_specific_sample = state.application_score;
   result.app_specific_score = parts.application;
   result.ip_colocation_factor = parts.ip_factor;
   result.ip_colocation_score = parts.ip_score;
   result.behaviour_penalty = state.behaviour_penalty;
   result.behaviour_score = parts.behaviour;
   result.ips = state.ips;
   result.topics.reserve(state.topics.size());
   for (const auto& [subject, topic] : state.topics) {
      result.topics.push_back({
          .subject = subject,
          .in_mesh = topic.in_mesh,
          .mesh_deliveries_active = topic.mesh_active,
          .time_in_mesh = topic.mesh_time,
          .first_message_deliveries = topic.first_deliveries,
          .mesh_message_deliveries = topic.mesh_deliveries,
          .mesh_failure_penalty = topic.mesh_failure,
          .invalid_message_deliveries = topic.invalid_deliveries,
          .weighted_score = topic_score(topic, _params.topics.at(subject)),
      });
   }
   return result;
}

} // namespace forge::net::p2p::detail
