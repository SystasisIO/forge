module;

#include <boost/describe.hpp>
#include <cstdint>
#include <memory>
#include <new>
#include <string>
#include <vector>

module forge.plugins.net.p2p.node.config;

import forge.schema.object;

forge::schema::object_schema<forge::plugins::net::p2p::node::dht_profile_config>
forge::schema::rules<forge::plugins::net::p2p::node::dht_profile_config>::define() {
   auto schema = forge::schema::object<forge::plugins::net::p2p::node::dht_profile_config>();
   schema.field<&forge::plugins::net::p2p::node::dht_profile_config::kind>("kind").default_value(
       forge::plugins::net::p2p::node::dht_profile_kind::amino_v1);
   schema.field<&forge::plugins::net::p2p::node::dht_profile_config::mode>("mode").default_value(
       forge::plugins::net::p2p::node::dht_mode::client);
   schema.field<&forge::plugins::net::p2p::node::dht_profile_config::protocol>("protocol").default_value("");
   schema.field<&forge::plugins::net::p2p::node::dht_profile_config::peers>("peers").default_value(true);
   schema.field<&forge::plugins::net::p2p::node::dht_profile_config::providers>("providers").default_value(true);
   schema.field<&forge::plugins::net::p2p::node::dht_profile_config::values>("values").default_value(true);
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::rendezvous_point_config>
forge::schema::rules<forge::plugins::net::p2p::node::rendezvous_point_config>::define() {
   auto schema = forge::schema::object<forge::plugins::net::p2p::node::rendezvous_point_config>();
   schema.field<&forge::plugins::net::p2p::node::rendezvous_point_config::endpoint>("endpoint");
   schema.field<&forge::plugins::net::p2p::node::rendezvous_point_config::namespaces>("namespaces")
       .default_value(std::vector<std::string>{})
       .max_items(1'000);
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::config>
forge::schema::rules<forge::plugins::net::p2p::node::config>::define() {
   auto schema = forge::schema::object<forge::plugins::net::p2p::node::config>();
   schema.field<&forge::plugins::net::p2p::node::config::pubsub>("pubsub")
       .default_value(forge::plugins::net::p2p::node::pubsub_config{});
   schema.field<&forge::plugins::net::p2p::node::config::diagnostics>("diagnostics")
       .default_value(forge::plugins::net::p2p::node::diagnostics_config{});
   schema.field<&forge::plugins::net::p2p::node::config::resources>("resources")
       .default_value(forge::plugins::net::p2p::node::resource_config{})
       .description("Sparse native resource-scope budgets; omitted values preserve defaults, zero denies");
   schema.field<&forge::plugins::net::p2p::node::config::connections>("connections")
       .default_value(forge::plugins::net::p2p::node::connection_config{});
   schema.field<&forge::plugins::net::p2p::node::config::identify>("identify")
       .default_value(forge::plugins::net::p2p::node::identify_config{});
   schema.field<&forge::plugins::net::p2p::node::config::peer_state>("peer-state")
       .default_value(forge::plugins::net::p2p::node::peer_state_config{});
   schema.field<&forge::plugins::net::p2p::node::config::listen>("listen")
       .default_value(std::vector<std::string>{})
       .description("Listen endpoints, for example /ip4/0.0.0.0/udp/9443/quic-v1 or /ip4/0.0.0.0/tcp/4001");
   schema.field<&forge::plugins::net::p2p::node::config::bootstrap>("bootstrap")
       .default_value(std::vector<std::string>{})
       .description("Bootstrap peer endpoints as libp2p multiaddr text");
   schema.field<&forge::plugins::net::p2p::node::config::bootstrap_requirement>("bootstrap-requirement")
       .default_value("allow-disconnected")
       .description("Startup connectivity policy: allow-disconnected or require-connection");
   schema.field<&forge::plugins::net::p2p::node::config::bootstrap_startup_budget_ms>("bootstrap-startup-budget-ms")
       .default_value(std::uint64_t{10'000})
       .range(1, 600'000);
   schema.field<&forge::plugins::net::p2p::node::config::bootstrap_connect_timeout_ms>("bootstrap-connect-timeout-ms")
       .default_value(std::uint64_t{2'000})
       .range(1, 300'000);
   schema.field<&forge::plugins::net::p2p::node::config::bootstrap_max_parallel>("bootstrap-max-parallel")
       .default_value(std::uint64_t{4})
       .range(1, 256);
   schema.field<&forge::plugins::net::p2p::node::config::advertised_endpoints>("advertised-endpoints")
       .default_value(std::vector<std::string>{})
       .description("Endpoints advertised to peers as libp2p multiaddr text");
   schema.field<&forge::plugins::net::p2p::node::config::peer_id>("peer-id").default_value("");
   schema.field<&forge::plugins::net::p2p::node::config::peer_store>("peer-store.store").default_value("");
   schema.field<&forge::plugins::net::p2p::node::config::peer_store_schema_policy>("peer-store.schema-policy")
       .default_value(forge::plugins::net::p2p::node::cache_schema_policy::reset)
       .description("Recoverable cache schema handling: reset or reject");
   schema.field<&forge::plugins::net::p2p::node::config::dht_profiles>("dht.profiles")
       .items<forge::plugins::net::p2p::node::dht_profile_config>()
       .default_value(std::vector<forge::plugins::net::p2p::node::dht_profile_config>{})
       .max_items(64)
       .description("Independent Amino or product Kademlia profiles");
   schema.field<&forge::plugins::net::p2p::node::config::topology_mode>("topology.mode")
       .default_value(forge::plugins::net::p2p::node::topology_mode::managed);
   schema.field<&forge::plugins::net::p2p::node::config::topology_low>("topology.peers.low")
       .default_value(std::uint64_t{128})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_target>("topology.peers.target")
       .default_value(std::uint64_t{160})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_high>("topology.peers.high")
       .default_value(std::uint64_t{192})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_refresh_interval_ms>("topology.refresh-interval-ms")
       .default_value(std::uint64_t{600'000})
       .range(1, 86'400'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_query_timeout_ms>("topology.query-timeout-ms")
       .default_value(std::uint64_t{10'000})
       .range(1, 600'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_max_candidates>("topology.max-candidates")
       .default_value(std::uint64_t{256})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_max_parallel_queries>(
             "topology.max-parallel-queries")
       .default_value(std::uint64_t{10})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_max_parallel_dials>("topology.max-parallel-dials")
       .default_value(std::uint64_t{4})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::topology_retry_jitter>("topology.retry-jitter")
       .default_value(0.20)
       .range(0.0, 0.999999);
   schema.field<&forge::plugins::net::p2p::node::config::rendezvous_role>("rendezvous.role")
       .default_value(forge::plugins::net::p2p::node::rendezvous_role::disabled);
   schema.field<&forge::plugins::net::p2p::node::config::rendezvous_points>("rendezvous.points")
       .items<forge::plugins::net::p2p::node::rendezvous_point_config>()
       .default_value(std::vector<forge::plugins::net::p2p::node::rendezvous_point_config>{})
       .max_items(1'000);
   schema.field<&forge::plugins::net::p2p::node::config::rendezvous_max_points>("rendezvous.max-points")
       .default_value(std::uint64_t{4})
       .range(1, 1'000);
   schema.field<&forge::plugins::net::p2p::node::config::peer_exchange_enabled>("peer-exchange.enabled")
       .default_value(true);
   schema.field<&forge::plugins::net::p2p::node::config::peer_exchange_max_peers>("peer-exchange.max-peers")
       .default_value(std::uint64_t{4})
       .range(1, 1'000);
   schema.field<&forge::plugins::net::p2p::node::config::certificate_secret>("identity.certificate-secret")
       .default_value("");
   schema.field<&forge::plugins::net::p2p::node::config::private_key_secret>("identity.private-key-secret")
       .default_value("");
   schema.field<&forge::plugins::net::p2p::node::config::api_codec>("api-codec").default_value("forge.raw");
   schema.field<&forge::plugins::net::p2p::node::config::api_deadline_ms>("api.deadline-ms")
       .default_value(std::uint64_t{0})
       .range(0, 86'400'000);
   schema.field<&forge::plugins::net::p2p::node::config::api_max_frame_size>("api.max-frame-size")
       .default_value(std::uint64_t{16 * 1024 * 1024})
       .range(1, 1024 * 1024 * 1024);
   schema.field<&forge::plugins::net::p2p::node::config::max_inflight_per_peer>("max-inflight-per-peer")
       .default_value(std::uint64_t{64})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::max_sessions>("max-sessions")
       .default_value(std::uint64_t{1024})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::max_protocol_handlers>("max-protocol-handlers")
       .default_value(std::uint64_t{1024})
       .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::config::allow_insecure_test_mode>("allow-insecure-test-mode")
       .default_value(false)
       .description("Test-only mode for local development without deployment identity material");
   schema.field<&forge::plugins::net::p2p::node::config::path_policy>("path.policy")
       .default_value("direct-preferred")
       .description("Default host path policy: direct-only, direct-preferred or relay-only");
   schema.field<&forge::plugins::net::p2p::node::config::relay_trust>("relay.trust")
       .default_value("known-only")
       .description("Relay trust policy: known-only or public-allowed");
   schema.field<&forge::plugins::net::p2p::node::config::relay_client_enabled>("relay.client-enabled")
       .default_value(true);
   schema.field<&forge::plugins::net::p2p::node::config::relay_server_enabled>("relay.server-enabled")
       .default_value(false);
   schema.field<&forge::plugins::net::p2p::node::config::relay_public_allowed>("relay.public-allowed")
       .default_value(false);
   schema.field<&forge::plugins::net::p2p::node::config::relay_reservation_ttl_ms>("relay.reservation-ttl-ms")
       .default_value(std::uint64_t{60'000})
       .range(1, 86'400'000);
   schema.field<&forge::plugins::net::p2p::node::config::relay_max_candidates>("relay.max-candidates")
       .default_value(std::uint64_t{4})
       .range(1, 10'000);
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::diagnostics_config>
forge::schema::rules<forge::plugins::net::p2p::node::diagnostics_config>::define() {
   auto schema = forge::schema::object<forge::plugins::net::p2p::node::diagnostics_config>();
   schema.field<&forge::plugins::net::p2p::node::diagnostics_config::max_peers>("max-peers")
      .default_value(std::uint64_t{1'024})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::diagnostics_config::max_sessions>("max-sessions")
      .default_value(std::uint64_t{1'024})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::diagnostics_config::max_endpoints_per_peer>("max-endpoints-per-peer")
      .default_value(std::uint64_t{64})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::diagnostics_config::max_protocols_per_peer>("max-protocols-per-peer")
      .default_value(std::uint64_t{128})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::diagnostics_config::max_relay_reservations_per_peer>(
      "max-relay-reservations-per-peer")
      .default_value(std::uint64_t{64})
      .range(1, 1'000'000);
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::resource_config::scope_patch>
forge::schema::rules<forge::plugins::net::p2p::node::resource_config::scope_patch>::define() {
   using patch = forge::plugins::net::p2p::node::resource_config::scope_patch;
   auto schema = forge::schema::object<patch>();
   schema.field<&patch::max_memory>("max-memory").description("Memory budget in bytes; zero denies allocation");
   schema.field<&patch::max_file_descriptors>("max-file-descriptors");
   schema.field<&patch::max_inbound_connections>("max-inbound-connections");
   schema.field<&patch::max_outbound_connections>("max-outbound-connections");
   schema.field<&patch::max_connections>("max-connections");
   schema.field<&patch::max_inbound_streams>("max-inbound-streams");
   schema.field<&patch::max_outbound_streams>("max-outbound-streams");
   schema.field<&patch::max_streams>("max-streams");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::resource_config>
forge::schema::rules<forge::plugins::net::p2p::node::resource_config>::define() {
   using config = forge::plugins::net::p2p::node::resource_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::system>("system").default_value(config::scope_patch{});
   schema.field<&config::transient>("transient").default_value(config::scope_patch{});
   schema.field<&config::peer>("peer").default_value(config::scope_patch{});
   schema.field<&config::protocol>("protocol").default_value(config::scope_patch{});
   schema.field<&config::service>("service").default_value(config::scope_patch{});
   schema.field<&config::protocol_peer>("protocol-peer").default_value(config::scope_patch{});
   schema.field<&config::service_peer>("service-peer").default_value(config::scope_patch{});
   schema.field<&config::connection>("connection").default_value(config::scope_patch{});
   schema.field<&config::stream>("stream").default_value(config::scope_patch{});
   schema.field<&config::max_dial_attempts>("max-dial-attempts");
   schema.field<&config::max_dial_attempts_per_peer>("max-dial-attempts-per-peer");
   schema.field<&config::max_relay_reservations>("max-relay-reservations");
   schema.field<&config::max_malformed_messages_per_peer>("max-malformed-messages-per-peer");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config>
forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config>::define() {
   auto schema = forge::schema::object<forge::plugins::net::p2p::node::pubsub_config>();
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::max_topics>("max-topics")
      .default_value(std::uint64_t{1'024})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::max_handlers_per_topic>("max-handlers-per-topic")
      .default_value(std::uint64_t{64})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::max_active_handlers>("max-active-handlers")
      .default_value(std::uint64_t{4'096})
      .range(1, 1'000'000);
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::max_message_size>("max-message-size")
      .default_value(std::uint64_t{1024 * 1024})
      .range(1, 1024 * 1024 * 1024);
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::handler_deadline_ms>("handler-deadline-ms")
      .default_value(std::uint64_t{5'000})
      .range(1, 86'400'000);
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::allowed_topics>("allowed-topics")
      .default_value(std::vector<std::string>{})
      .each_non_empty();
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::denied_topics>("denied-topics")
      .default_value(std::vector<std::string>{})
      .each_non_empty();
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::sign_publishes>("sign-publishes").default_value(true);
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::partial_messages>("partial-messages").default_value(false);
   schema.field<&forge::plugins::net::p2p::node::pubsub_config::enabled>("enabled").default_value(false);
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::connection_config>
forge::schema::rules<forge::plugins::net::p2p::node::connection_config>::define() {
   using config = forge::plugins::net::p2p::node::connection_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::session_low_watermark>("session-low-watermark");
   schema.field<&config::session_grace_period_ms>("session-grace-period-ms");
   schema.field<&config::session_prune_silence_ms>("session-prune-silence-ms");
   schema.field<&config::dial_backoff_base_ms>("dial-backoff-base-ms");
   schema.field<&config::dial_backoff_step_ms>("dial-backoff-step-ms");
   schema.field<&config::dial_backoff_max_ms>("dial-backoff-max-ms");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::identify_config>
forge::schema::rules<forge::plugins::net::p2p::node::identify_config>::define() {
   using config = forge::plugins::net::p2p::node::identify_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::timeout_ms>("timeout-ms");
   schema.field<&config::max_message_size>("max-message-size");
   schema.field<&config::max_total_message_size>("max-total-message-size");
   schema.field<&config::max_own_message_size>("max-own-message-size");
   schema.field<&config::max_message_parts>("max-message-parts");
   schema.field<&config::max_protocols>("max-protocols");
   schema.field<&config::max_listen_endpoints>("max-listen-endpoints");
   schema.field<&config::max_protocol_size>("max-protocol-size");
   schema.field<&config::max_version_size>("max-version-size");
   schema.field<&config::max_public_key_size>("max-public-key-size");
   schema.field<&config::max_signed_peer_record_size>("max-signed-peer-record-size");
   schema.field<&config::max_push_operations>("max-push-operations");
   schema.field<&config::protocol_version>("protocol-version");
   schema.field<&config::agent_version>("agent-version");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::peer_state_config>
forge::schema::rules<forge::plugins::net::p2p::node::peer_state_config>::define() {
   using config = forge::plugins::net::p2p::node::peer_state_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_peers>("max-peers");
   schema.field<&config::max_rendezvous>("max-rendezvous");
   schema.field<&config::max_pending>("max-pending");
   schema.field<&config::max_endpoints_per_peer>("max-endpoints-per-peer");
   schema.field<&config::max_protocols_per_peer>("max-protocols-per-peer");
   schema.field<&config::max_relay_reservations_per_peer>("max-relay-reservations-per-peer");
   schema.field<&config::max_relay_endpoints_per_reservation>("max-relay-endpoints-per-reservation");
   schema.field<&config::max_peer_record_bytes>("max-peer-record-bytes");
   schema.field<&config::hydration_page_limit>("hydration-page-limit");
   schema.field<&config::prune_page_limit>("prune-page-limit");
   schema.field<&config::max_persistence_waiters>("max-persistence-waiters");
   return schema;
}
