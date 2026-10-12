#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "libp2p_identity_fixture.hxx"

import forge.asio.runtime;
import forge.net.dns.types;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.dialing;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.mdns_policy;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.topology;
import forge.net.pnet.protector;
import forge.net.quic.exceptions;
import forge.net.quic.options;
import forge.net.transport.limits;

namespace {
namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

struct callback_counts {
   std::size_t copies = 0;
   std::size_t calls = 0;
};

template<class Result>
struct counted_callback {
   callback_counts* counts;
   explicit counted_callback(callback_counts& value) : counts(&value) {}
   counted_callback(const counted_callback& other) : counts(other.counts) { ++counts->copies; }
   counted_callback(counted_callback&&) noexcept = default;
   template<class... Args> Result operator()(Args&&...) const {
      ++counts->calls;
      if constexpr (!std::is_void_v<Result>) { return Result{}; }
   }
};

p2p::node::options private_policy() {
   auto options = p2p::node::options{};
   options.capabilities.bits = p2p::capabilities::peer_exchange;
   options.relay_policy.client_enabled = false;
   options.relay_policy.auto_discovery_enabled = false;
   options.path_policy.allow_relay = false;
   options.path_policy.allow_hole_punch = false;
   options.private_network.emplace();
   return options;
}

void provision_identity(p2p::node::options& options) {
   const auto identity = forge::tests::p2p::make_identity_fixture("node-policy-validation");
   options.certificate_pem = identity.certificate_pem;
   options.private_key_pem = identity.private_key_pem;
}

void add_custom_profile(p2p::node::options& options, callback_counts& counts) {
   auto profile = p2p::dht::profile{};
   profile.protocol = {"/forge/test/policy/1.0.0"};
   profile.capabilities.values = true;
   profile.value_policies.push_back({
       .key_prefix = {'/', 't', '/'},
       .validate = counted_callback<void>{counts},
       .select = counted_callback<std::size_t>{counts},
       .expiry = counted_callback<std::chrono::system_clock::time_point>{counts},
   });
   options.dht_profiles.push_back(std::move(profile));
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_node_policy_test_suite)

BOOST_AUTO_TEST_CASE(rendezvous_default_ttl_must_fit_the_accepted_interval) {
   auto options = p2p::node::options{};
   options.limits.rendezvous.min_ttl = 1s;
   options.limits.rendezvous.max_ttl = 60s;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   for (const auto ttl : {1s, 30s, 60s}) {
      options.limits.rendezvous.default_ttl = ttl;
      BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   }
   for (const auto ttl : {0s, 61s}) {
      options.limits.rendezvous.default_ttl = ttl;
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   }
   options.limits.rendezvous.min_ttl = 10s;
   options.limits.rendezvous.default_ttl = 9s;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(default_policy_needs_no_resources_but_full_validation_does) {
   const auto options = p2p::node::options{};
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   BOOST_TEST(options.certificate_pem.empty());
   BOOST_TEST(options.private_key_pem.empty());
   BOOST_TEST(!options.allow_insecure_test_mode);
   BOOST_TEST(!options.peer_state.persistence);
   BOOST_TEST(!options.private_network);
   BOOST_TEST(options.dht_records.empty());
}

BOOST_AUTO_TEST_CASE(relay_reservations_are_required_even_when_service_is_disabled) {
   for (const auto service_enabled : {false, true}) {
      auto options = p2p::node::options{};
      options.relay_policy.service_enabled = service_enabled;
      options.limits.relay.require_reservation = false;
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      options.limits.relay.require_reservation = true;
      BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   }
}

BOOST_AUTO_TEST_CASE(relay_ttl_and_duration_are_bounded_by_native_clocks_and_wire_seconds) {
   auto options = p2p::node::options{};
   const auto maximum_clock_delay = std::min(
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::duration::max()),
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::duration::max()));
   options.limits.relay.reservation_ttl = maximum_clock_delay;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.limits.relay.reservation_ttl = maximum_clock_delay + 1ms;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.relay.reservation_ttl = std::chrono::milliseconds::max();
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.relay.reservation_ttl = 1ms;

   const auto maximum_wire_duration = std::chrono::seconds{(std::numeric_limits<std::uint32_t>::max)()};
   options.limits.relay.max_duration = std::chrono::duration_cast<std::chrono::milliseconds>(maximum_wire_duration);
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.limits.relay.max_duration = std::chrono::duration_cast<std::chrono::milliseconds>(maximum_wire_duration + 1s);
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.relay.max_duration = 1'500ms;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.relay.max_duration = std::chrono::milliseconds::max();
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(zero_resource_scope_budgets_remain_legal) {
   auto options = p2p::node::options{};
   const auto denied = p2p::resource_manager::scope_limits{
       .max_memory = 0, .max_file_descriptors = 0,
       .max_inbound_connections = 0, .max_outbound_connections = 0, .max_connections = 0,
       .max_inbound_streams = 0, .max_outbound_streams = 0, .max_streams = 0,
   };
   auto& limits = options.limits.resources;
   for (auto* scope : {&limits.system, &limits.transient, &limits.peer, &limits.protocol,
                      &limits.service, &limits.protocol_peer, &limits.service_peer,
                      &limits.connection, &limits.stream}) {
      *scope = denied;
   }
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   limits.max_dial_attempts = 0;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(private_policy_defers_only_the_actual_protector) {
   auto options = private_policy();
   BOOST_CHECK_NO_THROW(p2p::private_network::validate_policy(*options.private_network));
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK_THROW(p2p::private_network::validate(*options.private_network), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   BOOST_TEST(!options.private_network->protector);
   options.private_network->internet_egress = static_cast<p2p::private_network::internet_egress_policy>(99);
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.private_network->internet_egress = p2p::private_network::internet_egress_policy::allow_internet;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));

   provision_identity(options);
   // Explicit test resources, never supplied or substituted by policy validation.
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   const auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
   options.private_network->protector = std::make_shared<forge::net::pnet::protector>(
       forge::net::pnet::pre_shared_key{key});
   BOOST_CHECK_NO_THROW(p2p::validate(options));
}

BOOST_AUTO_TEST_CASE(private_policy_rendezvous_points_require_tcp_even_when_discovery_is_disabled) {
   const auto identity = forge::tests::p2p::make_identity_fixture("private-policy-rendezvous");
   const auto peer = p2p::make_peer_id_from_certificate_pem(identity.certificate_pem).to_string();
   const auto quic = p2p::parse_endpoint("/ip4/127.0.0.1/udp/4001/quic-v1/p2p/" + peer);
   const auto tcp = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4001/p2p/" + peer);
   for (const auto egress : {p2p::private_network::internet_egress_policy::deny_external,
                             p2p::private_network::internet_egress_policy::allow_internet}) {
      auto options = private_policy();
      options.private_network->internet_egress = egress;
      options.limits.topology.operating_mode = p2p::topology::mode::static_only;
      // Native role has no disabled enumerator; disabled discovery and no
      // rendezvous capability are the native equivalent of the plugin choice.
      options.limits.topology.rendezvous_enabled = false;
      options.limits.topology.rendezvous_points = {{.endpoint = quic, .namespaces = {"stage7"}}};
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      options.limits.topology.rendezvous_points.front().endpoint = tcp;
      BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
      BOOST_TEST(!options.private_network->protector);
      options.private_network.reset();
      options.limits.topology.rendezvous_points.front().endpoint = quic;
      BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   }
}

BOOST_AUTO_TEST_CASE(quic_limit_validation_is_shared_by_client_and_server_options) {
   namespace quic = forge::net::quic;
   auto limits = quic::transport_limits{};
   auto client = quic::client_options{};
   auto server = quic::server_options{};
   const auto identity = forge::tests::p2p::make_identity_fixture("quic-policy-validation");
   server.certificate_pem = identity.certificate_pem;
   server.private_key_pem = identity.private_key_pem;
   BOOST_CHECK_NO_THROW(quic::validate(limits));
   BOOST_CHECK_NO_THROW(quic::validate(client));
   BOOST_CHECK_NO_THROW(quic::validate(server));
   limits.max_frame_size = 0;
   client.limits = limits;
   server.limits = limits;
   BOOST_CHECK_THROW(quic::validate(limits), quic::exceptions::invalid_options);
   BOOST_CHECK_THROW(quic::validate(client), quic::exceptions::invalid_options);
   BOOST_CHECK_THROW(quic::validate(server), quic::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(quic_policy_rejects_each_zero_transport_limit_when_profile_available) {
   using limits = forge::net::transport::limits;
   const auto fields = std::array{
       &limits::max_connections, &limits::max_streams_per_connection, &limits::max_queued_bytes,
       &limits::max_inbound_queued_bytes, &limits::max_inbound_queued_packets,
   };
   for (const auto field : fields) {
      auto options = p2p::node::options{};
      BOOST_REQUIRE(options.capabilities.has(p2p::capabilities::direct_quic));
      options.transport_limits.*field = 0;
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      options.capabilities.bits = p2p::capabilities::peer_exchange;
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      auto private_options = private_policy();
      private_options.transport_limits.*field = 0;
      BOOST_CHECK_NO_THROW(p2p::validate_policy(private_options));
   }
   auto options = p2p::node::options{};
   options.transport_limits.max_frame_size = 0;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.capabilities.bits = p2p::capabilities::peer_exchange;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   auto private_options = private_policy();
   private_options.transport_limits.max_frame_size = 0;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(private_options));
}

BOOST_AUTO_TEST_CASE(quic_policy_checks_exact_uint32_frame_boundary_without_identity) {
   const auto maximum = std::uint64_t{std::numeric_limits<std::uint32_t>::max()};
   auto options = p2p::node::options{};
   auto limits = forge::net::quic::transport_limits{};
   options.transport_limits.max_frame_size = maximum;
   limits.max_frame_size = maximum;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK_NO_THROW(forge::net::quic::validate(limits));
   options.transport_limits.max_frame_size = maximum + 1;
   limits.max_frame_size = maximum + 1;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(forge::net::quic::validate(limits), forge::net::quic::exceptions::invalid_options);
   options.capabilities.bits = p2p::capabilities::peer_exchange;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   auto private_options = private_policy();
   private_options.transport_limits.max_frame_size = maximum + 1;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(private_options));
}

BOOST_AUTO_TEST_CASE(private_and_stream_transport_incompatibilities_fail_without_resources) {
   auto options = private_policy();
   options.capabilities.add(p2p::capabilities::direct_quic);
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.capabilities.bits = p2p::capabilities::peer_exchange;
   options.relay_policy.client_enabled = true;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.relay_policy.client_enabled = false;
   options.path_policy.allow_hole_punch = true;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.path_policy.allow_hole_punch = false;
   options.lifecycle.listen = {p2p::parse_endpoint("/ip4/127.0.0.1/udp/0/quic-v1")};
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.lifecycle.listen = {p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.private_network.reset();
   options.stream_security = p2p::node::stream_security::tls;
   options.relay_policy.client_enabled = true;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.relay_policy.client_enabled = false;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.stream_security = static_cast<p2p::node::stream_security>(99);
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(legacy_discovery_is_canonicalized_without_mutation_or_capacity_clamp) {
   auto options = p2p::node::options{};
   const auto original = options.limits.topology;
   options.limits.discovery.query_timeout = 13s;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK(options.limits.topology.query_timeout == original.query_timeout);
   options.limits.topology.query_timeout = 14s;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.topology.query_timeout = 13s;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.limits.discovery.enabled = false;
   options.mdns.enabled = true;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   BOOST_CHECK(options.limits.topology.operating_mode == original.operating_mode);
   options.mdns.enabled = false;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.limits.discovery.enabled = true;
   options.limits.max_sessions = 1;
   options.limits.session_low_watermark = 1;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_TEST(options.limits.topology.peers.target == original.peers.target);
   options.limits.discovery.max_parallel_queries = 0;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(path_direct_endpoint_budget_matches_scheduler_attempt_bound) {
   for (auto options : {p2p::node::options{}, private_policy()}) {
      for (const auto limit : {std::size_t{0}, std::size_t{101}}) {
         options.path_policy.max_direct_endpoints = limit;
         BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      }
      for (const auto limit : {std::size_t{1}, std::size_t{100}}) {
         options.path_policy.max_direct_endpoints = limit;
         BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
      }
   }
}

BOOST_AUTO_TEST_CASE(full_validation_requires_real_identity_and_explicit_backends) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto options = p2p::node::options{};
   options.certificate_pem = "not a certificate";
   options.private_key_pem = "not a private key";
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   // Full validation preserves its existing presence checks; parsing is constructor-owned.
   BOOST_CHECK_NO_THROW(p2p::validate(options));
   BOOST_CHECK_THROW(static_cast<void>(p2p::node{runtime, options}), p2p::exceptions::invalid_identity);
   provision_identity(options);
   options.peer_state.persistence.reset();
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   BOOST_CHECK_NO_THROW(p2p::validate(options));
   const auto foreign = forge::tests::p2p::make_identity_fixture("node-policy-foreign");
   auto private_key = std::exchange(options.private_key_pem, foreign.private_key_pem);
   BOOST_CHECK_NO_THROW(p2p::validate(options));
   BOOST_CHECK_THROW(static_cast<void>(p2p::node{runtime, options}), p2p::exceptions::invalid_identity);
   options.private_key_pem = std::move(private_key);
   options.explicit_peer_id = p2p::make_peer_id_from_certificate_pem(foreign.certificate_pem);
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_identity);
   BOOST_CHECK_THROW(static_cast<void>(p2p::node{runtime, options}), p2p::exceptions::invalid_identity);
   options.explicit_peer_id.reset();
   options.dht_profiles.push_back(p2p::amino_v1(p2p::dht::mode::client));
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   options.dht_records.emplace(options.dht_profiles.front().protocol,
                               p2p::dht::record_store::options_for(options.dht_profiles.front()));
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   options.dht_records.begin()->second.persistence = p2p::dht::record_store::make_memory_persistence();
   BOOST_CHECK_NO_THROW(p2p::validate(options));
}

BOOST_AUTO_TEST_CASE(peer_store_pure_bounds_are_shared_without_removing_standalone_fallback) {
   const auto fields = std::array{
       &p2p::peer_store::options::max_peers, &p2p::peer_store::options::max_rendezvous,
       &p2p::peer_store::options::max_pending, &p2p::peer_store::options::max_endpoints_per_peer,
       &p2p::peer_store::options::max_protocols_per_peer, &p2p::peer_store::options::max_relay_reservations_per_peer,
       &p2p::peer_store::options::max_relay_endpoints_per_reservation, &p2p::peer_store::options::max_peer_record_bytes,
       &p2p::peer_store::options::hydration_page_limit, &p2p::peer_store::options::prune_page_limit,
       &p2p::peer_store::options::max_persistence_waiters,
   };
   for (const auto field : fields) {
      auto options = p2p::node::options{};
      options.peer_state.*field = 0;
      BOOST_CHECK_THROW(p2p::validate(options.peer_state), p2p::exceptions::invalid_options);
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      BOOST_CHECK_THROW(static_cast<void>(p2p::peer_store{options.peer_state}), p2p::exceptions::invalid_options);
   }
   const auto defaults = p2p::peer_store::options{};
   BOOST_CHECK_NO_THROW(p2p::validate(defaults));
   BOOST_CHECK_NO_THROW(static_cast<void>(p2p::peer_store{defaults}));
   BOOST_TEST(!defaults.persistence);
}

BOOST_AUTO_TEST_CASE(dht_record_limits_are_checked_without_requiring_or_calling_a_backend) {
   const auto fields = std::array{
       &p2p::dht::record_store::options::max_values,
       &p2p::dht::record_store::options::max_providers,
       &p2p::dht::record_store::options::max_local_providers,
       &p2p::dht::record_store::options::max_providers_per_key,
       &p2p::dht::record_store::options::max_total_bytes,
       &p2p::dht::record_store::options::max_record_bytes,
       &p2p::dht::record_store::options::hydration_page_limit,
       &p2p::dht::record_store::options::prune_page_limit,
       &p2p::dht::record_store::options::max_persistence_waiters,
       &p2p::dht::record_store::options::max_hydration_pages,
   };
   callback_counts counts;
   auto options = p2p::node::options{};
   add_custom_profile(options, counts);
   auto records = p2p::dht::record_store::options_for(options.dht_profiles.front());
   records.public_keys = counted_callback<std::optional<p2p::public_key>>{counts};
   options.dht_records.emplace(options.dht_profiles.front().protocol, std::move(records));
   counts = {};
   auto& configured = options.dht_records.begin()->second;
   for (const auto field : fields) {
      const auto original = std::exchange(configured.*field, 0);
      BOOST_CHECK_THROW(p2p::validate(configured), p2p::exceptions::invalid_options);
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      configured.*field = original;
   }
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_TEST(!configured.persistence);
   options.allow_insecure_test_mode = true;
   options.explicit_peer_id = p2p::make_peer_id(p2p::public_key{
       .type = p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 41)});
   BOOST_CHECK_NO_THROW(p2p::validate(options));
   BOOST_TEST(counts.copies == 0U);
   BOOST_TEST(counts.calls == 0U);
}

BOOST_AUTO_TEST_CASE(dht_record_backend_identity_is_checked_during_pure_preflight) {
   auto options = p2p::node::options{};
   auto first = p2p::custom_dht_profile({"/forge/test/records/a/1"}, p2p::dht::mode::client, {.peers = true});
   auto second = p2p::custom_dht_profile({"/forge/test/records/b/1"}, p2p::dht::mode::client, {.peers = true});
   auto records = p2p::dht::record_store::options_for(first);
   records.persistence = p2p::dht::record_store::make_memory_persistence();
   options.dht_records.emplace(first.protocol, records);
   options.dht_records.emplace(second.protocol, records);
   options.dht_profiles = {std::move(first), std::move(second)};
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.dht_records.rbegin()->second.persistence = p2p::dht::record_store::make_memory_persistence();
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
}

BOOST_AUTO_TEST_CASE(dht_record_defaults_follow_profile_without_relaxing_amino_invariants) {
   auto profile = p2p::custom_dht_profile({"/forge/test/records/defaults/1"}, p2p::dht::mode::client,
                                          {.peers = true});
   profile.limits.replication = 2;
   profile.limits.alpha = 1;
   profile.limits.max_record_size = 128;
   const auto custom = p2p::dht::record_store::options_for(profile);
   BOOST_TEST(custom.max_providers_per_key == 2U);
   BOOST_TEST(custom.max_record_bytes == 128U);
   BOOST_TEST(custom.max_values == p2p::dht::record_store::options{}.max_values);
   BOOST_TEST(!custom.persistence);
   auto amino = p2p::amino_v1();
   const auto defaults = p2p::dht::record_store::options_for(amino);
   BOOST_TEST(defaults.max_providers_per_key == amino.limits.replication);
   BOOST_TEST(defaults.max_record_bytes == amino.limits.max_record_size);
   --amino.limits.replication;
   BOOST_CHECK_THROW(static_cast<void>(p2p::dht::record_store::options_for(amino)), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(dht_record_preflight_accepts_declared_custom_values_before_policy_binding) {
   auto profile = p2p::dht::profile{};
   profile.protocol = {"/forge/test/records/unbound/1"};
   profile.capabilities.values = true;
   const auto records = p2p::dht::record_store::options_for(profile);
   BOOST_CHECK_NO_THROW(p2p::validate(profile, records));
   BOOST_CHECK_THROW(p2p::validate(profile), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(static_cast<void>(p2p::dht::record_store{profile, records}), p2p::exceptions::invalid_options);
   auto options = p2p::node::options{};
   options.dht_profiles.push_back(profile);
   options.dht_records.emplace(profile.protocol, records);
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(dht_record_per_key_capacity_cannot_exceed_amino_or_custom_replication) {
   auto custom = p2p::custom_dht_profile({"/forge/test/records/per-key/1"}, p2p::dht::mode::client,
                                        {.peers = true, .providers = true});
   custom.limits.replication = 2;
   custom.limits.alpha = 1;
   for (const auto& profile : std::array{p2p::amino_v1(), std::move(custom)}) {
      auto records = p2p::dht::record_store::options_for(profile);
      BOOST_CHECK_NO_THROW(p2p::validate(profile, records));
      BOOST_CHECK_NO_THROW(static_cast<void>(p2p::dht::record_store{profile, records}));
      ++records.max_providers_per_key;
      BOOST_CHECK_THROW(p2p::validate(profile, records), p2p::exceptions::invalid_options);
      BOOST_CHECK_THROW(static_cast<void>(p2p::dht::record_store{profile, records}), p2p::exceptions::invalid_options);
      auto options = p2p::node::options{};
      options.dht_profiles.push_back(profile);
      options.dht_records.emplace(profile.protocol, std::move(records));
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(pubsub_validation_always_checks_router_scoring_and_node_retry_limits) {
   auto options = p2p::node::options{};
   options.limits.pubsub.scoring.emplace();
   options.limits.pubsub.scoring->app_specific_weight = 1;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.pubsub.scoring->app_specific_weight = 0;
   options.limits.pubsub.scoring->thresholds.gossip_threshold = std::numeric_limits<double>::quiet_NaN();
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.pubsub.scoring.reset();
   options.limits.pubsub.limits.history_gossip = options.limits.pubsub.limits.history_length + 1;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.pubsub.limits.history_gossip = 1;
   options.limits.pubsub.partial_messages = true;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.pubsub.preferred = p2p::pubsub::version::v1_3;
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   options.limits.pubsub.limits.validation_retry_initial_delay = 0ms;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   options.limits.pubsub.limits.validation_retry_initial_delay = 1ms;
   options.limits.pubsub.limits.max_validation_attempts = 0;
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_CASE(pubsub_retry_native_authority_checks_clock_range_order_and_counts) {
   const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::duration::max());
   for (const auto field : {&p2p::pubsub::limits::validation_retry_initial_delay,
                            &p2p::pubsub::limits::validation_retry_max_delay}) {
      auto options = p2p::node::options{};
      auto& limits = options.limits.pubsub.limits;
      limits.validation_retry_initial_delay = 1ms;
      limits.validation_retry_max_delay = maximum;
      limits.*field = maximum;
      BOOST_CHECK_NO_THROW(p2p::pubsub::validate(options.limits.pubsub));
      BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
      for (const auto invalid : {0ms, -1ms}) {
         limits.*field = invalid;
         BOOST_CHECK_THROW(p2p::pubsub::validate(options.limits.pubsub), p2p::exceptions::invalid_options);
         BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      }
      if (maximum < std::chrono::milliseconds::max()) {
         for (const auto invalid : {maximum + 1ms, std::chrono::milliseconds::max()}) {
            limits.*field = invalid;
            BOOST_CHECK_THROW(p2p::pubsub::validate(options.limits.pubsub), p2p::exceptions::invalid_options);
            BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
         }
      }
   }
   auto options = p2p::node::options{};
   options.limits.pubsub.limits.validation_retry_initial_delay = 2ms;
   options.limits.pubsub.limits.validation_retry_max_delay = 1ms;
   BOOST_CHECK_THROW(p2p::pubsub::validate(options.limits.pubsub), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   for (const auto field : {&p2p::pubsub::limits::max_validation_attempts,
                            &p2p::pubsub::limits::max_validation_redeliveries,
                            &p2p::pubsub::limits::max_validation_requests}) {
      options = {};
      options.limits.pubsub.limits.*field = 0;
      BOOST_CHECK_THROW(p2p::pubsub::validate(options.limits.pubsub), p2p::exceptions::invalid_options);
      BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   }
}

BOOST_AUTO_TEST_CASE(pubsub_heartbeat_native_authority_checks_clock_range) {
   const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::duration::max());
   for (const auto field : {&p2p::pubsub::limits::heartbeat_initial_delay,
                            &p2p::pubsub::limits::heartbeat_interval}) {
      auto options = p2p::node::options{};
      auto& limits = options.limits.pubsub.limits;
      limits.*field = maximum;
      BOOST_CHECK_NO_THROW(p2p::pubsub::validate(options.limits.pubsub));
      BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
      for (const auto invalid : {0ms, -1ms}) {
         limits.*field = invalid;
         BOOST_CHECK_THROW(p2p::pubsub::validate(options.limits.pubsub), p2p::exceptions::invalid_options);
         BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
      }
      if (maximum < std::chrono::milliseconds::max()) {
         for (const auto invalid : {maximum + 1ms, std::chrono::milliseconds::max()}) {
            limits.*field = invalid;
            BOOST_CHECK_THROW(p2p::pubsub::validate(options.limits.pubsub), p2p::exceptions::invalid_options);
            BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(validation_never_copies_or_invokes_complete_application_callbacks) {
   callback_counts counts;
   auto options = p2p::node::options{};
   add_custom_profile(options, counts);
   options.limits.pubsub.scoring.emplace();
   options.limits.pubsub.scoring->app_specific_weight = 1;
   options.limits.pubsub.scoring->app_specific_score = counted_callback<double>{counts};
   options.limits.pubsub.tracer = counted_callback<void>{counts};
   counts = {};
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_TEST(counts.copies == 0U);
   BOOST_TEST(counts.calls == 0U);
   provision_identity(options);
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   auto records = p2p::dht::record_store::options_for(options.dht_profiles.front());
   records.persistence = p2p::dht::record_store::make_memory_persistence();
   records.public_keys = counted_callback<std::optional<p2p::public_key>>{counts};
   options.dht_records.emplace(options.dht_profiles.front().protocol, std::move(records));
   counts = {};
   BOOST_CHECK_NO_THROW(p2p::validate(options));
   BOOST_TEST(counts.copies == 0U);
   BOOST_TEST(counts.calls == 0U);
}

BOOST_AUTO_TEST_CASE(custom_dht_requires_every_callback_without_invoking_any) {
   callback_counts counts;
   auto options = p2p::node::options{};
   add_custom_profile(options, counts);
   counts = {};
   auto& policy = options.dht_profiles.front().value_policies.front();
   auto validator = std::exchange(policy.validate, {});
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   policy.validate = std::move(validator);
   auto selector = std::exchange(policy.select, {});
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   policy.select = std::move(selector);
   auto expiry = std::exchange(policy.expiry, {});
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   policy.expiry = std::move(expiry);
   BOOST_CHECK_NO_THROW(p2p::validate_policy(options));
   BOOST_TEST(counts.copies == 0U);
   BOOST_TEST(counts.calls == 0U);
}

BOOST_AUTO_TEST_CASE(dht_protocol_and_persistence_names_are_checked_before_actual_identity) {
   auto options = p2p::node::options{};
   options.certificate_pem = "not a certificate";
   options.private_key_pem = "not a key";
   options.dht_profiles.push_back(p2p::amino_v1(p2p::dht::mode::client));
   options.dht_profiles.push_back(p2p::amino_v1(p2p::dht::mode::server));
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
   options.dht_profiles.pop_back();
   options.dht_records.emplace(p2p::protocol_id{"/foreign/kad/1.0.0"}, p2p::dht::record_store::options{});
   BOOST_CHECK_THROW(p2p::validate_policy(options), p2p::exceptions::invalid_options);
   BOOST_CHECK_THROW(p2p::validate(options), p2p::exceptions::invalid_options);
}

BOOST_AUTO_TEST_SUITE_END()
