module;

#include <boost/describe.hpp>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.node.config;

import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

export namespace forge::plugins::net::p2p::node {

enum class pubsub_version : std::uint8_t {
   v1_0,
   v1_1,
   v1_2,
   v1_3,
};

enum class pubsub_signature_policy : std::uint8_t {
   strict_sign,
   strict_no_sign,
   lax_sign,
   lax_no_sign,
};

struct pubsub_config {
   struct wire_config {
      std::optional<std::uint64_t> max_rpc_size;
      std::optional<std::uint64_t> max_outbound_queue_bytes;
      std::optional<std::uint64_t> max_remote_topics_per_peer;
      std::optional<std::uint64_t> max_remote_topic_entries;
      std::optional<std::uint64_t> max_iwant_promises;
      bool operator==(const wire_config&) const = default;
   };

   struct validation_config {
      std::optional<std::uint64_t> max_attempts;
      std::optional<std::uint64_t> max_redeliveries;
      std::optional<std::uint64_t> max_requests;
      std::optional<std::uint64_t> retry_initial_delay_ms;
      std::optional<std::uint64_t> retry_max_delay_ms;
      bool operator==(const validation_config&) const = default;
   };

   struct idontwant_config {
      std::optional<std::uint64_t> threshold;
      std::optional<std::uint64_t> ttl_heartbeats;
      std::optional<std::uint64_t> max_entries_per_peer;
      std::optional<std::uint64_t> max_bytes_per_peer;
      std::optional<std::uint64_t> max_entries;
      std::optional<std::uint64_t> max_bytes;
      bool operator==(const idontwant_config&) const = default;
   };

   struct partial_config {
      std::optional<std::uint64_t> max_groups;
      std::optional<std::uint64_t> max_groups_per_topic;
      std::optional<std::uint64_t> max_group_bytes;
      std::optional<std::uint64_t> max_metadata_size;
      std::optional<std::uint64_t> ttl_heartbeats;
      std::optional<std::uint64_t> max_callbacks;
      std::optional<std::uint64_t> max_callback_bytes;
      bool operator==(const partial_config&) const = default;
   };

   struct scoring_config {
      struct topic_config {
         std::string topic;
         std::optional<double> topic_weight;
         std::optional<double> time_in_mesh_weight;
         std::optional<std::uint64_t> time_in_mesh_quantum_ms;
         std::optional<double> time_in_mesh_cap;
         std::optional<double> first_message_deliveries_weight;
         std::optional<double> first_message_deliveries_decay;
         std::optional<double> first_message_deliveries_cap;
         std::optional<double> mesh_message_deliveries_weight;
         std::optional<double> mesh_message_deliveries_decay;
         std::optional<double> mesh_message_deliveries_cap;
         std::optional<double> mesh_message_deliveries_threshold;
         std::optional<std::uint64_t> mesh_message_deliveries_window_ms;
         std::optional<std::uint64_t> mesh_message_deliveries_activation_ms;
         std::optional<double> mesh_failure_penalty_weight;
         std::optional<double> mesh_failure_penalty_decay;
         std::optional<double> invalid_message_deliveries_weight;
         std::optional<double> invalid_message_deliveries_decay;
         bool operator==(const topic_config&) const = default;
      };

      struct thresholds_config {
         std::optional<double> gossip_threshold;
         std::optional<double> publish_threshold;
         std::optional<double> graylist_threshold;
         std::optional<double> accept_px_threshold;
         std::optional<double> opportunistic_graft_threshold;
         bool operator==(const thresholds_config&) const = default;
      };

      struct limits_config {
         std::optional<std::uint64_t> max_connected_peers;
         std::optional<std::uint64_t> max_retained_peers;
         std::optional<std::uint64_t> max_messages;
         std::optional<std::uint64_t> max_delivery_peers;
         bool operator==(const limits_config&) const = default;
      };

      bool enabled = false;
      std::vector<topic_config> topics;
      std::optional<double> topic_score_cap;
      std::optional<double> ip_colocation_factor_weight;
      std::optional<double> ip_colocation_factor_threshold;
      std::optional<std::vector<std::string>> ip_colocation_factor_allowlist;
      std::optional<double> behaviour_penalty_weight;
      std::optional<double> behaviour_penalty_threshold;
      std::optional<double> behaviour_penalty_decay;
      std::optional<std::uint64_t> decay_interval_ms;
      std::optional<double> decay_to_zero;
      std::optional<std::uint64_t> retain_score_ms;
      std::optional<std::uint64_t> seen_message_ttl_ms;
      thresholds_config thresholds;
      limits_config limits;
      bool operator==(const scoring_config&) const = default;
   };

   bool enabled = false;
   std::uint64_t max_topics = 1'024;
   std::uint64_t max_handlers_per_topic = 64;
   std::uint64_t max_active_handlers = 4'096;
   std::uint64_t max_message_size = 1024 * 1024;
   std::uint64_t handler_deadline_ms = 5'000;
   std::vector<std::string> allowed_topics;
   std::vector<std::string> denied_topics;
   bool sign_publishes = true;
   bool partial_messages = false;
   std::optional<pubsub_version> preferred_version;
   std::optional<bool> allow_v1_0_fallback;
   std::optional<pubsub_signature_policy> signature_policy;
   std::optional<bool> flood_publish;
   std::optional<bool> peer_exchange;
   wire_config wire;
   validation_config validation;
   idontwant_config idontwant;
   partial_config partial;
   std::optional<scoring_config> scoring;
   bool operator==(const pubsub_config&) const = default;
};

struct diagnostics_config {
   std::uint64_t max_peers = 1'024;
   std::uint64_t max_sessions = 1'024;
   std::uint64_t max_endpoints_per_peer = 64;
   std::uint64_t max_protocols_per_peer = 128;
   std::uint64_t max_relay_reservations_per_peer = 64;
   bool operator==(const diagnostics_config&) const = default;
};

struct connection_config {
   std::optional<std::uint64_t> session_low_watermark;
   std::optional<std::uint64_t> session_grace_period_ms;
   std::optional<std::uint64_t> session_prune_silence_ms;
   std::optional<std::uint64_t> dial_backoff_base_ms;
   std::optional<std::uint64_t> dial_backoff_step_ms;
   std::optional<std::uint64_t> dial_backoff_max_ms;
   bool operator==(const connection_config&) const = default;
};

struct identify_config {
   std::optional<std::uint64_t> timeout_ms;
   std::optional<std::uint64_t> max_message_size;
   std::optional<std::uint64_t> max_total_message_size;
   std::optional<std::uint64_t> max_own_message_size;
   std::optional<std::uint64_t> max_message_parts;
   std::optional<std::uint64_t> max_protocols;
   std::optional<std::uint64_t> max_listen_endpoints;
   std::optional<std::uint64_t> max_protocol_size;
   std::optional<std::uint64_t> max_version_size;
   std::optional<std::uint64_t> max_public_key_size;
   std::optional<std::uint64_t> max_signed_peer_record_size;
   std::optional<std::uint64_t> max_push_operations;
   std::optional<std::string> protocol_version;
   std::optional<std::string> agent_version;
   bool operator==(const identify_config&) const = default;
};

struct peer_state_config {
   std::optional<std::uint64_t> max_peers;
   std::optional<std::uint64_t> max_rendezvous;
   std::optional<std::uint64_t> max_pending;
   std::optional<std::uint64_t> max_endpoints_per_peer;
   std::optional<std::uint64_t> max_protocols_per_peer;
   std::optional<std::uint64_t> max_relay_reservations_per_peer;
   std::optional<std::uint64_t> max_relay_endpoints_per_reservation;
   std::optional<std::uint64_t> max_peer_record_bytes;
   std::optional<std::uint64_t> hydration_page_limit;
   std::optional<std::uint64_t> prune_page_limit;
   std::optional<std::uint64_t> max_persistence_waiters;
   bool operator==(const peer_state_config&) const = default;
};

enum class tcp_security : std::uint8_t {
   tls_and_noise = 1,
   tls = 2,
   noise = 3,
};

struct tcp_config {
   std::optional<tcp_security> security;
   bool operator==(const tcp_config&) const = default;
};

struct quic_config {
   std::optional<std::uint64_t> max_connections;
   std::optional<std::uint64_t> max_streams_per_connection;
   std::optional<std::uint64_t> max_queued_bytes;
   std::optional<std::uint64_t> max_inbound_queued_bytes;
   std::optional<std::uint64_t> max_inbound_queued_packets;
   bool operator==(const quic_config&) const = default;
};

enum class internet_egress_policy : std::uint8_t {
   deny_external = 1,
   allow_internet = 2,
};

struct private_network_config {
   std::string psk_secret;
   internet_egress_policy internet_egress = internet_egress_policy::deny_external;
   bool operator==(const private_network_config&) const = default;
};

struct dns_config {
   struct resolution_config {
      std::optional<std::uint64_t> max_dns_lookups;
      std::optional<std::uint64_t> max_txt_records;
      std::optional<std::uint64_t> max_resolved_addresses;
      std::optional<std::uint64_t> max_recursion_depth;
      std::optional<std::uint64_t> max_multiaddr_size;
      std::optional<std::uint64_t> max_roots;
      bool operator==(const resolution_config&) const = default;
   };

   struct nameserver_config {
      std::string address;
      std::uint64_t port = 53;
      std::string scope_id;
      bool operator==(const nameserver_config&) const = default;
   };

   struct resolver_config {
      std::optional<std::vector<nameserver_config>> nameservers;
      std::optional<std::uint64_t> max_in_flight;
      bool operator==(const resolver_config&) const = default;
   };

   resolution_config resolution;
   resolver_config resolver;
   bool operator==(const dns_config&) const = default;
};

struct dial_config {
   struct ranker_config {
      std::optional<std::uint64_t> public_delay_ms;
      std::optional<std::uint64_t> private_delay_ms;
      bool operator==(const ranker_config&) const = default;
   };

   struct black_holes_config {
      std::optional<std::uint64_t> window_size;
      std::optional<std::uint64_t> min_successes;
      std::optional<bool> udp_enabled;
      std::optional<bool> ipv6_enabled;
      bool operator==(const black_holes_config&) const = default;
   };

   ranker_config ranker;
   black_holes_config black_holes;
   std::optional<std::uint64_t> max_concurrent_attempts;
   bool operator==(const dial_config&) const = default;
};

struct reachability_config {
   std::optional<bool> client_v1_enabled;
   std::optional<bool> client_v2_enabled;
   std::optional<bool> service_v1_enabled;
   std::optional<bool> service_v2_enabled;
   std::optional<std::uint64_t> timeout_ms;
   std::optional<std::uint64_t> refresh_interval_ms;
   std::optional<std::uint64_t> observation_ttl_ms;
   std::optional<std::uint64_t> min_observers;
   std::optional<std::uint64_t> min_reachability_observers;
   std::optional<std::uint64_t> max_observers;
   std::optional<std::uint64_t> max_observations;
   std::optional<std::uint64_t> max_candidates;
   std::optional<std::uint64_t> max_confirmed_per_local;
   std::optional<std::uint64_t> max_pending_probes;
   std::optional<std::uint64_t> service_per_peer_limit;
   std::optional<std::uint64_t> service_global_limit;
   std::optional<std::uint64_t> service_rate_interval_ms;
   std::optional<bool> ping_enabled;
   std::optional<std::uint64_t> ping_interval_ms;
   std::optional<std::uint64_t> ping_timeout_ms;
   std::optional<std::uint64_t> max_parallel_pings;
   std::optional<std::uint64_t> max_event_subscribers;
   bool operator==(const reachability_config&) const = default;
};

struct mdns_config {
   std::optional<bool> enabled;
   std::optional<bool> ipv4_enabled;
   std::optional<bool> ipv6_enabled;
   std::optional<std::uint64_t> record_ttl_seconds;
   std::optional<std::uint64_t> query_interval_ms;
   std::optional<std::uint64_t> max_interfaces;
   std::optional<std::uint64_t> max_peers;
   std::optional<std::uint64_t> max_addresses_per_peer;
   std::optional<std::uint64_t> max_pending_packets;
   std::optional<std::uint64_t> max_packet_size;
   std::optional<std::uint64_t> max_records_per_packet;
   bool operator==(const mdns_config&) const = default;
};

struct resource_config {
   // Missing budgets preserve the owning native scope's defaults; zero denies.
   struct scope_patch {
      std::optional<std::uint64_t> max_memory;
      std::optional<std::uint64_t> max_file_descriptors;
      std::optional<std::uint64_t> max_inbound_connections;
      std::optional<std::uint64_t> max_outbound_connections;
      std::optional<std::uint64_t> max_connections;
      std::optional<std::uint64_t> max_inbound_streams;
      std::optional<std::uint64_t> max_outbound_streams;
      std::optional<std::uint64_t> max_streams;
      bool operator==(const scope_patch&) const = default;
   };

   scope_patch system;
   scope_patch transient;
   scope_patch peer;
   scope_patch protocol;
   scope_patch service;
   scope_patch protocol_peer;
   scope_patch service_peer;
   scope_patch connection;
   scope_patch stream;
   std::optional<std::uint64_t> max_dial_attempts;
   std::optional<std::uint64_t> max_dial_attempts_per_peer;
   std::optional<std::uint64_t> max_relay_reservations;
   std::optional<std::uint64_t> max_malformed_messages_per_peer;
   bool operator==(const resource_config&) const = default;
};

enum class path_policy : std::uint8_t {
   direct_only = 1,
   direct_preferred = 2,
   relay_only = 3,
};

enum class relay_trust_policy : std::uint8_t {
   known_only = 1,
   public_allowed = 2,
};

struct relay_config {
   relay_trust_policy trust = relay_trust_policy::known_only;
   bool client_enabled = true;
   bool server_enabled = false;
   bool auto_discovery_enabled = true;
   bool public_allowed = false;
   std::uint64_t reservation_ttl_ms = 60'000;
   std::uint64_t max_candidates = 4;
   std::optional<std::uint64_t> target_reservations;
   std::optional<std::uint64_t> refresh_margin_ms;
   std::optional<std::uint64_t> max_parallel_reservations;
   std::optional<std::uint64_t> candidate_backoff_ms;
   std::optional<std::uint64_t> max_active_relays;
   std::optional<std::uint64_t> max_reservations;
   std::optional<std::uint64_t> max_streams_per_reservation;
   std::optional<std::uint64_t> max_relay_bytes;
   std::optional<std::uint64_t> max_queued_bytes;
   std::optional<std::uint64_t> max_duration_ms;
   std::optional<std::uint64_t> max_reservations_per_ip;
   std::optional<std::uint64_t> max_circuits_per_peer;
   std::optional<std::uint64_t> max_service_requests;
   std::optional<std::uint64_t> max_service_requests_per_peer;
   std::optional<std::uint64_t> max_service_requests_per_ip;
   std::optional<std::uint64_t> service_request_window_ms;
   std::optional<std::uint64_t> handshake_timeout_ms;
   bool operator==(const relay_config&) const = default;
};

enum class bootstrap_requirement : std::uint8_t {
   allow_disconnected = 1,
   require_connection = 2,
};

enum class dht_profile_kind : std::uint8_t {
   amino_v1 = 1,
   custom = 2,
};

enum class dht_mode : std::uint8_t {
   client = 1,
   server = 2,
};

enum class cache_schema_policy : std::uint8_t {
   reject = 1,
   reset = 2,
};

enum class topology_mode : std::uint8_t {
   managed = 1,
   static_only = 2,
};

enum class rendezvous_role : std::uint8_t {
   disabled = 1,
   client = 2,
   server = 3,
   client_and_server = 4,
};

struct dht_profile_config {
   struct records_config {
      std::optional<std::uint64_t> max_values;
      std::optional<std::uint64_t> max_total_bytes;
      std::optional<std::uint64_t> hydration_page_limit;
      std::optional<std::uint64_t> prune_page_limit;
      std::optional<std::uint64_t> max_hydration_pages;
      std::optional<std::uint64_t> max_providers;
      std::optional<std::uint64_t> max_local_providers;
      bool operator==(const records_config&) const = default;
   };

   dht_profile_kind kind = dht_profile_kind::amino_v1;
   dht_mode mode = dht_mode::client;
   std::string protocol;
   bool peers = true;
   bool providers = true;
   bool values = true;
   records_config records;
   bool operator==(const dht_profile_config&) const = default;
};

struct rendezvous_point_config {
   std::string endpoint;
   std::vector<std::string> namespaces;
   bool operator==(const rendezvous_point_config&) const = default;
};

struct bootstrap_policy_config {
   bootstrap_requirement requirement = bootstrap_requirement::allow_disconnected;
   std::uint64_t startup_budget_ms = 10'000;
   std::uint64_t connect_timeout_ms = 2'000;
   std::uint64_t max_parallel = 4;
   std::optional<std::uint64_t> retry_initial_delay_ms;
   std::optional<std::uint64_t> retry_max_delay_ms;
   std::optional<double> retry_jitter;
   std::optional<std::uint64_t> maintenance_interval_ms;
   bool operator==(const bootstrap_policy_config&) const = default;
};

struct identity_config {
   std::string certificate_secret;
   std::string private_key_secret;
   bool operator==(const identity_config&) const = default;
};

struct peer_store_config {
   std::string store;
   cache_schema_policy schema_policy = cache_schema_policy::reset;
   bool operator==(const peer_store_config&) const = default;
};

struct path_config {
   forge::plugins::net::p2p::node::path_policy policy = path_policy::direct_preferred;
   std::optional<std::uint64_t> max_direct_endpoints;
   bool operator==(const path_config&) const = default;
};

struct topology_config {
   struct watermarks {
      std::uint64_t low = 128;
      std::uint64_t target = 160;
      std::uint64_t high = 192;
      bool operator==(const watermarks&) const = default;
   };

   topology_mode mode = topology_mode::managed;
   watermarks peers;
   std::uint64_t refresh_interval_ms = 600'000;
   std::uint64_t query_timeout_ms = 10'000;
   std::uint64_t max_candidates = 256;
   std::uint64_t max_parallel_queries = 10;
   std::uint64_t max_parallel_dials = 4;
   double retry_jitter = 0.20;
   std::optional<std::uint64_t> max_tagged_peers;
   std::optional<std::uint64_t> max_tags_per_peer;
   std::optional<std::uint64_t> max_tag_size;
   bool operator==(const topology_config&) const = default;
};

struct rendezvous_config {
   forge::plugins::net::p2p::node::rendezvous_role role = rendezvous_role::disabled;
   std::vector<rendezvous_point_config> points;
   std::uint64_t max_points = 4;
   std::optional<std::uint64_t> max_namespaces;
   std::optional<std::uint64_t> default_ttl_seconds;
   std::optional<std::uint64_t> min_ttl_seconds;
   std::optional<std::uint64_t> max_ttl_seconds;
   std::optional<std::uint64_t> max_namespace_size;
   std::optional<std::uint64_t> max_registrations_per_peer;
   std::optional<std::uint64_t> max_discover_limit;
   std::optional<std::uint64_t> max_message_size;
   bool operator==(const rendezvous_config&) const = default;
};

struct peer_exchange_config {
   bool enabled = true;
   std::uint64_t max_peers = 4;
   std::optional<std::uint64_t> max_message_size;
   std::optional<std::uint64_t> max_records;
   std::optional<std::uint64_t> max_queue;
   bool operator==(const peer_exchange_config&) const = default;
};

struct config {
   std::vector<std::string> listen;
   std::vector<std::string> bootstrap;
   std::vector<std::string> advertised_endpoints;
   std::string peer_id;
   peer_store_config peer_store;
   identity_config identity;
   std::string api_codec = "forge.raw";
   std::uint64_t api_deadline_ms = 0;
   std::uint64_t api_max_frame_size = 16 * 1024 * 1024;
   std::uint64_t max_inflight_per_peer = 64;
   std::uint64_t max_sessions = 1024;
   std::uint64_t max_protocol_handlers = 1024;
   bool allow_insecure_test_mode = false;
   path_config path;
   relay_config relay;
   bootstrap_policy_config bootstrap_policy;
   std::vector<dht_profile_config> dht_profiles;
   topology_config topology;
   rendezvous_config rendezvous;
   peer_exchange_config peer_exchange;
   pubsub_config pubsub;
   diagnostics_config diagnostics;
   resource_config resources;
   connection_config connections;
   identify_config identify;
   peer_state_config peer_state;
   tcp_config tcp;
   quic_config quic;
   std::optional<private_network_config> private_network;
   dns_config dns;
   dial_config dial;
   reachability_config reachability;
   mdns_config mdns;
   bool operator==(const config&) const = default;
};

BOOST_DESCRIBE_ENUM(path_policy, direct_only, direct_preferred, relay_only)
BOOST_DESCRIBE_ENUM(relay_trust_policy, known_only, public_allowed)
BOOST_DESCRIBE_ENUM(bootstrap_requirement, allow_disconnected, require_connection)
BOOST_DESCRIBE_ENUM(dht_profile_kind, amino_v1, custom)
BOOST_DESCRIBE_ENUM(dht_mode, client, server)
BOOST_DESCRIBE_ENUM(cache_schema_policy, reject, reset)
BOOST_DESCRIBE_ENUM(topology_mode, managed, static_only)
BOOST_DESCRIBE_ENUM(rendezvous_role, disabled, client, server, client_and_server)
BOOST_DESCRIBE_ENUM(tcp_security, tls_and_noise, tls, noise)
BOOST_DESCRIBE_ENUM(internet_egress_policy, deny_external, allow_internet)
BOOST_DESCRIBE_ENUM(pubsub_version, v1_0, v1_1, v1_2, v1_3)
BOOST_DESCRIBE_ENUM(pubsub_signature_policy, strict_sign, strict_no_sign, lax_sign, lax_no_sign)
BOOST_DESCRIBE_STRUCT(dht_profile_config::records_config, (),
                      (max_values, max_total_bytes, hydration_page_limit, prune_page_limit, max_hydration_pages,
                       max_providers, max_local_providers))
BOOST_DESCRIBE_STRUCT(dht_profile_config, (), (kind, mode, protocol, peers, providers, values, records))
BOOST_DESCRIBE_STRUCT(rendezvous_point_config, (), (endpoint, namespaces))
BOOST_DESCRIBE_STRUCT(pubsub_config::wire_config, (),
                      (max_rpc_size, max_outbound_queue_bytes, max_remote_topics_per_peer,
                       max_remote_topic_entries, max_iwant_promises))
BOOST_DESCRIBE_STRUCT(pubsub_config::validation_config, (),
                      (max_attempts, max_redeliveries, max_requests, retry_initial_delay_ms, retry_max_delay_ms))
BOOST_DESCRIBE_STRUCT(pubsub_config::idontwant_config, (),
                      (threshold, ttl_heartbeats, max_entries_per_peer, max_bytes_per_peer, max_entries, max_bytes))
BOOST_DESCRIBE_STRUCT(pubsub_config::partial_config, (),
                      (max_groups, max_groups_per_topic, max_group_bytes, max_metadata_size, ttl_heartbeats,
                       max_callbacks, max_callback_bytes))
BOOST_DESCRIBE_STRUCT(pubsub_config::scoring_config::topic_config, (),
                      (topic, topic_weight, time_in_mesh_weight, time_in_mesh_quantum_ms, time_in_mesh_cap,
                       first_message_deliveries_weight, first_message_deliveries_decay, first_message_deliveries_cap,
                       mesh_message_deliveries_weight, mesh_message_deliveries_decay, mesh_message_deliveries_cap,
                       mesh_message_deliveries_threshold, mesh_message_deliveries_window_ms,
                       mesh_message_deliveries_activation_ms, mesh_failure_penalty_weight, mesh_failure_penalty_decay,
                       invalid_message_deliveries_weight, invalid_message_deliveries_decay))
BOOST_DESCRIBE_STRUCT(pubsub_config::scoring_config::thresholds_config, (),
                      (gossip_threshold, publish_threshold, graylist_threshold, accept_px_threshold,
                       opportunistic_graft_threshold))
BOOST_DESCRIBE_STRUCT(pubsub_config::scoring_config::limits_config, (),
                      (max_connected_peers, max_retained_peers, max_messages, max_delivery_peers))
BOOST_DESCRIBE_STRUCT(pubsub_config::scoring_config, (),
                      (enabled, topics, topic_score_cap, ip_colocation_factor_weight, ip_colocation_factor_threshold,
                       ip_colocation_factor_allowlist, behaviour_penalty_weight, behaviour_penalty_threshold,
                       behaviour_penalty_decay, decay_interval_ms, decay_to_zero, retain_score_ms, seen_message_ttl_ms,
                       thresholds, limits))
BOOST_DESCRIBE_STRUCT(pubsub_config, (),
                      (enabled, max_topics, max_handlers_per_topic, max_active_handlers, max_message_size,
                       handler_deadline_ms, allowed_topics, denied_topics, sign_publishes, partial_messages,
                       preferred_version, allow_v1_0_fallback, signature_policy, flood_publish, peer_exchange, wire,
                       validation, idontwant, partial, scoring))
BOOST_DESCRIBE_STRUCT(diagnostics_config, (),
                      (max_peers, max_sessions, max_endpoints_per_peer, max_protocols_per_peer,
                       max_relay_reservations_per_peer))
BOOST_DESCRIBE_STRUCT(resource_config::scope_patch, (),
                      (max_memory, max_file_descriptors, max_inbound_connections, max_outbound_connections,
                       max_connections, max_inbound_streams, max_outbound_streams, max_streams))
BOOST_DESCRIBE_STRUCT(resource_config, (),
                      (system, transient, peer, protocol, service, protocol_peer, service_peer, connection, stream,
                       max_dial_attempts, max_dial_attempts_per_peer, max_relay_reservations,
                       max_malformed_messages_per_peer))
BOOST_DESCRIBE_STRUCT(connection_config, (),
                      (session_low_watermark, session_grace_period_ms, session_prune_silence_ms,
                       dial_backoff_base_ms, dial_backoff_step_ms, dial_backoff_max_ms))
BOOST_DESCRIBE_STRUCT(identify_config, (),
                      (timeout_ms, max_message_size, max_total_message_size, max_own_message_size,
                       max_message_parts, max_protocols, max_listen_endpoints, max_protocol_size, max_version_size,
                       max_public_key_size, max_signed_peer_record_size, max_push_operations,
                       protocol_version, agent_version))
BOOST_DESCRIBE_STRUCT(peer_state_config, (),
                      (max_peers, max_rendezvous, max_pending, max_endpoints_per_peer, max_protocols_per_peer,
                       max_relay_reservations_per_peer, max_relay_endpoints_per_reservation, max_peer_record_bytes,
                       hydration_page_limit, prune_page_limit, max_persistence_waiters))
BOOST_DESCRIBE_STRUCT(tcp_config, (), (security))
BOOST_DESCRIBE_STRUCT(quic_config, (),
                      (max_connections, max_streams_per_connection, max_queued_bytes,
                       max_inbound_queued_bytes, max_inbound_queued_packets))
BOOST_DESCRIBE_STRUCT(private_network_config, (), (psk_secret, internet_egress))
BOOST_DESCRIBE_STRUCT(dns_config::resolution_config, (),
                      (max_dns_lookups, max_txt_records, max_resolved_addresses, max_recursion_depth,
                       max_multiaddr_size, max_roots))
BOOST_DESCRIBE_STRUCT(dns_config::nameserver_config, (), (address, port, scope_id))
BOOST_DESCRIBE_STRUCT(dns_config::resolver_config, (), (nameservers, max_in_flight))
BOOST_DESCRIBE_STRUCT(dns_config, (), (resolution, resolver))
BOOST_DESCRIBE_STRUCT(dial_config::ranker_config, (), (public_delay_ms, private_delay_ms))
BOOST_DESCRIBE_STRUCT(dial_config::black_holes_config, (), (window_size, min_successes, udp_enabled, ipv6_enabled))
BOOST_DESCRIBE_STRUCT(dial_config, (), (ranker, black_holes, max_concurrent_attempts))
BOOST_DESCRIBE_STRUCT(reachability_config, (),
                      (client_v1_enabled, client_v2_enabled, service_v1_enabled, service_v2_enabled,
                       timeout_ms, refresh_interval_ms, observation_ttl_ms, min_observers,
                       min_reachability_observers, max_observers, max_observations, max_candidates,
                       max_confirmed_per_local, max_pending_probes, service_per_peer_limit, service_global_limit,
                       service_rate_interval_ms, ping_enabled, ping_interval_ms, ping_timeout_ms,
                       max_parallel_pings, max_event_subscribers))
BOOST_DESCRIBE_STRUCT(mdns_config, (),
                      (enabled, ipv4_enabled, ipv6_enabled, record_ttl_seconds, query_interval_ms, max_interfaces,
                       max_peers, max_addresses_per_peer, max_pending_packets, max_packet_size, max_records_per_packet))
BOOST_DESCRIBE_STRUCT(relay_config, (),
                      (trust, client_enabled, server_enabled, auto_discovery_enabled, public_allowed,
                       reservation_ttl_ms, max_candidates, target_reservations, refresh_margin_ms,
                       max_parallel_reservations, candidate_backoff_ms, max_active_relays, max_reservations,
                       max_streams_per_reservation, max_relay_bytes, max_queued_bytes, max_duration_ms,
                       max_reservations_per_ip, max_circuits_per_peer, max_service_requests,
                       max_service_requests_per_peer, max_service_requests_per_ip,
                       service_request_window_ms, handshake_timeout_ms))
BOOST_DESCRIBE_STRUCT(bootstrap_policy_config, (),
                      (requirement, startup_budget_ms, connect_timeout_ms, max_parallel, retry_initial_delay_ms,
                       retry_max_delay_ms, retry_jitter, maintenance_interval_ms))
BOOST_DESCRIBE_STRUCT(identity_config, (), (certificate_secret, private_key_secret))
BOOST_DESCRIBE_STRUCT(peer_store_config, (), (store, schema_policy))
BOOST_DESCRIBE_STRUCT(path_config, (), (policy, max_direct_endpoints))
BOOST_DESCRIBE_STRUCT(topology_config::watermarks, (), (low, target, high))
BOOST_DESCRIBE_STRUCT(topology_config, (),
                      (mode, peers, refresh_interval_ms, query_timeout_ms, max_candidates, max_parallel_queries,
                       max_parallel_dials, retry_jitter, max_tagged_peers, max_tags_per_peer, max_tag_size))
BOOST_DESCRIBE_STRUCT(rendezvous_config, (),
                      (role, points, max_points, max_namespaces, default_ttl_seconds, min_ttl_seconds, max_ttl_seconds,
                       max_namespace_size, max_registrations_per_peer, max_discover_limit, max_message_size))
BOOST_DESCRIBE_STRUCT(peer_exchange_config, (), (enabled, max_peers, max_message_size, max_records, max_queue))
BOOST_DESCRIBE_STRUCT(config, (),
                      (listen, bootstrap, advertised_endpoints, peer_id, peer_store, identity, api_codec,
                       api_deadline_ms, api_max_frame_size, max_inflight_per_peer, max_sessions, max_protocol_handlers,
                       allow_insecure_test_mode, path, relay, bootstrap_policy, dht_profiles, topology, rendezvous,
                       peer_exchange, pubsub, diagnostics, resources, connections, identify, peer_state, tcp, quic,
                       private_network, dns, dial, reachability, mdns))

} // namespace forge::plugins::net::p2p::node

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dht_profile_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dht_profile_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dht_profile_config::records_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::dht_profile_config::records_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::rendezvous_point_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::rendezvous_point_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::wire_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::pubsub_config::wire_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::validation_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::pubsub_config::validation_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::idontwant_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::pubsub_config::idontwant_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::partial_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::pubsub_config::partial_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::scoring_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::pubsub_config::scoring_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::scoring_config::topic_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::pubsub_config::scoring_config::topic_config> define();
};

export template <> struct forge::schema::rules<
    forge::plugins::net::p2p::node::pubsub_config::scoring_config::thresholds_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::pubsub_config::scoring_config::thresholds_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::pubsub_config::scoring_config::limits_config> {
   [[nodiscard]] static forge::schema::object_schema<
       forge::plugins::net::p2p::node::pubsub_config::scoring_config::limits_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::diagnostics_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::diagnostics_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::resource_config::scope_patch> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::resource_config::scope_patch> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::resource_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::resource_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::connection_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::connection_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::identify_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::identify_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::peer_state_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::peer_state_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::tcp_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::tcp_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::quic_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::quic_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::private_network_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::private_network_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dns_config::resolution_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config::resolution_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dns_config::nameserver_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config::nameserver_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dns_config::resolver_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config::resolver_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dns_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dns_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dial_config::ranker_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dial_config::ranker_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dial_config::black_holes_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dial_config::black_holes_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dial_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dial_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::reachability_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::reachability_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::mdns_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::mdns_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::relay_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::relay_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::bootstrap_policy_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::bootstrap_policy_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::identity_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::identity_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::peer_store_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::peer_store_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::path_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::path_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::topology_config::watermarks> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::topology_config::watermarks> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::topology_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::topology_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::rendezvous_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::rendezvous_config> define();
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::peer_exchange_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::peer_exchange_config> define();
};
