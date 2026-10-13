module;

#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

module forge.plugins.net.p2p.node.config;

import forge.schema.object;

forge::schema::object_schema<forge::plugins::net::p2p::node::reachability_config>
forge::schema::rules<forge::plugins::net::p2p::node::reachability_config>::define() {
   using config = forge::plugins::net::p2p::node::reachability_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::client_v1_enabled>("client-v1-enabled");
   schema.field<&config::client_v2_enabled>("client-v2-enabled");
   schema.field<&config::service_v1_enabled>("service-v1-enabled");
   schema.field<&config::service_v2_enabled>("service-v2-enabled");
   schema.field<&config::timeout_ms>("timeout-ms");
   schema.field<&config::refresh_interval_ms>("refresh-interval-ms");
   schema.field<&config::observation_ttl_ms>("observation-ttl-ms");
   schema.field<&config::min_observers>("min-observers");
   schema.field<&config::min_reachability_observers>("min-reachability-observers");
   schema.field<&config::max_observers>("max-observers");
   schema.field<&config::max_observations>("max-observations");
   schema.field<&config::max_candidates>("max-candidates");
   schema.field<&config::max_confirmed_per_local>("max-confirmed-per-local");
   schema.field<&config::max_pending_probes>("max-pending-probes");
   schema.field<&config::service_per_peer_limit>("service-per-peer-limit");
   schema.field<&config::service_global_limit>("service-global-limit");
   schema.field<&config::service_rate_interval_ms>("service-rate-interval-ms");
   schema.field<&config::ping_enabled>("ping-enabled");
   schema.field<&config::ping_interval_ms>("ping-interval-ms");
   schema.field<&config::ping_timeout_ms>("ping-timeout-ms");
   schema.field<&config::max_parallel_pings>("max-parallel-pings");
   schema.field<&config::max_event_subscribers>("max-event-subscribers");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::mdns_config>
forge::schema::rules<forge::plugins::net::p2p::node::mdns_config>::define() {
   using config = forge::plugins::net::p2p::node::mdns_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::enabled>("enabled");
   schema.field<&config::ipv4_enabled>("ipv4-enabled");
   schema.field<&config::ipv6_enabled>("ipv6-enabled");
   schema.field<&config::record_ttl_seconds>("record-ttl-seconds");
   schema.field<&config::query_interval_ms>("query-interval-ms");
   schema.field<&config::max_interfaces>("max-interfaces");
   schema.field<&config::max_peers>("max-peers");
   schema.field<&config::max_addresses_per_peer>("max-addresses-per-peer");
   schema.field<&config::max_pending_packets>("max-pending-packets");
   schema.field<&config::max_packet_size>("max-packet-size");
   schema.field<&config::max_records_per_packet>("max-records-per-packet");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::relay_config>
forge::schema::rules<forge::plugins::net::p2p::node::relay_config>::define() {
   using config = forge::plugins::net::p2p::node::relay_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::trust>("trust").default_value("known-only")
       .description("Relay trust policy: known-only or public-allowed");
   schema.field<&config::client_enabled>("client-enabled").default_value(true);
   schema.field<&config::server_enabled>("server-enabled").default_value(false);
   schema.field<&config::auto_discovery_enabled>("auto-discovery-enabled").default_value(true);
   schema.field<&config::public_allowed>("public-allowed").default_value(false);
   schema.field<&config::reservation_ttl_ms>("reservation-ttl-ms")
       .default_value(std::uint64_t{60'000}).range(1, 86'400'000);
   schema.field<&config::max_candidates>("max-candidates").default_value(std::uint64_t{4}).range(1, 10'000);
   schema.field<&config::target_reservations>("target-reservations");
   schema.field<&config::refresh_margin_ms>("refresh-margin-ms");
   schema.field<&config::max_parallel_reservations>("max-parallel-reservations");
   schema.field<&config::candidate_backoff_ms>("candidate-backoff-ms");
   schema.field<&config::max_active_relays>("max-active-relays");
   schema.field<&config::max_reservations>("max-reservations");
   schema.field<&config::max_streams_per_reservation>("max-streams-per-reservation");
   schema.field<&config::max_relay_bytes>("max-relay-bytes");
   schema.field<&config::max_queued_bytes>("max-queued-bytes");
   schema.field<&config::max_duration_ms>("max-duration-ms");
   schema.field<&config::max_reservations_per_ip>("max-reservations-per-ip");
   schema.field<&config::max_circuits_per_peer>("max-circuits-per-peer");
   schema.field<&config::max_service_requests>("max-service-requests");
   schema.field<&config::max_service_requests_per_peer>("max-service-requests-per-peer");
   schema.field<&config::max_service_requests_per_ip>("max-service-requests-per-ip");
   schema.field<&config::service_request_window_ms>("service-request-window-ms");
   schema.field<&config::handshake_timeout_ms>("handshake-timeout-ms");
   return schema;
}
