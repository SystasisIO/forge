module;

#include <boost/describe.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

export module forge.plugins.net.p2p.node.types;

import forge.api.transport.options;
import forge.net.p2p.identity;
import forge.net.p2p.endpoint;
import forge.schema.diagnostic;
import forge.schema.value_kind;
import forge.schema.object;
import forge.schema.enums;

export namespace forge::plugins::net::p2p::node {

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
};

struct rendezvous_point_config {
   std::string endpoint;
   std::vector<std::string> namespaces;
};

BOOST_DESCRIBE_ENUM(path_policy, direct_only, direct_preferred, relay_only)
BOOST_DESCRIBE_ENUM(relay_trust_policy, known_only, public_allowed)
BOOST_DESCRIBE_ENUM(bootstrap_requirement, allow_disconnected, require_connection)
BOOST_DESCRIBE_ENUM(dht_profile_kind, amino_v1, custom)
BOOST_DESCRIBE_ENUM(dht_mode, client, server)
BOOST_DESCRIBE_ENUM(cache_schema_policy, reject, reset)
BOOST_DESCRIBE_ENUM(topology_mode, managed, static_only)
BOOST_DESCRIBE_ENUM(rendezvous_role, disabled, client, server, client_and_server)
BOOST_DESCRIBE_STRUCT(dht_profile_config, (), (kind, mode, protocol, peers, providers, values))
BOOST_DESCRIBE_STRUCT(rendezvous_point_config, (), (endpoint, namespaces))

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
};

struct info {
   forge::net::p2p::peer_id local_peer;
   std::vector<forge::net::p2p::endpoint> local_endpoints;
   bool started = false;
};

struct remote_options {
   std::chrono::milliseconds open_deadline{10'000};
   std::optional<forge::api::core::codec_id> codec;
   std::optional<std::size_t> max_inflight;
   std::optional<std::chrono::milliseconds> deadline;
   std::optional<std::uint32_t> max_frame_size;
};

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
                       rendezvous_max_points, peer_exchange_enabled, peer_exchange_max_peers))

} // namespace forge::plugins::net::p2p::node

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::dht_profile_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::dht_profile_config> define() {
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
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::rendezvous_point_config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::rendezvous_point_config> define() {
      auto schema = forge::schema::object<forge::plugins::net::p2p::node::rendezvous_point_config>();
      schema.field<&forge::plugins::net::p2p::node::rendezvous_point_config::endpoint>("endpoint");
      schema.field<&forge::plugins::net::p2p::node::rendezvous_point_config::namespaces>("namespaces")
          .default_value(std::vector<std::string>{})
          .max_items(1'000);
      return schema;
   }
};

export template <> struct forge::schema::rules<forge::plugins::net::p2p::node::config> {
   [[nodiscard]] static forge::schema::object_schema<forge::plugins::net::p2p::node::config> define() {
      auto schema = forge::schema::object<forge::plugins::net::p2p::node::config>();
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
};
