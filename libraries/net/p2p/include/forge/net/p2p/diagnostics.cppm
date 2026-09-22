module;

#include <boost/describe.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module forge.net.p2p.diagnostics;

import forge.net.p2p.discovery;
import forge.net.p2p.dialing;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.host_event;
import forge.net.p2p.lifecycle;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.topology;
import forge.multiformats.multiaddr;

export namespace forge::net::p2p {

struct diagnostics {
   enum class session_direction : std::uint8_t {
      inbound = 1,
      outbound = 2,
   };

   struct options {
      std::size_t max_peers = 1'024;
      std::size_t max_sessions = 1'024;
      std::size_t max_endpoints_per_peer = 64;
      std::size_t max_protocols_per_peer = 128;
      std::size_t max_relay_reservations_per_peer = 64;
      std::size_t max_dht_profiles = 64;
   };

   struct metrics_snapshot {
      std::uint64_t sessions_opened = 0;
      std::uint64_t sessions_closed = 0;
      std::uint64_t sessions_pruned = 0;
      std::uint64_t connection_rejections = 0;
      std::uint64_t gater_peer_dial_rejections = 0;
      std::uint64_t gater_address_dial_rejections = 0;
      std::uint64_t gater_accept_rejections = 0;
      std::uint64_t gater_secured_rejections = 0;
      std::uint64_t gater_upgraded_rejections = 0;
      std::uint64_t handshakes_completed = 0;
      std::uint64_t handshakes_failed = 0;
      std::uint64_t protocol_streams_opened = 0;
      std::uint64_t protocol_streams_accepted = 0;
      std::uint64_t protocol_rejections = 0;
      std::uint64_t peer_exchange_messages = 0;
      std::uint64_t reachability_checks = 0;
      std::uint64_t reachability_public = 0;
      std::uint64_t reachability_private = 0;
      std::uint64_t relays_opened = 0;
      std::uint64_t relay_rejections = 0;
      std::uint64_t relay_reservations = 0;
      std::uint64_t relay_reservation_rejections = 0;
      std::uint64_t relay_reservation_expirations = 0;
      std::uint64_t relay_discovery_refreshes = 0;
      std::uint64_t relay_discovery_attempts = 0;
      std::uint64_t relay_discovery_successes = 0;
      std::uint64_t relay_discovery_failures = 0;
      std::uint64_t relay_bytes = 0;
      std::uint64_t hole_punch_attempts = 0;
      std::uint64_t hole_punch_successes = 0;
      std::uint64_t hole_punch_failures = 0;
      std::uint64_t path_direct_opens = 0;
      std::uint64_t path_relay_opens = 0;
      std::uint64_t path_direct_attempts = 0;
      std::uint64_t path_relay_attempts = 0;
      std::uint64_t direct_failures = 0;
      std::uint64_t relay_failures = 0;
      std::uint64_t dht_queries = 0;
      std::uint64_t dht_responses = 0;
      std::uint64_t rendezvous_registrations = 0;
      std::uint64_t rendezvous_discovers = 0;
      std::uint64_t pubsub_messages_published = 0;
      std::uint64_t pubsub_messages_received = 0;
      std::uint64_t pubsub_messages_delivered = 0;
      std::uint64_t pubsub_duplicates = 0;
      std::uint64_t pubsub_invalid_messages = 0;
      std::uint64_t pubsub_control_messages = 0;
      std::uint64_t backpressure_rejections = 0;
      std::size_t active_sessions = 0;
      std::size_t active_relays = 0;
      std::size_t active_relay_reservations = 0;
      bool stopped = false;
   };

   struct network_state {
      peer_id local_peer;
      std::vector<endpoint> local_endpoints;
      bool stopped = false;
   };

   struct endpoint_record {
      forge::multiformats::multiaddr address;
      path::kind kind = path::kind::direct;
      std::optional<peer_id> relay_peer;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::chrono::milliseconds last_latency{0};
      std::chrono::system_clock::time_point backoff_until{};
      double score = 0.0;
   };

   struct relay_reservation {
      peer_id relay;
      std::uint64_t reservation_id = 0;
      std::chrono::system_clock::time_point expires_at{};
      std::vector<forge::net::p2p::endpoint> endpoints;
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::chrono::milliseconds last_latency{0};
      double score = 0.0;
   };

   struct peer {
      peer_id peer;
      capability_set capabilities{};
      discovery::source discovered_by = discovery::source::explicit_config;
      std::string protocol_version;
      std::string agent_version;
      std::vector<protocol_id> protocols;
      std::vector<endpoint_record> endpoints;
      std::vector<relay_reservation> relay_reservations;
      reachability::state reachability = reachability::state::unknown;
      std::optional<forge::net::p2p::endpoint> observed_endpoint;
      std::chrono::system_clock::time_point reachability_expires_at{};
      std::chrono::system_clock::time_point discovered_at{};
      std::chrono::system_clock::time_point discovery_expires_at{};
      std::chrono::system_clock::time_point discovery_backoff_until{};
      std::uint64_t successes = 0;
      std::uint64_t failures = 0;
      std::chrono::milliseconds last_latency{0};
      double score = 0.0;
      bool protected_peer = false;
   };

   struct session {
      std::uint64_t id = 0;
      peer_id remote_peer;
      capability_set capabilities{};
      path::kind path = path::kind::direct;
      std::optional<peer_id> relay_peer;
      std::optional<forge::net::p2p::endpoint> direct_endpoint;
      std::optional<forge::net::p2p::endpoint> remote_endpoint;
      session_direction direction = session_direction::outbound;
      std::chrono::milliseconds age{0};
      std::chrono::milliseconds idle{0};
      bool closed = false;
      bool protected_peer = false;
      identify::state identify_state = identify::state::unknown;
      std::string identify_error;
   };

   struct connection_state {
      std::size_t active_sessions = 0;
      std::vector<peer_id> protected_peers;
      std::size_t retained_identify_attempts = 0;
   };

   struct persistence_state {
      std::size_t pending_peer_mutations = 0;
      std::uint64_t failure_count = 0;
      bool degraded = false;
      bool closing = false;
      bool closed = false;
      std::string last_failure;
   };

   struct dht_profile_state {
      protocol_id protocol;
      std::string kind;
      std::string mode;
      bool peers = false;
      bool providers = false;
      bool values = false;
      std::size_t routing_active = 0;
      std::size_t routing_replacements = 0;
      std::size_t routing_candidates = 0;
      std::size_t routing_nonempty_buckets = 0;
      bool maintenance_enabled = false;
      bool maintenance_startup_pending = false;
      bool maintenance_in_flight = false;
      std::uint32_t maintenance_failures = 0;
      std::chrono::milliseconds maintenance_next_attempt{0};
      std::uint64_t persistence_failures = 0;
      bool persistence_degraded = false;
      bool durability_uncertain = false;
      bool persistence_closing = false;
      bool persistence_closed = false;
      std::string last_persistence_failure;
   };

   struct topology_state {
      std::string mode;
      std::string phase;
      std::size_t low_watermark = 0;
      std::size_t target_watermark = 0;
      std::size_t high_watermark = 0;
      std::chrono::milliseconds refresh_interval{0};
      std::chrono::milliseconds query_timeout{0};
      std::size_t max_candidates = 0;
      std::size_t max_parallel_queries = 0;
      std::size_t max_parallel_dials = 0;
      std::size_t configured_rendezvous_points = 0;
      std::size_t max_peer_exchange_peers = 0;
      bool dht_enabled = false;
      bool peer_exchange_enabled = false;
      bool refresh_queued = false;
      bool refresh_in_flight = false;
      std::size_t observations = 0;
      // Distinct peers with currently valid transient mDNS evidence.
      std::size_t mdns_observations = 0;
      std::size_t active_operations = 0;
      std::size_t waiting_refreshes = 0;
      std::uint64_t completed_refreshes = 0;
      std::uint64_t failed_refreshes = 0;
   };

   struct reachability_state {
      host_event host;
      bool client_v1_enabled = false;
      bool client_v2_enabled = false;
      bool service_v1_enabled = false;
      bool service_v2_enabled = false;
      bool internet_egress_allowed = false;
      std::size_t pending_probes = 0;
      std::size_t pending_pings = 0;
      std::size_t waiting_probes = 0;
      std::size_t active_handlers = 0;
      std::size_t max_pending_probes = 0;
      std::size_t max_parallel_pings = 0;
      std::uint64_t ping_successes = 0;
      std::uint64_t ping_failures = 0;
      std::uint64_t probe_errors = 0;
   };

   struct snapshot {
      network_state network;
      metrics_snapshot metrics;
      resource_manager::snapshot resources;
      pubsub::snapshot pubsub;
      connection_state connections;
      std::vector<peer> peers;
      std::vector<session> sessions;
      persistence_state persistence;
      std::vector<dht_profile_state> dht_profiles;
      lifecycle_status lifecycle;
      resource_manager::limits effective_limits;
      topology_state topology;
      dialing::black_hole_status black_holes;
      reachability_state reachability;
   };
};

} // namespace forge::net::p2p

BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::options, (),
                      (max_peers, max_sessions, max_endpoints_per_peer, max_protocols_per_peer,
                       max_relay_reservations_per_peer, max_dht_profiles))
BOOST_DESCRIBE_STRUCT(
    forge::net::p2p::diagnostics::metrics_snapshot, (),
    (sessions_opened, sessions_closed, sessions_pruned, connection_rejections, gater_peer_dial_rejections,
     gater_address_dial_rejections, gater_accept_rejections, gater_secured_rejections, gater_upgraded_rejections,
     handshakes_completed, handshakes_failed, protocol_streams_opened, protocol_streams_accepted, protocol_rejections,
     peer_exchange_messages, reachability_checks, reachability_public, reachability_private, relays_opened,
     relay_rejections, relay_reservations, relay_reservation_rejections, relay_reservation_expirations,
     relay_discovery_refreshes, relay_discovery_attempts, relay_discovery_successes, relay_discovery_failures,
     relay_bytes, hole_punch_attempts, hole_punch_successes, hole_punch_failures, path_direct_opens, path_relay_opens,
     path_direct_attempts, path_relay_attempts, direct_failures, relay_failures, dht_queries, dht_responses,
     rendezvous_registrations, rendezvous_discovers, pubsub_messages_published, pubsub_messages_received,
     pubsub_messages_delivered, pubsub_duplicates, pubsub_invalid_messages, pubsub_control_messages,
     backpressure_rejections, active_sessions, active_relays, active_relay_reservations, stopped))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::network_state, (), (local_peer, local_endpoints, stopped))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::endpoint_record, (),
                      (address, kind, relay_peer, successes, failures, last_latency, backoff_until, score))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::relay_reservation, (),
                      (relay, reservation_id, expires_at, endpoints, successes, failures, last_latency, score))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::peer, (),
                      (peer, capabilities, discovered_by, protocol_version, agent_version, protocols, endpoints,
                       relay_reservations, reachability, observed_endpoint, reachability_expires_at, discovered_at,
                       discovery_expires_at, discovery_backoff_until, successes, failures, last_latency, score,
                       protected_peer))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::session, (),
                      (id, remote_peer, capabilities, path, relay_peer, direct_endpoint, remote_endpoint, direction,
                       age, idle, closed, protected_peer, identify_state, identify_error))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::connection_state, (),
                      (active_sessions, protected_peers, retained_identify_attempts))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::persistence_state, (),
                      (pending_peer_mutations, failure_count, degraded, closing, closed, last_failure))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::dht_profile_state, (),
                      (protocol, kind, mode, peers, providers, values, routing_active, routing_replacements,
                       routing_candidates, routing_nonempty_buckets, maintenance_enabled, maintenance_startup_pending,
                       maintenance_in_flight, maintenance_failures, maintenance_next_attempt, persistence_failures,
                       persistence_degraded, durability_uncertain, persistence_closing, persistence_closed,
                       last_persistence_failure))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::topology_state, (),
                      (mode, phase, low_watermark, target_watermark, high_watermark, refresh_interval, query_timeout,
                       max_candidates, max_parallel_queries, max_parallel_dials, configured_rendezvous_points,
                       max_peer_exchange_peers, dht_enabled, peer_exchange_enabled, refresh_queued, refresh_in_flight,
                       observations, mdns_observations, active_operations, waiting_refreshes, completed_refreshes, failed_refreshes))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::snapshot, (),
                      (network, metrics, resources, pubsub, connections, peers, sessions, persistence, dht_profiles,
                       lifecycle, effective_limits, topology, black_holes, reachability))
BOOST_DESCRIBE_STRUCT(forge::net::p2p::diagnostics::reachability_state, (),
                      (host, client_v1_enabled, client_v2_enabled, service_v1_enabled, service_v2_enabled,
                       internet_egress_allowed, pending_probes, pending_pings, waiting_probes, active_handlers,
                       max_pending_probes, max_parallel_pings, ping_successes, ping_failures, probe_errors))
