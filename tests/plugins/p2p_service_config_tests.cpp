module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.registry;
import forge.asio.runtime;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.endpoint;
import forge.net.p2p.mdns_policy;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.relay;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;

#include "../../plugins/net/p2p/node/details/configuration_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::string_view section = "plugins.net.p2p.node";

enum class unit { count, milliseconds, seconds, bytes64 };
struct numeric_field {
   const char* path;
   std::uint64_t (*read)(const p2p::node::options&);
   std::uint64_t sample;
   unit units = unit::count;
};
struct boolean_field {
   const char* path;
   bool (*read)(const p2p::node::options&);
};

constexpr auto numbers = std::array{
   numeric_field{"reachability.timeout-ms", [](const auto& v) -> std::uint64_t { return v.reachability_policy.timeout.count(); }, 1000, unit::milliseconds},
   numeric_field{"reachability.refresh-interval-ms", [](const auto& v) -> std::uint64_t { return v.reachability_policy.refresh_interval.count(); }, 1000, unit::milliseconds},
   numeric_field{"reachability.observation-ttl-ms", [](const auto& v) -> std::uint64_t { return v.reachability_policy.observation_ttl.count(); }, 20000, unit::milliseconds},
   numeric_field{"reachability.min-observers", [](const auto& v) -> std::uint64_t { return v.reachability_policy.min_observers; }, 1},
   numeric_field{"reachability.min-reachability-observers", [](const auto& v) -> std::uint64_t { return v.reachability_policy.min_reachability_observers; }, 1},
   numeric_field{"reachability.max-observers", [](const auto& v) -> std::uint64_t { return v.reachability_policy.max_observers; }, 128},
   numeric_field{"reachability.max-observations", [](const auto& v) -> std::uint64_t { return v.reachability_policy.max_observations; }, 2048},
   numeric_field{"reachability.max-candidates", [](const auto& v) -> std::uint64_t { return v.reachability_policy.max_candidates; }, 512},
   numeric_field{"reachability.max-confirmed-per-local", [](const auto& v) -> std::uint64_t { return v.reachability_policy.max_confirmed_per_local; }, 2},
   numeric_field{"reachability.max-pending-probes", [](const auto& v) -> std::uint64_t { return v.reachability_policy.max_pending_probes; }, 8},
   numeric_field{"reachability.service-per-peer-limit", [](const auto& v) -> std::uint64_t { return v.reachability_policy.service_per_peer_limit; }, 2},
   numeric_field{"reachability.service-global-limit", [](const auto& v) -> std::uint64_t { return v.reachability_policy.service_global_limit; }, 64},
   numeric_field{"reachability.service-rate-interval-ms", [](const auto& v) -> std::uint64_t { return v.reachability_policy.service_rate_interval.count(); }, 1000, unit::milliseconds},
   numeric_field{"reachability.ping-interval-ms", [](const auto& v) -> std::uint64_t { return v.reachability_policy.ping_interval.count(); }, 1000, unit::milliseconds},
   numeric_field{"reachability.ping-timeout-ms", [](const auto& v) -> std::uint64_t { return v.reachability_policy.ping_timeout.count(); }, 1000, unit::milliseconds},
   numeric_field{"reachability.max-parallel-pings", [](const auto& v) -> std::uint64_t { return v.reachability_policy.max_parallel_pings; }, 8},
   numeric_field{"reachability.max-event-subscribers", [](const auto& v) -> std::uint64_t { return v.reachability_policy.max_event_subscribers; }, 8},
   numeric_field{"mdns.record-ttl-seconds", [](const auto& v) -> std::uint64_t { return v.mdns.record_ttl.count(); }, 180, unit::seconds},
   numeric_field{"mdns.query-interval-ms", [](const auto& v) -> std::uint64_t { return v.mdns.query_interval.count(); }, 1000, unit::milliseconds},
   numeric_field{"mdns.max-interfaces", [](const auto& v) -> std::uint64_t { return v.mdns.max_interfaces; }, 8},
   numeric_field{"mdns.max-peers", [](const auto& v) -> std::uint64_t { return v.mdns.max_peers; }, 8},
   numeric_field{"mdns.max-addresses-per-peer", [](const auto& v) -> std::uint64_t { return v.mdns.max_addresses_per_peer; }, 8},
   numeric_field{"mdns.max-pending-packets", [](const auto& v) -> std::uint64_t { return v.mdns.max_pending_packets; }, 8},
   numeric_field{"mdns.max-packet-size", [](const auto& v) -> std::uint64_t { return v.mdns.max_packet_size; }, 512},
   numeric_field{"mdns.max-records-per-packet", [](const auto& v) -> std::uint64_t { return v.mdns.max_records_per_packet; }, 8},
   numeric_field{"relay.target-reservations", [](const auto& v) -> std::uint64_t { return v.relay_policy.target_reservations; }, 3},
   numeric_field{"relay.refresh-margin-ms", [](const auto& v) -> std::uint64_t { return v.relay_policy.refresh_margin.count(); }, 1000, unit::milliseconds},
   numeric_field{"relay.max-parallel-reservations", [](const auto& v) -> std::uint64_t { return v.relay_policy.max_parallel_reservations; }, 3},
   numeric_field{"relay.candidate-backoff-ms", [](const auto& v) -> std::uint64_t { return v.relay_policy.candidate_backoff.count(); }, 1000, unit::milliseconds},
   numeric_field{"relay.max-active-relays", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_active_relays; }, 8},
   numeric_field{"relay.max-reservations", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_reservations; }, 8},
   numeric_field{"relay.max-streams-per-reservation", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_streams_per_reservation; }, 8},
   numeric_field{"relay.max-relay-bytes", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_relay_bytes; }, 1024, unit::bytes64},
   numeric_field{"relay.max-queued-bytes", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_queued_bytes; }, 1024},
   numeric_field{"relay.max-duration-ms", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_duration.count(); }, 1000, unit::milliseconds},
   numeric_field{"relay.max-reservations-per-ip", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_reservations_per_ip; }, 2},
   numeric_field{"relay.max-circuits-per-peer", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_circuits_per_peer; }, 2},
   numeric_field{"relay.max-service-requests", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_service_requests; }, 64},
   numeric_field{"relay.max-service-requests-per-peer", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_service_requests_per_peer; }, 4},
   numeric_field{"relay.max-service-requests-per-ip", [](const auto& v) -> std::uint64_t { return v.limits.relay.max_service_requests_per_ip; }, 16},
   numeric_field{"relay.service-request-window-ms", [](const auto& v) -> std::uint64_t { return v.limits.relay.service_request_window.count(); }, 1000, unit::milliseconds},
   numeric_field{"relay.handshake-timeout-ms", [](const auto& v) -> std::uint64_t { return v.limits.relay.handshake_timeout.count(); }, 1000, unit::milliseconds},
};
constexpr auto flags = std::array{
   boolean_field{"reachability.client-v1-enabled", [](const auto& v) { return v.reachability_policy.client_v1_enabled; }},
   boolean_field{"reachability.client-v2-enabled", [](const auto& v) { return v.reachability_policy.client_v2_enabled; }},
   boolean_field{"reachability.service-v1-enabled", [](const auto& v) { return v.reachability_policy.service_v1_enabled; }},
   boolean_field{"reachability.service-v2-enabled", [](const auto& v) { return v.reachability_policy.service_v2_enabled; }},
   boolean_field{"reachability.ping-enabled", [](const auto& v) { return v.reachability_policy.ping_enabled; }},
   boolean_field{"mdns.enabled", [](const auto& v) { return v.mdns.enabled; }},
   boolean_field{"mdns.ipv4-enabled", [](const auto& v) { return v.mdns.ipv4_enabled; }},
   boolean_field{"mdns.ipv6-enabled", [](const auto& v) { return v.mdns.ipv6_enabled; }},
};

forge::config::core::document valid_document() {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".peer-store.store", "unbound-store");
   document.set(std::string{section} + ".identity.certificate-secret", "unbound/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "unbound/key");
   // Managed topology permits enabled mDNS; configuration never starts its service.
   document.set(std::string{section} + ".topology.mode", "managed");
   return document;
}

np::config decode(const forge::config::core::document& document) {
   return np::decode_config({document, std::string{section}});
}

p2p::node::options mapped(const np::config& config, p2p::node::options native = {}) {
   np::apply_reachability(native, config.reachability);
   np::apply_mdns(native, config.mdns);
   np::apply_relay(native, config.relay, np::parse_policy(config));
   return native;
}

void check_settings(const p2p::node::options& actual, const p2p::node::options& expected) {
   for (const auto& field : numbers) {
      BOOST_TEST_CONTEXT(field.path) { BOOST_TEST(field.read(actual) == field.read(expected)); }
   }
   for (const auto& field : flags) {
      BOOST_TEST_CONTEXT(field.path) { BOOST_TEST(field.read(actual) == field.read(expected)); }
   }
}

template<class T> T run(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (result.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   return result.get();
}

void configure(forge::asio::runtime& runtime, np::plugin& plugin, const forge::config::core::document& document) {
   run(runtime, plugin.configure({document, std::string{section}}));
}

forge::config::core::document yaml_candidate(std::string_view text) {
   const auto parsed = forge::codec::yaml::read_document(text);
   BOOST_REQUIRE(parsed.ok());
   const auto* changes = parsed.value.object_at("");
   BOOST_REQUIRE(changes);
   auto document = valid_document();
   for (const auto& [name, setting] : *changes) { document.set(std::string{section} + "." + name, setting); }
   return document;
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_service_config_defaults_and_every_numeric_patch_are_independent) {
   const auto defaults = mapped({});
   check_settings(defaults, p2p::node::options{});
   BOOST_TEST(defaults.relay_policy.max_candidates_per_refresh == 4U);
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto& field : numbers) {
      BOOST_TEST_CONTEXT(field.path) {
         auto document = valid_document();
         document.set(std::string{section} + "." + field.path, field.sample);
         const auto config = decode(document);
         BOOST_CHECK(config != np::config{});
         const auto actual = mapped(config);
         BOOST_TEST(field.read(actual) == field.sample);
         for (const auto& other : numbers) {
            if (&other != &field) { BOOST_TEST(other.read(actual) == other.read(defaults)); }
         }
         for (const auto& flag : flags) { BOOST_TEST(flag.read(actual) == flag.read(defaults)); }
         BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_service_config_boolean_sparse_false_and_removal_preserve_native_defaults) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto defaults = mapped({});
   for (const auto& field : flags) {
      for (const auto enabled : {false, true}) {
         BOOST_TEST_CONTEXT(field.path << "=" << enabled) {
            auto document = valid_document();
            document.set(std::string{section} + "." + field.path, enabled);
            const auto actual = mapped(decode(document));
            BOOST_TEST(field.read(actual) == enabled);
            for (const auto& other : flags) {
               if (&other != &field) { BOOST_TEST(other.read(actual) == other.read(defaults)); }
            }
            for (const auto& number : numbers) { BOOST_TEST(number.read(actual) == number.read(defaults)); }
            BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
         }
      }
   }
   auto native = defaults;
   native.reachability_policy.ping_enabled = false;
   native.reachability_policy.max_event_subscribers = 7;
   native.mdns.ipv4_enabled = false;
   native.mdns.max_peers = 19;
   native.limits.relay.max_service_requests = 23;
   check_settings(mapped({}, native), native);
   auto patch = np::config{};
   patch.reachability.max_event_subscribers = 3;
   patch.mdns.max_peers = 11;
   patch.relay.max_service_requests = 17;
   const auto changed = mapped(patch, native);
   BOOST_TEST(changed.reachability_policy.max_event_subscribers == 3U);
   BOOST_TEST(changed.mdns.max_peers == 11U);
   BOOST_TEST(changed.limits.relay.max_service_requests == 17U);
   check_settings(mapped(decode(valid_document())), defaults);
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, valid_document()));
}

BOOST_AUTO_TEST_CASE(p2p_service_config_checked_native_counts_durations_and_uint64_bytes) {
   for (const auto& field : numbers) {
      BOOST_TEST_CONTEXT(field.path) {
         const auto maximum = field.units == unit::milliseconds
             ? static_cast<std::uint64_t>((std::numeric_limits<std::chrono::milliseconds::rep>::max)())
             : field.units == unit::seconds
             ? static_cast<std::uint64_t>((std::numeric_limits<std::chrono::seconds::rep>::max)())
             : field.units == unit::bytes64 ? (std::numeric_limits<std::uint64_t>::max)()
             : static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
         auto document = valid_document();
         document.set(std::string{section} + "." + field.path, maximum);
         // This checks representation only, not acceptance by native policy validation.
         BOOST_TEST(field.read(mapped(decode(document))) == maximum);
         if (field.units == unit::milliseconds || field.units == unit::seconds ||
             (field.units == unit::count && sizeof(std::size_t) < sizeof(std::uint64_t))) {
            document.set(std::string{section} + "." + field.path, maximum + 1);
            BOOST_CHECK_THROW(mapped(decode(document)), np::exceptions::invalid_config);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_service_config_zero_bad_scalar_and_unknown_yaml_reject_before_native_execution) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto valid = valid_document();
   configure(runtime, plugin, valid);
   for (const auto& field : numbers) {
      for (const auto& invalid : {value{std::uint64_t{0}}, value{std::int64_t{-1}}, value{1.5},
                                  value{"18446744073709551616"}, value{true}}) {
         BOOST_TEST_CONTEXT(field.path) {
            auto document = valid;
            document.set(std::string{section} + "." + field.path, invalid);
            BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
         }
      }
   }
   for (const auto& flag : flags) {
      auto document = valid;
      document.set(std::string{section} + "." + flag.path, value::array_type{});
      BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
   }
   for (const auto* text : {
            "reachabilty: {}\n", "reachability: null\n", "reachability: []\n",
            "reachability: {ping-timeout: 3}\n", "reachability: {client-v1-enabled: 2}\n",
            "reachability: {timeout-ms: -1}\n", "reachability: {timeout-ms: 1.5}\n",
            "mdns: null\n", "mdns: []\n", "mdns: {record-ttl-ms: 120}\n",
            "mdns: {record-ttl-seconds: 18446744073709551616}\n", "mdns: {enabled: 2}\n",
            "relay: {max-candidates-per-refresh: 2}\n", "relay: {service-enabled: true}\n",
            "relay: {max-duration-ms: 1.5}\n", "relay: {require-reservation: false}\n"}) {
      BOOST_TEST_CONTEXT(text) {
         BOOST_CHECK_THROW(configure(runtime, plugin, yaml_candidate(text)), np::exceptions::invalid_config);
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_service_config_native_compound_bounds_and_duration_units_are_authoritative) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto& [path, number] : std::array{
            std::pair{"reachability.timeout-ms", std::uint64_t{86'400'001}},
            std::pair{"reachability.refresh-interval-ms", std::uint64_t{86'400'001}},
            std::pair{"reachability.observation-ttl-ms", std::uint64_t{19'999}},
            std::pair{"reachability.min-observers", std::uint64_t{65}},
            std::pair{"reachability.min-reachability-observers", std::uint64_t{65}},
            std::pair{"reachability.max-observers", std::uint64_t{3}},
            std::pair{"reachability.max-observations", std::uint64_t{63}},
            std::pair{"reachability.max-confirmed-per-local", std::uint64_t{257}},
            std::pair{"reachability.max-pending-probes", std::uint64_t{65}},
            std::pair{"reachability.service-per-peer-limit", std::uint64_t{31}},
            std::pair{"mdns.record-ttl-seconds", std::uint64_t{1}},
            std::pair{"mdns.record-ttl-seconds", std::uint64_t{3601}},
            std::pair{"mdns.query-interval-ms", std::uint64_t{999}},
            std::pair{"mdns.query-interval-ms", std::uint64_t{60'001}},
            std::pair{"mdns.max-packet-size", std::uint64_t{511}},
            std::pair{"mdns.max-packet-size", std::uint64_t{8933}},
            std::pair{"relay.target-reservations", std::uint64_t{5}},
            std::pair{"relay.max-parallel-reservations", std::uint64_t{5}},
            std::pair{"relay.refresh-margin-ms", std::uint64_t{86'400'001}},
            std::pair{"relay.candidate-backoff-ms", std::uint64_t{86'400'001}},
            std::pair{"relay.max-duration-ms", std::uint64_t{999}},
            std::pair{"relay.max-duration-ms", std::uint64_t{1001}},
            std::pair{"relay.max-service-requests", std::uint64_t{65'537}},
            std::pair{"relay.service-request-window-ms", std::uint64_t{3'600'001}},
            std::pair{"relay.handshake-timeout-ms", std::uint64_t{300'001}}}) {
      BOOST_TEST_CONTEXT(path << "=" << number) {
         auto document = valid_document();
         document.set(std::string{section} + "." + path, number);
         BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
      }
   }
   for (const auto* text : {
            "mdns: {enabled: true, ipv4-enabled: false, ipv6-enabled: false}\n",
            "mdns: {enabled: true}\ntopology: {mode: static-only}\n",
            "relay: {max-reservations: 1, target-reservations: 2}\n"}) {
      BOOST_CHECK_THROW(configure(runtime, plugin, yaml_candidate(text)), np::exceptions::invalid_config);
   }
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, yaml_candidate(
       "reachability: {timeout-ms: 86400000, observation-ttl-ms: 86400000}\n"
       "mdns: {record-ttl-seconds: 2, query-interval-ms: 1000, max-packet-size: 512}\n"
       "relay: {max-candidates: 1, target-reservations: 1, max-parallel-reservations: 1, max-duration-ms: 1000}\n")));
}

BOOST_AUTO_TEST_CASE(p2p_service_config_mdns_upper_bounds_are_checked_before_provider_binding) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto& [path, maximum] : std::array{
            std::pair{"mdns.max-interfaces", std::uint64_t{256}},
            std::pair{"mdns.max-pending-packets", std::uint64_t{1024}},
            std::pair{"mdns.max-records-per-packet", std::uint64_t{1024}},
            std::pair{"mdns.max-addresses-per-peer", std::uint64_t{64}}}) {
      for (const auto enabled : {false, true}) {
         BOOST_TEST_CONTEXT(path << " enabled=" << enabled) {
            auto document = valid_document();
            document.set(std::string{section} + ".mdns.enabled", enabled);
            document.set(std::string{section} + "." + path, maximum);
            BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
            document.set(std::string{section} + "." + path, maximum + 1);
            BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_service_config_existing_relay_paths_remain_single_authority) {
   const auto empty = forge::config::core::decode<np::config>({});
   BOOST_REQUIRE(empty.ok());
   BOOST_TEST(empty.value.relay.client_enabled);
   BOOST_TEST(!empty.value.relay.server_enabled);
   BOOST_TEST(empty.value.relay.auto_discovery_enabled);
   BOOST_TEST(!empty.value.relay.public_allowed);
   BOOST_TEST(empty.value.relay.reservation_ttl_ms == 60'000U);
   BOOST_TEST(empty.value.relay.max_candidates == 4U);
   BOOST_CHECK(empty.value.relay.trust == np::relay_trust_policy::known_only);
   for (const auto candidates : {std::uint64_t{1}, std::uint64_t{2}, std::uint64_t{17}}) {
      auto document = valid_document();
      document.set(std::string{section} + ".relay.max-candidates", candidates);
      document.set(std::string{section} + ".relay.reservation-ttl-ms", std::uint64_t{1234});
      document.set(std::string{section} + ".relay.trust", "public-allowed");
      document.set(std::string{section} + ".relay.server-enabled", true);
      const auto config = decode(document);
      const auto policy = np::parse_policy(config);
      const auto actual = mapped(config);
      BOOST_TEST(policy.path.max_relay_candidates == candidates);
      BOOST_TEST(actual.relay_policy.max_candidates_per_refresh == candidates);
      BOOST_TEST(actual.relay_policy.target_reservations == std::min(std::uint64_t{2}, candidates));
      BOOST_TEST(actual.limits.relay.reservation_ttl.count() == 1234);
      BOOST_TEST(actual.relay_policy.public_relay_allowed);
      BOOST_TEST(actual.relay_policy.service_enabled);
   }
   if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
      auto config = np::config{};
      config.relay.max_candidates = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()) + 1;
      BOOST_CHECK_THROW((void)np::parse_policy(config), np::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(p2p_service_config_unbound_providers_and_stop_freeze_without_native_service_start) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   auto document = yaml_candidate(
       "reachability: {service-v1-enabled: true, service-v2-enabled: true, max-event-subscribers: 3}\n"
       "mdns: {enabled: true, ipv6-enabled: false}\nrelay: {server-enabled: true, target-reservations: 3}\n");
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   auto apis = forge::api::core::registry{};
   auto installer = forge::api::core::installer{apis};
   run(runtime, plugin.provide(installer));
   const auto host = apis.get<np::api>(np::api::ref()).shared();
   const auto diagnostics = apis.get<np::diagnostics_api>(np::diagnostics_api::ref()).shared();
   BOOST_CHECK_THROW(static_cast<void>(host->local_endpoint()), np::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW(static_cast<void>(diagnostics->snapshot()), np::exceptions::plugin_not_initialized);
   plugin.request_stop();
   for (const auto* path : {"reachability.ping-interval-ms", "mdns.max-peers", "relay.max-service-requests"}) {
      auto candidate = document;
      candidate.set(std::string{section} + "." + path, std::uint64_t{1000});
      candidate.set(std::string{section} + ".diagnostics.max-peers", std::uint64_t{2});
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::route_conflict);
   }
   run(runtime, plugin.shutdown());
}
