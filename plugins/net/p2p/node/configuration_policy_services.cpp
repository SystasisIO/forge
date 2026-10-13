module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.config.core.component;
import forge.exceptions;
import forge.net.p2p.mdns_policy;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.relay;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;

#include "details/configuration_policy.hxx"

namespace forge::plugins::net::p2p::node {

void apply_reachability(forge::net::p2p::node::options& options, const reachability_config& value) {
   auto& policy = options.reachability_policy;
   apply_bool(policy.client_v1_enabled, value.client_v1_enabled);
   apply_bool(policy.client_v2_enabled, value.client_v2_enabled);
   apply_bool(policy.service_v1_enabled, value.service_v1_enabled);
   apply_bool(policy.service_v2_enabled, value.service_v2_enabled);
   apply_time(policy.timeout, value.timeout_ms);
   apply_time(policy.refresh_interval, value.refresh_interval_ms);
   apply_time(policy.observation_ttl, value.observation_ttl_ms);
   apply_size(policy.min_observers, value.min_observers, "reachability.min-observers");
   apply_size(policy.min_reachability_observers, value.min_reachability_observers,
              "reachability.min-reachability-observers");
   apply_size(policy.max_observers, value.max_observers, "reachability.max-observers");
   apply_size(policy.max_observations, value.max_observations, "reachability.max-observations");
   apply_size(policy.max_candidates, value.max_candidates, "reachability.max-candidates");
   apply_size(policy.max_confirmed_per_local, value.max_confirmed_per_local, "reachability.max-confirmed-per-local");
   apply_size(policy.max_pending_probes, value.max_pending_probes, "reachability.max-pending-probes");
   apply_size(policy.service_per_peer_limit, value.service_per_peer_limit, "reachability.service-per-peer-limit");
   apply_size(policy.service_global_limit, value.service_global_limit, "reachability.service-global-limit");
   apply_time(policy.service_rate_interval, value.service_rate_interval_ms);
   apply_bool(policy.ping_enabled, value.ping_enabled);
   apply_time(policy.ping_interval, value.ping_interval_ms);
   apply_time(policy.ping_timeout, value.ping_timeout_ms);
   apply_size(policy.max_parallel_pings, value.max_parallel_pings, "reachability.max-parallel-pings");
   apply_size(policy.max_event_subscribers, value.max_event_subscribers, "reachability.max-event-subscribers");
}

void apply_mdns(forge::net::p2p::node::options& options, const mdns_config& value) {
   auto& policy = options.mdns;
   apply_bool(policy.enabled, value.enabled);
   apply_bool(policy.ipv4_enabled, value.ipv4_enabled);
   apply_bool(policy.ipv6_enabled, value.ipv6_enabled);
   if (value.record_ttl_seconds) {
      policy.record_ttl = to_seconds(*value.record_ttl_seconds);
   }
   apply_time(policy.query_interval, value.query_interval_ms);
   apply_size(policy.max_interfaces, value.max_interfaces, "mdns.max-interfaces");
   apply_size(policy.max_peers, value.max_peers, "mdns.max-peers");
   apply_size(policy.max_addresses_per_peer, value.max_addresses_per_peer, "mdns.max-addresses-per-peer");
   apply_size(policy.max_pending_packets, value.max_pending_packets, "mdns.max-pending-packets");
   apply_size(policy.max_packet_size, value.max_packet_size, "mdns.max-packet-size");
   apply_size(policy.max_records_per_packet, value.max_records_per_packet, "mdns.max-records-per-packet");
}

void apply_relay(forge::net::p2p::node::options& options, const relay_config& value, const parsed_policy& policy) {
   auto& relay = options.relay_policy;
   auto& limits = options.limits.relay;
   relay.client_enabled = policy.relay_client_enabled;
   relay.service_enabled = policy.relay_server_enabled;
   relay.auto_discovery_enabled = value.auto_discovery_enabled;
   relay.public_relay_allowed = policy.relay_public_allowed;
   relay.max_candidates_per_refresh = policy.relay_max_candidates;
   relay.target_reservations = std::min(relay.target_reservations, policy.relay_max_candidates);
   if (relay.target_reservations == 0) { relay.target_reservations = 1; }
   limits.reservation_ttl = policy.relay_reservation_ttl;
   // Preserve the existing relay baseline before applying explicit sparse values.
   apply_size(relay.target_reservations, value.target_reservations, "relay.target-reservations");
   apply_time(relay.refresh_margin, value.refresh_margin_ms);
   apply_size(relay.max_parallel_reservations, value.max_parallel_reservations, "relay.max-parallel-reservations");
   apply_time(relay.candidate_backoff, value.candidate_backoff_ms);
   apply_size(limits.max_active_relays, value.max_active_relays, "relay.max-active-relays");
   apply_size(limits.max_reservations, value.max_reservations, "relay.max-reservations");
   apply_size(limits.max_streams_per_reservation, value.max_streams_per_reservation,
              "relay.max-streams-per-reservation");
   if (value.max_relay_bytes) { limits.max_relay_bytes = *value.max_relay_bytes; }
   apply_size(limits.max_queued_bytes, value.max_queued_bytes, "relay.max-queued-bytes");
   apply_time(limits.max_duration, value.max_duration_ms);
   apply_size(limits.max_reservations_per_ip, value.max_reservations_per_ip, "relay.max-reservations-per-ip");
   apply_size(limits.max_circuits_per_peer, value.max_circuits_per_peer, "relay.max-circuits-per-peer");
   apply_size(limits.max_service_requests, value.max_service_requests, "relay.max-service-requests");
   apply_size(limits.max_service_requests_per_peer, value.max_service_requests_per_peer,
              "relay.max-service-requests-per-peer");
   apply_size(limits.max_service_requests_per_ip, value.max_service_requests_per_ip,
              "relay.max-service-requests-per-ip");
   apply_time(limits.service_request_window, value.service_request_window_ms);
   apply_time(limits.handshake_timeout, value.handshake_timeout_ms);
}

} // namespace forge::plugins::net::p2p::node
