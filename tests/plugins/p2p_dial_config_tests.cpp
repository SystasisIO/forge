module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
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
import forge.net.dns.types;
import forge.net.p2p.address_resolution;
import forge.net.p2p.dialing;
import forge.net.p2p.endpoint;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.schema.object;

#include "../../plugins/net/p2p/node/details/configuration_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::string_view section = "plugins.net.p2p.node";

struct numeric_field {
   const char* path;
   std::uint64_t (*read)(const p2p::node::options&);
   std::uint64_t ceiling;
   bool duration = false;
};

constexpr auto fields = std::array{
   numeric_field{"dns.resolution.max-dns-lookups", [](const auto& v) -> std::uint64_t {
      return v.dns_resolution.bounds.max_dns_lookups; }, 32},
   numeric_field{"dns.resolution.max-txt-records", [](const auto& v) -> std::uint64_t {
      return v.dns_resolution.bounds.max_txt_records; }, 16},
   numeric_field{"dns.resolution.max-resolved-addresses", [](const auto& v) -> std::uint64_t {
      return v.dns_resolution.bounds.max_resolved_addresses; }, 100},
   numeric_field{"dns.resolution.max-recursion-depth", [](const auto& v) -> std::uint64_t {
      return v.dns_resolution.bounds.max_recursion_depth; }, 4},
   numeric_field{"dns.resolution.max-multiaddr-size", [](const auto& v) -> std::uint64_t {
      return v.dns_resolution.bounds.max_multiaddr_size; }, 4096},
   numeric_field{"dns.resolution.max-roots", [](const auto& v) -> std::uint64_t {
      return v.dns_resolution.bounds.max_roots; }, 100},
   numeric_field{"dns.resolver.max-in-flight", [](const auto& v) -> std::uint64_t {
      return v.dns_resolver.max_in_flight; }, 1024},
   numeric_field{"dial.ranker.public-delay-ms", [](const auto& v) -> std::uint64_t {
      return v.direct_dial.ranker.public_delay.count(); }, 250, true},
   numeric_field{"dial.ranker.private-delay-ms", [](const auto& v) -> std::uint64_t {
      return v.direct_dial.ranker.private_delay.count(); }, 30, true},
   numeric_field{"dial.black-holes.window-size", [](const auto& v) -> std::uint64_t {
      return v.direct_dial.black_holes.window_size; }, 100},
   numeric_field{"dial.black-holes.min-successes", [](const auto& v) -> std::uint64_t {
      return v.direct_dial.black_holes.min_successes; }, 100},
   numeric_field{"dial.max-concurrent-attempts", [](const auto& v) -> std::uint64_t {
      return v.direct_dial.max_concurrent_attempts; }, 4},
   numeric_field{"path.max-direct-endpoints", [](const auto& v) -> std::uint64_t {
      return v.path_policy.max_direct_endpoints; }, 100},
};

forge::config::core::document valid_document() {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".peer-store.store", "unbound-store");
   document.set(std::string{section} + ".identity.certificate-secret", "unbound/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "unbound/key");
   document.set(std::string{section} + ".topology.mode", "static-only");
   return document;
}

np::config decode(const forge::config::core::document& document) {
   return np::decode_config({document, std::string{section}});
}

p2p::node::options mapped(const np::config& config, p2p::node::options native = {}) {
   np::apply_dns(native, config.dns);
   np::apply_dial(native, config.dial);
   np::apply_path(native.path_policy, config.path.max_direct_endpoints);
   return native;
}

void check_settings(const p2p::node::options& actual, const p2p::node::options& expected) {
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.path) { BOOST_TEST(field.read(actual) == field.read(expected)); }
   }
   BOOST_TEST(actual.direct_dial.black_holes.udp_enabled == expected.direct_dial.black_holes.udp_enabled);
   BOOST_TEST(actual.direct_dial.black_holes.ipv6_enabled == expected.direct_dial.black_holes.ipv6_enabled);
   BOOST_REQUIRE_EQUAL(actual.dns_resolver.nameservers.size(), expected.dns_resolver.nameservers.size());
   for (auto index = std::size_t{0}; index != actual.dns_resolver.nameservers.size(); ++index) {
      BOOST_TEST(actual.dns_resolver.nameservers[index].address == expected.dns_resolver.nameservers[index].address);
      BOOST_TEST(actual.dns_resolver.nameservers[index].port == expected.dns_resolver.nameservers[index].port);
      BOOST_TEST(actual.dns_resolver.nameservers[index].scope_id == expected.dns_resolver.nameservers[index].scope_id);
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

value nameservers(std::string address, std::uint64_t port = 53, std::string scope = {}) {
   return value::array_type{value::object_type{
       {"address", std::move(address)}, {"port", port}, {"scope-id", std::move(scope)}}};
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_dial_config_defaults_and_each_numeric_field_map_without_cross_field_mutation) {
   const auto empty = forge::config::core::decode<np::config>({});
   BOOST_REQUIRE(empty.ok());
   BOOST_TEST(!empty.value.dns.resolver.nameservers);
   BOOST_TEST(!empty.value.path.max_direct_endpoints);
   check_settings(mapped(empty.value), p2p::node::options{});
   for (const auto& field : fields) {
      for (const auto number : {std::uint64_t{0}, std::uint64_t{1}, std::uint64_t{123}}) {
         BOOST_TEST_CONTEXT(field.path << "=" << number) {
            auto document = valid_document();
            document.set(std::string{section} + "." + field.path, number);
            const auto actual = mapped(decode(document));
            BOOST_TEST(field.read(actual) == number);
            const auto defaults = p2p::node::options{};
            for (const auto& other : fields) {
               if (&other != &field) { BOOST_TEST(other.read(actual) == other.read(defaults)); }
            }
            BOOST_TEST(actual.dns_resolver.nameservers.empty());
            BOOST_TEST(actual.direct_dial.black_holes.udp_enabled);
            BOOST_TEST(actual.direct_dial.black_holes.ipv6_enabled);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_dial_config_sparse_overlay_and_removal_restore_native_base) {
   auto native = p2p::node::options{};
   native.dns_resolution.bounds.max_dns_lookups = 7;
   native.dns_resolver.max_in_flight = 3;
   native.dns_resolver.nameservers = {{.address = "127.0.0.2", .port = 5353}};
   native.direct_dial.ranker.public_delay = 11ms;
   native.direct_dial.black_holes.udp_enabled = false;
   native.path_policy.max_direct_endpoints = 19;
   check_settings(mapped({}, native), native);
   auto patch = np::config{};
   patch.dns.resolver.nameservers = std::vector<np::dns_config::nameserver_config>{};
   patch.dial.black_holes.ipv6_enabled = false;
   auto expected = native;
   expected.dns_resolver.nameservers.clear();
   expected.direct_dial.black_holes.ipv6_enabled = false;
   check_settings(mapped(patch, native), expected);

   auto document = valid_document();
   for (const auto& field : fields) { document.set(std::string{section} + "." + field.path, field.ceiling); }
   document.set(std::string{section} + ".dns.resolver.nameservers", nameservers("127.0.0.1", 5353));
   document.set(std::string{section} + ".dial.black-holes.udp-enabled", false);
   document.set(std::string{section} + ".dial.black-holes.ipv6-enabled", false);
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   configure(runtime, plugin, document);
   document.set(std::string{section} + ".dns", value::object_type{});
   document.set(std::string{section} + ".dial", value::object_type{});
   document.set(std::string{section} + ".path", value::object_type{});
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   check_settings(mapped(decode(document)), p2p::node::options{});
   BOOST_TEST(np::parse_policy(decode(document)).path.max_direct_endpoints == p2p::path::policy{}.max_direct_endpoints);
}

BOOST_AUTO_TEST_CASE(p2p_dial_config_valid_yaml_maps_nameserver_defaults_scope_and_explicit_false) {
   const auto document = yaml_candidate(
       "dns:\n  resolution:\n    max-dns-lookups: 7\n    max-txt-records: 3\n"
       "    max-resolved-addresses: 6\n    max-recursion-depth: 2\n    max-multiaddr-size: 512\n    max-roots: 9\n"
       "  resolver:\n    max-in-flight: 2\n    nameservers:\n      - address: 127.0.0.1\n"
       "      - address: '::1'\n        port: 5353\n      - address: 'fe80::1'\n        scope-id: zone-1\n"
       "dial:\n  ranker:\n    public-delay-ms: 17\n    private-delay-ms: 3\n"
       "  black-holes:\n    window-size: 20\n    min-successes: 1\n    udp-enabled: false\n    ipv6-enabled: false\n"
       "  max-concurrent-attempts: 2\npath:\n  policy: direct-only\n  max-direct-endpoints: 17\n");
   const auto config = decode(document);
   BOOST_REQUIRE(config.dns.resolver.nameservers);
   BOOST_REQUIRE_EQUAL(config.dns.resolver.nameservers->size(), 3U);
   BOOST_TEST(config.dns.resolver.nameservers->front().port == 53U);
   auto expected = p2p::node::options{};
   expected.dns_resolution.bounds = {.max_dns_lookups = 7, .max_txt_records = 3, .max_resolved_addresses = 6,
       .max_recursion_depth = 2, .max_multiaddr_size = 512, .max_roots = 9};
   expected.dns_resolver = {.nameservers = {{.address = "127.0.0.1"}, {.address = "::1", .port = 5353},
       {.address = "fe80::1", .scope_id = "zone-1"}}, .max_in_flight = 2};
   expected.direct_dial.ranker = {.public_delay = 17ms, .private_delay = 3ms};
   expected.direct_dial.black_holes = {.window_size = 20, .min_successes = 1, .udp_enabled = false, .ipv6_enabled = false};
   expected.direct_dial.max_concurrent_attempts = 2;
   expected.path_policy.max_direct_endpoints = 17;
   check_settings(mapped(config), expected);
   const auto policy = np::parse_policy(config);
   BOOST_TEST(policy.path.max_direct_endpoints == 17U);
   BOOST_TEST(policy.path.allow_direct);
   BOOST_TEST(!policy.path.allow_relay);
   BOOST_TEST(!policy.path.allow_hole_punch);
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
}

BOOST_AUTO_TEST_CASE(p2p_dial_config_bad_numbers_and_native_bounds_reject_before_commit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto valid = valid_document();
   configure(runtime, plugin, valid);
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.path) {
         for (const auto& invalid : {value{std::int64_t{-1}}, value{1.5}, value{"18446744073709551616"},
                                     value{std::uint64_t{0}}, value{field.ceiling + 1}}) {
            auto candidate = valid;
            candidate.set(std::string{section} + "." + field.path, invalid);
            BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
         }
         auto candidate = valid;
         candidate.set(std::string{section} + "." + field.path, field.ceiling);
         BOOST_CHECK_NO_THROW(configure(runtime, plugin, candidate));
      }
   }
   auto candidate = valid;
   candidate.set(std::string{section} + ".dial.black-holes.window-size", std::uint64_t{21});
   candidate.set(std::string{section} + ".dial.black-holes.min-successes", std::uint64_t{1});
   BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
   candidate.set(std::string{section} + ".dial.black-holes.min-successes", std::uint64_t{2});
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, candidate));
   candidate.set(std::string{section} + ".dial.black-holes.min-successes", std::uint64_t{22});
   BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_dial_config_checked_count_duration_and_nameserver_port_ranges) {
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.path) {
         const auto maximum = field.duration
             ? static_cast<std::uint64_t>((std::numeric_limits<std::chrono::milliseconds::rep>::max)())
             : static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
         auto document = valid_document();
         document.set(std::string{section} + "." + field.path, maximum);
         BOOST_TEST(field.read(mapped(decode(document))) == maximum);
         if (field.duration || sizeof(std::size_t) < sizeof(std::uint64_t)) {
            // Count overflow is exercised only on a narrower size_t target.
            document.set(std::string{section} + "." + field.path, maximum + 1);
            BOOST_CHECK_THROW(mapped(decode(document)), np::exceptions::invalid_config);
         }
      }
   }
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto port : {std::uint64_t{1}, std::uint64_t{53}, std::uint64_t{65535}}) {
      auto document = valid_document();
      document.set(std::string{section} + ".dns.resolver.nameservers", nameservers("127.0.0.1", port));
      BOOST_TEST(mapped(decode(document)).dns_resolver.nameservers.front().port == port);
      BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   }
   for (const auto port : {std::uint64_t{0}, std::uint64_t{65536}, (std::numeric_limits<std::uint64_t>::max)()}) {
      auto document = valid_document();
      document.set(std::string{section} + ".dns.resolver.nameservers", nameservers("127.0.0.1", port));
      BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(p2p_dial_config_strict_yaml_and_invalid_nameserver_scope_remain_typed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto* text : {
            "dnas: {}\n", "dail: {}\n", "dns: null\n", "dial: []\n",
            "dns:\n  resolution: []\n", "dns:\n  resolver: null\n",
            "dns:\n  resolution:\n    max-root: 1\n", "dns:\n  resolver:\n    timeout-ms: 1\n",
            "dns:\n  resolver:\n    nameservers: localhost\n", "dns:\n  resolver:\n    nameservers: [{}]\n",
            "dns:\n  resolver:\n    nameservers: [{address: true}]\n",
            "dns:\n  resolver:\n    nameservers: [{address: 127.0.0.1, port: -1}]\n",
            "dns:\n  resolver:\n    nameservers: [{address: 127.0.0.1, port: 1.5}]\n",
            "dns:\n  resolver:\n    nameservers: [{address: 127.0.0.1, port: 18446744073709551616}]\n",
            "dns:\n  resolver:\n    nameservers: [{address: 127.0.0.1, scope-id: []}]\n",
            "dial:\n  ranker: false\n", "dial:\n  black-holes: []\n",
            "dial:\n  attempt-timeout-ms: 1\n", "dial:\n  black-holes:\n    udp-enabled: 2\n",
            "dial:\n  black-holes:\n    ipv6-enabled: []\n", "path:\n  max-direct-endpoint: 1\n"}) {
      BOOST_TEST_CONTEXT(text) {
         BOOST_CHECK_THROW(configure(runtime, plugin, yaml_candidate(text)), np::exceptions::invalid_config);
      }
   }
   for (const auto& [address, scope] : std::array{
            std::pair{"", ""}, std::pair{"nameserver.example", ""}, std::pair{"127.0.0.1", "zone-1"},
            std::pair{"::1", "zone-1"}, std::pair{"fe80::1", ""}, std::pair{"fe80::1", "%zone"},
            std::pair{"fe80::1", "bad zone"}, std::pair{"fe80::1", "bad:zone"}}) {
      auto document = valid_document();
      document.set(std::string{section} + ".dns.resolver.nameservers", nameservers(address, 53, scope));
      BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
   }
   auto document = valid_document();
   document.set(std::string{section} + ".dns.resolver.nameservers", nameservers("fe80::1", 53, "zone-1"));
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   document.set(std::string{section} + ".dns.resolver.nameservers", value::array_type{});
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   BOOST_TEST(mapped(decode(document)).dns_resolver.nameservers.empty());
}

BOOST_AUTO_TEST_CASE(p2p_dial_config_configure_needs_no_providers_or_native_node_and_stop_freezes_updates) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   auto document = valid_document();
   document.set(std::string{section} + ".listen", value::array_type{value{"/ip4/127.0.0.1/tcp/0"}});
   document.set(std::string{section} + ".dns.resolver.nameservers", nameservers("203.0.113.7", 65535));
   document.set(std::string{section} + ".path.max-direct-endpoints", std::uint64_t{19});
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   BOOST_TEST(np::parse_policy(decode(document)).path.max_direct_endpoints == 19U);
   auto apis = forge::api::core::registry{};
   auto installer = forge::api::core::installer{apis};
   run(runtime, plugin.provide(installer));
   const auto host = apis.get<np::api>(np::api::ref()).shared();
   const auto diagnostics = apis.get<np::diagnostics_api>(np::diagnostics_api::ref()).shared();
   BOOST_CHECK_THROW(static_cast<void>(host->local_endpoint()), np::exceptions::plugin_not_initialized);
   BOOST_CHECK_THROW(static_cast<void>(diagnostics->snapshot()), np::exceptions::plugin_not_initialized);
   plugin.request_stop();
   document.set(std::string{section} + ".dns.resolution.max-roots", std::uint64_t{1});
   document.set(std::string{section} + ".dial.max-concurrent-attempts", std::uint64_t{1});
   document.set(std::string{section} + ".path.max-direct-endpoints", std::uint64_t{1});
   BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::route_conflict);
   run(runtime, plugin.shutdown());
}
