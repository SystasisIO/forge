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

struct pubsub_config {
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
   dht_profile_kind kind = dht_profile_kind::amino_v1;
   dht_mode mode = dht_mode::client;
   std::string protocol;
   bool peers = true;
   bool providers = true;
   bool values = true;
   bool operator==(const dht_profile_config&) const = default;
};

struct rendezvous_point_config {
   std::string endpoint;
   std::vector<std::string> namespaces;
   bool operator==(const rendezvous_point_config&) const = default;
};

struct config {
   std::vector<std::string> listen;
   std::vector<std::string> bootstrap;
   std::vector<std::string> advertised_endpoints;
   std::string peer_id;
   std::string peer_store;
   std::string certificate_secret;
   std::string private_key_secret;
   std::string api_codec = "forge.raw";
   std::uint64_t api_deadline_ms = 0;
   std::uint64_t api_max_frame_size = 16 * 1024 * 1024;
   std::uint64_t max_inflight_per_peer = 64;
   std::uint64_t max_sessions = 1024;
   std::uint64_t max_protocol_handlers = 1024;
   bool allow_insecure_test_mode = false;
   forge::plugins::net::p2p::node::path_policy path_policy = forge::plugins::net::p2p::node::path_policy::direct_preferred;
   relay_trust_policy relay_trust = relay_trust_policy::known_only;
   bool relay_client_enabled = true;
   bool relay_server_enabled = false;
   bool relay_public_allowed = false;
   std::uint64_t relay_reservation_ttl_ms = 60'000;
   std::uint64_t relay_max_candidates = 4;
   forge::plugins::net::p2p::node::bootstrap_requirement bootstrap_requirement =
       forge::plugins::net::p2p::node::bootstrap_requirement::allow_disconnected;
   std::uint64_t bootstrap_startup_budget_ms = 10'000;
   std::uint64_t bootstrap_connect_timeout_ms = 2'000;
   std::uint64_t bootstrap_max_parallel = 4;
   cache_schema_policy peer_store_schema_policy = cache_schema_policy::reset;
   std::vector<dht_profile_config> dht_profiles;
   forge::plugins::net::p2p::node::topology_mode topology_mode = forge::plugins::net::p2p::node::topology_mode::managed;
   std::uint64_t topology_low = 128;
   std::uint64_t topology_target = 160;
   std::uint64_t topology_high = 192;
   std::uint64_t topology_refresh_interval_ms = 600'000;
   std::uint64_t topology_query_timeout_ms = 10'000;
   std::uint64_t topology_max_candidates = 256;
   std::uint64_t topology_max_parallel_queries = 10;
   std::uint64_t topology_max_parallel_dials = 4;
   double topology_retry_jitter = 0.20;
   forge::plugins::net::p2p::node::rendezvous_role rendezvous_role =
       forge::plugins::net::p2p::node::rendezvous_role::disabled;
   std::vector<rendezvous_point_config> rendezvous_points;
   std::uint64_t rendezvous_max_points = 4;
   bool peer_exchange_enabled = true;
   std::uint64_t peer_exchange_max_peers = 4;
   pubsub_config pubsub;
   diagnostics_config diagnostics;
   resource_config resources;
   connection_config connections;
   identify_config identify;
   peer_state_config peer_state;
   tcp_config tcp;
   quic_config quic;
   bool relay_auto_discovery_enabled = true;
   std::optional<private_network_config> private_network;
   dns_config dns;
   dial_config dial;
   std::optional<std::uint64_t> path_max_direct_endpoints;
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
BOOST_DESCRIBE_STRUCT(dht_profile_config, (), (kind, mode, protocol, peers, providers, values))
BOOST_DESCRIBE_STRUCT(rendezvous_point_config, (), (endpoint, namespaces))
BOOST_DESCRIBE_STRUCT(pubsub_config, (),
                      (enabled, max_topics, max_handlers_per_topic, max_active_handlers, max_message_size,
                       handler_deadline_ms, allowed_topics, denied_topics, sign_publishes, partial_messages))
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
BOOST_DESCRIBE_STRUCT(config, (),
                      (listen, bootstrap, advertised_endpoints, peer_id, peer_store, certificate_secret,
                       private_key_secret, api_codec, api_deadline_ms, api_max_frame_size, max_inflight_per_peer,
                       max_sessions, max_protocol_handlers, allow_insecure_test_mode, path_policy, relay_trust,
                       relay_client_enabled, relay_server_enabled, relay_public_allowed, relay_reservation_ttl_ms,
                       relay_max_candidates, bootstrap_requirement, bootstrap_startup_budget_ms,
                       bootstrap_connect_timeout_ms, bootstrap_max_parallel, peer_store_schema_policy, dht_profiles,
                       topology_mode, topology_low, topology_target, topology_high, topology_refresh_interval_ms,
                       topology_query_timeout_ms, topology_max_candidates, topology_max_parallel_queries,
                       topology_max_parallel_dials, topology_retry_jitter, rendezvous_role, rendezvous_points,
                       rendezvous_max_points, peer_exchange_enabled, peer_exchange_max_peers, pubsub, diagnostics,
                       resources, connections, identify, peer_state, tcp, quic,
                       relay_auto_discovery_enabled, private_network, dns, dial, path_max_direct_endpoints))

} // namespace forge::plugins::net::p2p::node

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dht_profile_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dht_profile_config> define();
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
