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
   schema.field<&forge::plugins::net::p2p::node::dht_profile_config::records>("records").default_value(
       forge::plugins::net::p2p::node::dht_profile_config::records_config{});
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dht_profile_config::records_config>
forge::schema::rules<forge::plugins::net::p2p::node::dht_profile_config::records_config>::define() {
   using config = forge::plugins::net::p2p::node::dht_profile_config::records_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_values>("max-values");
   schema.field<&config::max_total_bytes>("max-total-bytes");
   schema.field<&config::hydration_page_limit>("hydration-page-limit");
   schema.field<&config::prune_page_limit>("prune-page-limit");
   schema.field<&config::max_hydration_pages>("max-hydration-pages");
   schema.field<&config::max_providers>("max-providers");
   schema.field<&config::max_local_providers>("max-local-providers");
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
   schema.field<&forge::plugins::net::p2p::node::config::tcp>("tcp")
       .default_value(forge::plugins::net::p2p::node::tcp_config{});
   schema.field<&forge::plugins::net::p2p::node::config::quic>("quic")
       .default_value(forge::plugins::net::p2p::node::quic_config{});
   schema.field<&forge::plugins::net::p2p::node::config::private_network>("private-network");
   schema.field<&forge::plugins::net::p2p::node::config::dns>("dns")
       .default_value(forge::plugins::net::p2p::node::dns_config{});
   schema.field<&forge::plugins::net::p2p::node::config::dial>("dial")
       .default_value(forge::plugins::net::p2p::node::dial_config{});
   schema.field<&forge::plugins::net::p2p::node::config::reachability>("reachability")
       .default_value(forge::plugins::net::p2p::node::reachability_config{});
   schema.field<&forge::plugins::net::p2p::node::config::mdns>("mdns")
       .default_value(forge::plugins::net::p2p::node::mdns_config{});
   schema.field<&forge::plugins::net::p2p::node::config::listen>("listen")
       .default_value(std::vector<std::string>{})
       .description("Listen endpoints, for example /ip4/0.0.0.0/udp/9443/quic-v1 or /ip4/0.0.0.0/tcp/4001");
   schema.field<&forge::plugins::net::p2p::node::config::bootstrap>("bootstrap")
       .default_value(std::vector<std::string>{})
       .description("Bootstrap peer endpoints as libp2p multiaddr text");
   schema.field<&forge::plugins::net::p2p::node::config::bootstrap_policy>("bootstrap-policy")
       .default_value(forge::plugins::net::p2p::node::bootstrap_policy_config{});
   schema.field<&forge::plugins::net::p2p::node::config::advertised_endpoints>("advertised-endpoints")
       .default_value(std::vector<std::string>{})
       .description("Endpoints advertised to peers as libp2p multiaddr text");
   schema.field<&forge::plugins::net::p2p::node::config::peer_id>("peer-id").default_value("");
   schema.field<&forge::plugins::net::p2p::node::config::peer_store>("peer-store")
       .default_value(forge::plugins::net::p2p::node::peer_store_config{});
   schema.field<&forge::plugins::net::p2p::node::config::dht_profiles>("dht.profiles")
       .items<forge::plugins::net::p2p::node::dht_profile_config>()
       .default_value(std::vector<forge::plugins::net::p2p::node::dht_profile_config>{})
       .max_items(64)
       .description("Independent Amino or product Kademlia profiles");
   schema.field<&forge::plugins::net::p2p::node::config::topology>("topology")
       .default_value(forge::plugins::net::p2p::node::topology_config{});
   schema.field<&forge::plugins::net::p2p::node::config::rendezvous>("rendezvous")
       .default_value(forge::plugins::net::p2p::node::rendezvous_config{});
   schema.field<&forge::plugins::net::p2p::node::config::peer_exchange>("peer-exchange")
       .default_value(forge::plugins::net::p2p::node::peer_exchange_config{});
   schema.field<&forge::plugins::net::p2p::node::config::identity>("identity")
       .default_value(forge::plugins::net::p2p::node::identity_config{});
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
   schema.field<&forge::plugins::net::p2p::node::config::path>("path")
       .default_value(forge::plugins::net::p2p::node::path_config{});
   schema.field<&forge::plugins::net::p2p::node::config::relay>("relay")
       .default_value(forge::plugins::net::p2p::node::relay_config{});
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config::resolution_config>
forge::schema::rules<forge::plugins::net::p2p::node::dns_config::resolution_config>::define() {
   using config = forge::plugins::net::p2p::node::dns_config::resolution_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_dns_lookups>("max-dns-lookups");
   schema.field<&config::max_txt_records>("max-txt-records");
   schema.field<&config::max_resolved_addresses>("max-resolved-addresses");
   schema.field<&config::max_recursion_depth>("max-recursion-depth");
   schema.field<&config::max_multiaddr_size>("max-multiaddr-size");
   schema.field<&config::max_roots>("max-roots");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config::nameserver_config>
forge::schema::rules<forge::plugins::net::p2p::node::dns_config::nameserver_config>::define() {
   using config = forge::plugins::net::p2p::node::dns_config::nameserver_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::address>("address").required().non_empty();
   schema.field<&config::port>("port").default_value(std::uint64_t{53});
   schema.field<&config::scope_id>("scope-id").default_value("");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config::resolver_config>
forge::schema::rules<forge::plugins::net::p2p::node::dns_config::resolver_config>::define() {
   using config = forge::plugins::net::p2p::node::dns_config::resolver_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::nameservers>("nameservers")
       .items<forge::plugins::net::p2p::node::dns_config::nameserver_config>();
   schema.field<&config::max_in_flight>("max-in-flight");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config>
forge::schema::rules<forge::plugins::net::p2p::node::dns_config>::define() {
   using config = forge::plugins::net::p2p::node::dns_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::resolution>("resolution").default_value(config::resolution_config{});
   schema.field<&config::resolver>("resolver").default_value(config::resolver_config{});
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dial_config::ranker_config>
forge::schema::rules<forge::plugins::net::p2p::node::dial_config::ranker_config>::define() {
   using config = forge::plugins::net::p2p::node::dial_config::ranker_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::public_delay_ms>("public-delay-ms");
   schema.field<&config::private_delay_ms>("private-delay-ms");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dial_config::black_holes_config>
forge::schema::rules<forge::plugins::net::p2p::node::dial_config::black_holes_config>::define() {
   using config = forge::plugins::net::p2p::node::dial_config::black_holes_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::window_size>("window-size");
   schema.field<&config::min_successes>("min-successes");
   schema.field<&config::udp_enabled>("udp-enabled");
   schema.field<&config::ipv6_enabled>("ipv6-enabled");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::dial_config>
forge::schema::rules<forge::plugins::net::p2p::node::dial_config>::define() {
   using config = forge::plugins::net::p2p::node::dial_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::ranker>("ranker").default_value(config::ranker_config{});
   schema.field<&config::black_holes>("black-holes").default_value(config::black_holes_config{});
   schema.field<&config::max_concurrent_attempts>("max-concurrent-attempts");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::private_network_config>
forge::schema::rules<forge::plugins::net::p2p::node::private_network_config>::define() {
   auto schema = forge::schema::object<forge::plugins::net::p2p::node::private_network_config>();
   schema.field<&forge::plugins::net::p2p::node::private_network_config::psk_secret>("psk-secret")
       .required().non_empty().description("Secrets reference for the decoded 32-byte private-network PSK");
   schema.field<&forge::plugins::net::p2p::node::private_network_config::internet_egress>("internet-egress")
       .default_value(forge::plugins::net::p2p::node::internet_egress_policy::deny_external);
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

forge::schema::object_schema<forge::plugins::net::p2p::node::tcp_config>
forge::schema::rules<forge::plugins::net::p2p::node::tcp_config>::define() {
   using config = forge::plugins::net::p2p::node::tcp_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::security>("security");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::quic_config>
forge::schema::rules<forge::plugins::net::p2p::node::quic_config>::define() {
   using config = forge::plugins::net::p2p::node::quic_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::max_connections>("max-connections");
   schema.field<&config::max_streams_per_connection>("max-streams-per-connection");
   schema.field<&config::max_queued_bytes>("max-queued-bytes");
   schema.field<&config::max_inbound_queued_bytes>("max-inbound-queued-bytes");
   schema.field<&config::max_inbound_queued_packets>("max-inbound-queued-packets");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::bootstrap_policy_config>
forge::schema::rules<forge::plugins::net::p2p::node::bootstrap_policy_config>::define() {
   using config = forge::plugins::net::p2p::node::bootstrap_policy_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::requirement>("requirement").default_value("allow-disconnected");
   schema.field<&config::startup_budget_ms>("startup-budget-ms")
       .default_value(std::uint64_t{10'000}).range(1, 600'000);
   schema.field<&config::connect_timeout_ms>("connect-timeout-ms")
       .default_value(std::uint64_t{2'000}).range(1, 300'000);
   schema.field<&config::max_parallel>("max-parallel").default_value(std::uint64_t{4}).range(1, 256);
   schema.field<&config::retry_initial_delay_ms>("retry-initial-delay-ms");
   schema.field<&config::retry_max_delay_ms>("retry-max-delay-ms");
   schema.field<&config::retry_jitter>("retry-jitter");
   schema.field<&config::maintenance_interval_ms>("maintenance-interval-ms");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::identity_config>
forge::schema::rules<forge::plugins::net::p2p::node::identity_config>::define() {
   using config = forge::plugins::net::p2p::node::identity_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::certificate_secret>("certificate-secret").default_value("");
   schema.field<&config::private_key_secret>("private-key-secret").default_value("");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::peer_store_config>
forge::schema::rules<forge::plugins::net::p2p::node::peer_store_config>::define() {
   using config = forge::plugins::net::p2p::node::peer_store_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::store>("store").default_value("");
   schema.field<&config::schema_policy>("schema-policy")
       .default_value(forge::plugins::net::p2p::node::cache_schema_policy::reset);
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::path_config>
forge::schema::rules<forge::plugins::net::p2p::node::path_config>::define() {
   using config = forge::plugins::net::p2p::node::path_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::policy>("policy").default_value("direct-preferred");
   schema.field<&config::max_direct_endpoints>("max-direct-endpoints");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::topology_config::watermarks>
forge::schema::rules<forge::plugins::net::p2p::node::topology_config::watermarks>::define() {
   using config = forge::plugins::net::p2p::node::topology_config::watermarks;
   auto schema = forge::schema::object<config>();
   schema.field<&config::low>("low").default_value(std::uint64_t{128}).range(1, 1'000'000);
   schema.field<&config::target>("target").default_value(std::uint64_t{160}).range(1, 1'000'000);
   schema.field<&config::high>("high").default_value(std::uint64_t{192}).range(1, 1'000'000);
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::topology_config>
forge::schema::rules<forge::plugins::net::p2p::node::topology_config>::define() {
   using config = forge::plugins::net::p2p::node::topology_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::mode>("mode").default_value(forge::plugins::net::p2p::node::topology_mode::managed);
   schema.field<&config::peers>("peers").default_value(config::watermarks{});
   schema.field<&config::refresh_interval_ms>("refresh-interval-ms")
       .default_value(std::uint64_t{600'000}).range(1, 86'400'000);
   schema.field<&config::query_timeout_ms>("query-timeout-ms")
       .default_value(std::uint64_t{10'000}).range(1, 600'000);
   schema.field<&config::max_candidates>("max-candidates").default_value(std::uint64_t{256}).range(1, 1'000'000);
   schema.field<&config::max_parallel_queries>("max-parallel-queries")
       .default_value(std::uint64_t{10}).range(1, 1'000'000);
   schema.field<&config::max_parallel_dials>("max-parallel-dials")
       .default_value(std::uint64_t{4}).range(1, 1'000'000);
   schema.field<&config::retry_jitter>("retry-jitter").default_value(0.20).range(0.0, 0.999999);
   schema.field<&config::max_tagged_peers>("max-tagged-peers");
   schema.field<&config::max_tags_per_peer>("max-tags-per-peer");
   schema.field<&config::max_tag_size>("max-tag-size");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::rendezvous_config>
forge::schema::rules<forge::plugins::net::p2p::node::rendezvous_config>::define() {
   using config = forge::plugins::net::p2p::node::rendezvous_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::role>("role").default_value(forge::plugins::net::p2p::node::rendezvous_role::disabled);
   schema.field<&config::points>("points").items<forge::plugins::net::p2p::node::rendezvous_point_config>()
       .default_value(std::vector<forge::plugins::net::p2p::node::rendezvous_point_config>{}).max_items(1'000);
   schema.field<&config::max_points>("max-points").default_value(std::uint64_t{4}).range(1, 1'000);
   schema.field<&config::max_namespaces>("max-namespaces");
   schema.field<&config::default_ttl_seconds>("default-ttl-seconds");
   schema.field<&config::min_ttl_seconds>("min-ttl-seconds");
   schema.field<&config::max_ttl_seconds>("max-ttl-seconds");
   schema.field<&config::max_namespace_size>("max-namespace-size");
   schema.field<&config::max_registrations_per_peer>("max-registrations-per-peer");
   schema.field<&config::max_discover_limit>("max-discover-limit");
   schema.field<&config::max_message_size>("max-message-size");
   return schema;
}

forge::schema::object_schema<forge::plugins::net::p2p::node::peer_exchange_config>
forge::schema::rules<forge::plugins::net::p2p::node::peer_exchange_config>::define() {
   using config = forge::plugins::net::p2p::node::peer_exchange_config;
   auto schema = forge::schema::object<config>();
   schema.field<&config::enabled>("enabled").default_value(true);
   schema.field<&config::max_peers>("max-peers").default_value(std::uint64_t{4}).range(1, 1'000);
   schema.field<&config::max_message_size>("max-message-size");
   schema.field<&config::max_records>("max-records");
   schema.field<&config::max_queue>("max-queue");
   return schema;
}
