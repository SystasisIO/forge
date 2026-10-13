module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.registry;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.signals;
import forge.asio.runtime;
import forge.asio.task;
import forge.log.logger;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.multiformats.multiaddr;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.rendezvous;
import forge.net.p2p.scoring;
import forge.net.p2p.topology;
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

enum class unit { count, milliseconds, seconds };
struct numeric_field {
   const char* path;
   std::uint64_t (*read)(const p2p::node::options&);
   std::uint64_t sample;
   unit units = unit::count;
};

constexpr auto numbers = std::array{
   numeric_field{"bootstrap-policy.startup-budget-ms", [](const auto& v) -> std::uint64_t {
      return v.lifecycle.startup_budget.count(); }, 5000, unit::milliseconds},
   numeric_field{"bootstrap-policy.connect-timeout-ms", [](const auto& v) -> std::uint64_t {
      return v.lifecycle.connect_timeout.count(); }, 1500, unit::milliseconds},
   numeric_field{"bootstrap-policy.max-parallel", [](const auto& v) -> std::uint64_t {
      return v.lifecycle.max_parallel_bootstrap; }, 7},
   numeric_field{"bootstrap-policy.retry-initial-delay-ms", [](const auto& v) -> std::uint64_t {
      return v.lifecycle.bootstrap_retry_initial_delay.count(); }, 1500, unit::milliseconds},
   numeric_field{"bootstrap-policy.retry-max-delay-ms", [](const auto& v) -> std::uint64_t {
      return v.lifecycle.bootstrap_retry_max_delay.count(); }, 20000, unit::milliseconds},
   numeric_field{"bootstrap-policy.maintenance-interval-ms", [](const auto& v) -> std::uint64_t {
      return v.lifecycle.maintenance_interval.count(); }, 1500, unit::milliseconds},
   numeric_field{"topology.peers.low", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.peers.low; }, 127},
   numeric_field{"topology.peers.target", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.peers.target; }, 159},
   numeric_field{"topology.peers.high", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.peers.high; }, 193},
   numeric_field{"topology.refresh-interval-ms", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.refresh_interval.count(); }, 5000, unit::milliseconds},
   numeric_field{"topology.query-timeout-ms", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.query_timeout.count(); }, 1500, unit::milliseconds},
   numeric_field{"topology.max-candidates", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_candidates; }, 17},
   numeric_field{"topology.max-parallel-queries", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_parallel_queries; }, 7},
   numeric_field{"topology.max-parallel-dials", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_parallel_dials; }, 3},
   numeric_field{"topology.max-tagged-peers", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_tagged_peers; }, 17},
   numeric_field{"topology.max-tags-per-peer", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_tags_per_peer; }, 7},
   numeric_field{"topology.max-tag-size", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_tag_size; }, 17},
   numeric_field{"rendezvous.max-points", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_rendezvous_points; }, 7},
   numeric_field{"rendezvous.max-namespaces", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_rendezvous_namespaces; }, 7},
   numeric_field{"rendezvous.default-ttl-seconds", [](const auto& v) -> std::uint64_t {
      return v.limits.rendezvous.default_ttl.count(); }, 8000, unit::seconds},
   numeric_field{"rendezvous.min-ttl-seconds", [](const auto& v) -> std::uint64_t {
      return v.limits.rendezvous.min_ttl.count(); }, 6000, unit::seconds},
   numeric_field{"rendezvous.max-ttl-seconds", [](const auto& v) -> std::uint64_t {
      return v.limits.rendezvous.max_ttl.count(); }, 9000, unit::seconds},
   numeric_field{"rendezvous.max-namespace-size", [](const auto& v) -> std::uint64_t {
      return v.limits.rendezvous.max_namespace_size; }, 17},
   numeric_field{"rendezvous.max-registrations-per-peer", [](const auto& v) -> std::uint64_t {
      return v.limits.rendezvous.max_registrations_per_peer; }, 17},
   numeric_field{"rendezvous.max-discover-limit", [](const auto& v) -> std::uint64_t {
      return v.limits.rendezvous.max_discover_limit; }, 17},
   numeric_field{"rendezvous.max-message-size", [](const auto& v) -> std::uint64_t {
      return v.limits.rendezvous.max_message_size; }, 1024},
   numeric_field{"peer-exchange.max-peers", [](const auto& v) -> std::uint64_t {
      return v.limits.topology.max_peer_exchange_peers; }, 7},
   numeric_field{"peer-exchange.max-message-size", [](const auto& v) -> std::uint64_t {
      return v.limits.max_peer_exchange_message_size; }, 1024},
   numeric_field{"peer-exchange.max-records", [](const auto& v) -> std::uint64_t {
      return v.limits.max_peer_exchange_records; }, 17},
   numeric_field{"peer-exchange.max-queue", [](const auto& v) -> std::uint64_t {
      return v.limits.max_peer_exchange_queue; }, 17},
   numeric_field{"path.max-direct-endpoints", [](const auto& v) -> std::uint64_t {
      return v.path_policy.max_direct_endpoints; }, 17},
};

forge::config::core::document valid_document() {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".peer-store.store", "unbound-store");
   document.set(std::string{section} + ".identity.certificate-secret", "unbound/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "unbound/key");
   return document;
}

np::config decode(const forge::config::core::document& document) {
   return np::decode_config({document, std::string{section}});
}

p2p::node::options mapped(const np::config& config, p2p::node::options native = {}) {
   np::apply_lifecycle(native, config);
   np::apply_topology(native, config, false);
   native.path_policy = np::parse_policy(config).path;
   return native;
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

BOOST_AUTO_TEST_CASE(p2p_logging_plugin_configuration_is_count_only_and_optional) {
   struct capture_sink final : forge::sink {
      std::mutex mutex;
      std::vector<forge::log_record> records;
      void log(const forge::log_record& record) override {
         const auto lock = std::scoped_lock{mutex};
         records.push_back(record);
      }
   };
   auto capture = std::make_shared<capture_sink>();
   auto previous = forge::logger::get("forge.plugins.net.p2p.node");
   auto route = forge::logger{"forge.plugins.net.p2p.node"};
   route.set_log_level(forge::log_level::debug);
   route.add_sink(capture);
   forge::logger::update("forge.plugins.net.p2p.node", route);
   auto restore = boost::scope::scope_exit{[&] { forge::logger::update("forge.plugins.net.p2p.node", previous); }};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   auto document = valid_document();
   document.set(std::string{section} + ".identity.certificate-secret", "unsafe-certificate-secret-sentinel");
   document.set(std::string{section} + ".identity.private-key-secret", "unsafe-key-secret-sentinel");
   configure(runtime, plugin, document);
#ifndef FORGE_DISABLE_LOGGING
   {
      const auto lock = std::scoped_lock{capture->mutex};
      BOOST_REQUIRE_EQUAL(capture->records.size(), 1U);
      const auto& record = capture->records.front();
      BOOST_TEST(record.logger == "forge.plugins.net.p2p.node");
      BOOST_TEST(record.level.value == forge::log_level::debug);
      BOOST_REQUIRE_EQUAL(record.fields.size(), 4U);
      BOOST_TEST(record.fields.front().key == "event");
      BOOST_TEST(record.fields.front().value == "config.committed");
      const auto text = forge::format_json_log_record(record);
      BOOST_TEST(text.find("unsafe-certificate-secret-sentinel") == std::string::npos);
      BOOST_TEST(text.find("unsafe-key-secret-sentinel") == std::string::npos);
      BOOST_TEST(text.find("unbound-store") == std::string::npos);
   }
#endif
   route.set_enabled(false);
   configure(runtime, plugin, document);
   const auto lock = std::scoped_lock{capture->mutex};
#ifndef FORGE_DISABLE_LOGGING
   BOOST_TEST(capture->records.size() == 1U);
#else
   BOOST_TEST(capture->records.empty());
#endif
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_config_defaults_and_every_numeric_patch_are_independent) {
   const auto defaults = mapped({});
   const auto native = p2p::node::options{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto& field : numbers) {
      BOOST_TEST_CONTEXT(field.path) {
         BOOST_TEST(field.read(defaults) == field.read(native));
         auto document = valid_document();
         document.set(std::string{section} + "." + field.path, field.sample);
         const auto actual = mapped(decode(document));
         BOOST_TEST(field.read(actual) == field.sample);
         for (const auto& other : numbers) {
            if (&other != &field) { BOOST_TEST(other.read(actual) == other.read(defaults)); }
         }
         BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
      }
   }
   BOOST_TEST(defaults.lifecycle.bootstrap_retry_jitter == native.lifecycle.bootstrap_retry_jitter);
   BOOST_TEST(defaults.limits.topology.retry_jitter == native.limits.topology.retry_jitter);
   BOOST_TEST(defaults.limits.topology.peer_exchange_enabled);
   BOOST_TEST(defaults.limits.rendezvous.require_signed_peer_record);
   BOOST_CHECK(defaults.limits.topology.operating_mode == p2p::topology::mode::managed);
   const auto empty = forge::config::core::decode<np::config>({});
   BOOST_REQUIRE(empty.ok());
   BOOST_CHECK(empty.value == np::config{});
   BOOST_TEST(empty.value.identity.certificate_secret.empty());
   BOOST_TEST(empty.value.identity.private_key_secret.empty());
   BOOST_TEST(empty.value.peer_store.store.empty());
   BOOST_CHECK(empty.value.peer_store.schema_policy == np::cache_schema_policy::reset);
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_config_sparse_new_fields_preserve_overrides_and_removal_resets_defaults) {
   auto native = p2p::node::options{};
   native.lifecycle.bootstrap_retry_initial_delay = 1700ms;
   native.lifecycle.bootstrap_retry_max_delay = 19000ms;
   native.lifecycle.bootstrap_retry_jitter = 0.11;
   native.lifecycle.maintenance_interval = 1700ms;
   native.limits.topology.max_tagged_peers = 17;
   native.limits.topology.max_tags_per_peer = 7;
   native.limits.topology.max_tag_size = 19;
   native.limits.topology.max_rendezvous_namespaces = 7;
   native.limits.rendezvous.default_ttl = 8000s;
   native.limits.rendezvous.min_ttl = 6000s;
   native.limits.rendezvous.max_ttl = 9000s;
   native.limits.rendezvous.max_namespace_size = 17;
   native.limits.rendezvous.max_registrations_per_peer = 19;
   native.limits.rendezvous.max_discover_limit = 23;
   native.limits.rendezvous.max_message_size = 1024;
   native.limits.max_peer_exchange_message_size = 2048;
   native.limits.max_peer_exchange_records = 17;
   native.limits.max_peer_exchange_queue = 19;
   const auto unchanged = mapped({}, native);
   for (const auto& field : numbers) {
      if (std::string_view{field.path}.find("retry-") != std::string_view::npos ||
          std::string_view{field.path} == "bootstrap-policy.maintenance-interval-ms" ||
          std::string_view{field.path}.starts_with("topology.max-tag") ||
          std::string_view{field.path} == "rendezvous.max-namespaces" ||
          (std::string_view{field.path}.starts_with("rendezvous.") &&
           std::string_view{field.path} != "rendezvous.max-points") ||
          (std::string_view{field.path}.starts_with("peer-exchange.") &&
           std::string_view{field.path} != "peer-exchange.max-peers")) {
         BOOST_TEST_CONTEXT(field.path) { BOOST_TEST(field.read(unchanged) == field.read(native)); }
      }
   }
   BOOST_TEST(unchanged.lifecycle.bootstrap_retry_jitter == 0.11);
   const auto reset = mapped(decode(valid_document()));
   const auto defaults = mapped({});
   for (const auto& field : numbers) { BOOST_TEST(field.read(reset) == field.read(defaults)); }
   BOOST_TEST(reset.lifecycle.bootstrap_retry_jitter == 0.20);
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_config_raw_dnsaddr_bootstrap_and_four_key_migration) {
   const auto document = yaml_candidate(
       "bootstrap: ['/dnsaddr/bootstrap.fixture.test']\n"
       "bootstrap-policy: {requirement: require-connection, startup-budget-ms: 5000, "
       "connect-timeout-ms: 1500, max-parallel: 7}\n"
       "identity: {certificate-secret: unbound/cert, private-key-secret: unbound/key}\n"
       "peer-store: {store: unbound-store, schema-policy: reject}\n");
   const auto config = decode(document);
   const auto actual = mapped(config);
   BOOST_REQUIRE_EQUAL(actual.lifecycle.bootstrap.size(), 1U);
   BOOST_TEST(actual.lifecycle.bootstrap.front().address.to_string() == "/dnsaddr/bootstrap.fixture.test");
   BOOST_CHECK(actual.lifecycle.requirement == p2p::bootstrap_requirement::require_connection);
   BOOST_CHECK(config.peer_store.schema_policy == np::cache_schema_policy::reject);
   BOOST_TEST(config.identity.certificate_secret == "unbound/cert");
   BOOST_TEST(config.identity.private_key_secret == "unbound/key");
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document)); // No initialize/startup or DNS query.
   for (const auto& [key, setting] : std::array{
            std::pair{"bootstrap-requirement", value{"allow-disconnected"}},
            std::pair{"bootstrap-startup-budget-ms", value{std::uint64_t{5000}}},
            std::pair{"bootstrap-connect-timeout-ms", value{std::uint64_t{1500}}},
            std::pair{"bootstrap-max-parallel", value{std::uint64_t{7}}}}) {
      BOOST_TEST_CONTEXT(key) {
         auto candidate = document;
         candidate.set(std::string{section} + "." + key, setting);
         BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
      }
   }
   auto malformed = document;
   malformed.set(std::string{section} + ".bootstrap", value::array_type{"not-a-multiaddr"});
   BOOST_CHECK_THROW(configure(runtime, plugin, malformed), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_config_checked_ms_seconds_counts_and_native_pex_wire_bounds) {
   const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
   const auto max_ms = static_cast<std::uint64_t>((std::numeric_limits<std::chrono::milliseconds::rep>::max)());
   const auto max_seconds = static_cast<std::uint64_t>((std::numeric_limits<std::chrono::seconds::rep>::max)());
   BOOST_TEST(np::to_ms(max_ms).count() == (std::numeric_limits<std::chrono::milliseconds::rep>::max)());
   BOOST_TEST(np::to_seconds(max_seconds).count() == (std::numeric_limits<std::chrono::seconds::rep>::max)());
   BOOST_CHECK_THROW((void)np::to_ms(maximum), np::exceptions::invalid_config);
   BOOST_CHECK_THROW((void)np::to_seconds(maximum), np::exceptions::invalid_config);
   for (const auto& field : numbers) {
      auto document = valid_document();
      document.set(std::string{section} + "." + field.path, maximum);
      BOOST_TEST_CONTEXT(field.path) {
         if (field.units != unit::count || sizeof(std::size_t) < sizeof(std::uint64_t)) {
            BOOST_CHECK_THROW(mapped(decode(document)), np::exceptions::invalid_config);
         }
      }
   }
   auto native_size = std::size_t{};
   const auto max_size = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
   np::apply_size(native_size, max_size, "checked-count");
   BOOST_TEST(native_size == (std::numeric_limits<std::size_t>::max)());
   if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
      BOOST_CHECK_THROW(np::apply_size(native_size, maximum, "checked-count"), np::exceptions::invalid_config);
      BOOST_TEST(native_size == (std::numeric_limits<std::size_t>::max)());
   }
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto* field : {"peer-exchange.max-message-size", "peer-exchange.max-records"}) {
      auto document = valid_document();
      const auto boundary = static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
      document.set(std::string{section} + "." + field, boundary);
      BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
      document.set(std::string{section} + "." + field, boundary + 1);
      BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_config_invalid_numbers_jitter_and_compound_candidates_are_typed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto& field : numbers) {
      for (const auto& bad : std::array{value{std::uint64_t{0}}, value{std::int64_t{-1}}, value{1.5}}) {
         BOOST_TEST_CONTEXT(field.path) {
            auto document = valid_document();
            document.set(std::string{section} + "." + field.path, bad);
            BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
         }
      }
   }
   for (const auto* field : {"bootstrap-policy.retry-jitter", "topology.retry-jitter"}) {
      for (const auto bad : {-0.01, 1.0, (std::numeric_limits<double>::quiet_NaN)(),
                            (std::numeric_limits<double>::infinity)(), -(std::numeric_limits<double>::infinity)()}) {
         auto document = valid_document();
         document.set(std::string{section} + "." + field, bad);
         BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
      }
      for (const auto good : {0.0, 0.20}) {
         auto document = valid_document();
         document.set(std::string{section} + "." + field, good);
         const auto actual = mapped(decode(document));
         BOOST_TEST((std::string_view{field}.starts_with("bootstrap") ? actual.lifecycle.bootstrap_retry_jitter :
                     actual.limits.topology.retry_jitter) == good);
         BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
      }
   }
   for (const auto* text : {
            "bootstrap-policy: {requirement: require-connection}\n",
            "bootstrap-policy: {retry-initial-delay-ms: 30001, retry-max-delay-ms: 30000}\n",
            "bootstrap-policy: {retry-max-delay-ms: 3600001}\n",
            "bootstrap-policy: {retry-jitter: 0.21}\n",
            "bootstrap-policy: {maintenance-interval-ms: 600001}\n",
            "topology: {peers: {low: 161}}\n",
            "topology: {peers: {target: 193}}\n",
            "rendezvous: {min-ttl-seconds: 9000, max-ttl-seconds: 8000}\n",
            "rendezvous: {min-ttl-seconds: 1, max-ttl-seconds: 60}\n",
            "rendezvous: {default-ttl-seconds: 7199}\n",
            "rendezvous: {default-ttl-seconds: 259201}\n",
            "path: {max-direct-endpoints: 101}\n"}) {
      BOOST_CHECK_THROW(configure(runtime, plugin, yaml_candidate(text)), np::exceptions::invalid_config);
   }
   auto invalid = np::config{};
   invalid.bootstrap_policy.requirement = static_cast<np::bootstrap_requirement>(255);
   BOOST_CHECK_THROW(mapped(invalid), np::exceptions::invalid_config);
   invalid = {};
   invalid.topology.mode = static_cast<np::topology_mode>(255);
   BOOST_CHECK_THROW(mapped(invalid), np::exceptions::invalid_config);
   invalid = {};
   invalid.rendezvous.role = static_cast<np::rendezvous_role>(255);
   BOOST_CHECK_THROW(mapped(invalid), np::exceptions::invalid_config);
   invalid = {};
   invalid.path.policy = static_cast<np::path_policy>(255);
   BOOST_CHECK_THROW(mapped(invalid), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_config_strict_unknown_yaml_and_existing_themed_paths) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto* text : {
            "bootstrap-policy: {max-paralell: 4}\n", "topolgy: {}\n", "topology: {peers: {lo: 1}}\n",
            "rendezvous: {require-signed-peer-record: false}\n", "peer-exchange: {queue: 4}\n",
            "path: {max-direct-endpoint: 4}\n",
            "identity: {certificate-secret: unbound/cert, private-key-secret: unbound/key, inline-key: nope}\n",
            "peer-store: {store: unbound-store, policy: reject}\n", "bootstrap-policy: []\n"}) {
      BOOST_CHECK_THROW(configure(runtime, plugin, yaml_candidate(text)), np::exceptions::invalid_config);
   }
   for (const auto* policy : {"direct-only", "direct-preferred", "relay-only"}) {
      auto document = valid_document();
      document.set(std::string{section} + ".path.policy", policy);
      BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   }
   auto document = valid_document();
   document.set(std::string{section} + ".enabled", true);
   document.set("unrelated.bootstrap-requirement", "legacy-key-outside-node");
   document.set(std::string{section} + ".peer-exchange.enabled", false);
   document.set(std::string{section} + ".topology.mode", "static-only");
   const auto actual = mapped(decode(document));
   BOOST_CHECK(actual.limits.topology.operating_mode == p2p::topology::mode::static_only);
   BOOST_TEST(!actual.limits.topology.peer_exchange_enabled);
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
}

BOOST_AUTO_TEST_CASE(p2p_lifecycle_config_listenerless_candidate_rollback_and_group_freeze) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = forge::asio::task::scheduler{runtime};
   auto apis = forge::api::core::registry{};
   auto signals = forge::app::signal_bus{};
   auto events = forge::app::event_bus{};
   auto context = forge::app::plugin_context{scheduler, apis, signals, events};
   auto plugin = np::plugin{};
   auto cleanup = boost::scope::scope_exit{[&] {
      plugin.request_stop();
      auto node_error = std::exception_ptr{};
      auto scheduler_error = std::exception_ptr{};
      try { run(runtime, plugin.shutdown()); } catch (...) { node_error = std::current_exception(); }
      try { run(runtime, scheduler.shutdown()); } catch (...) { scheduler_error = std::current_exception(); }
      if (node_error) { BOOST_ERROR("listenerless lifecycle node cleanup failed after both joins"); }
      if (scheduler_error) { BOOST_ERROR("listenerless lifecycle scheduler cleanup failed after both joins"); }
   }};
   // Explicitly no transport/identity/persistence evidence: no listener or providers.
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".allow-insecure-test-mode", true);
   document.set(std::string{section} + ".peer-id", p2p::make_peer_id({
       .type = p2p::public_key::type::ed25519, .data = std::vector<std::uint8_t>(32, 0x73)}).value);
   document.set(std::string{section} + ".topology.mode", "static-only");
   document.set(std::string{section} + ".topology.peers.low", std::uint64_t{7});
   document.set(std::string{section} + ".topology.peers.target", std::uint64_t{9});
   document.set(std::string{section} + ".topology.peers.high", std::uint64_t{11});
   document.set(std::string{section} + ".peer-exchange.max-peers", std::uint64_t{7});
   configure(runtime, plugin, document);
   auto rejected = document;
   rejected.set(std::string{section} + ".topology.peers.low", std::uint64_t{70});
   rejected.set(std::string{section} + ".topology.peers.target", std::uint64_t{90});
   rejected.set(std::string{section} + ".topology.peers.high", std::uint64_t{110});
   rejected.set(std::string{section} + ".bootstrap-policy.retry-initial-delay-ms", std::uint64_t{30001});
   BOOST_CHECK_THROW(configure(runtime, plugin, rejected), np::exceptions::invalid_config);
   for (const auto ttl : {std::uint64_t{7199}, std::uint64_t{259201}}) {
      auto invalid_ttl = document;
      invalid_ttl.set(std::string{section} + ".topology.peers.low", std::uint64_t{70});
      invalid_ttl.set(std::string{section} + ".topology.peers.target", std::uint64_t{90});
      invalid_ttl.set(std::string{section} + ".topology.peers.high", std::uint64_t{110});
      invalid_ttl.set(std::string{section} + ".rendezvous.default-ttl-seconds", ttl);
      BOOST_CHECK_THROW(configure(runtime, plugin, invalid_ttl), np::exceptions::invalid_config);
   }
   for (const auto& [path, maximum] : std::array{
            std::pair{"mdns.max-interfaces", std::uint64_t{256}},
            std::pair{"mdns.max-pending-packets", std::uint64_t{1024}},
            std::pair{"mdns.max-records-per-packet", std::uint64_t{1024}},
            std::pair{"mdns.max-addresses-per-peer", std::uint64_t{64}}}) {
      auto invalid_mdns = document;
      invalid_mdns.set(std::string{section} + ".topology.peers.low", std::uint64_t{70});
      invalid_mdns.set(std::string{section} + ".topology.peers.target", std::uint64_t{90});
      invalid_mdns.set(std::string{section} + ".topology.peers.high", std::uint64_t{110});
      invalid_mdns.set(std::string{section} + "." + path, maximum + 1);
      BOOST_CHECK_THROW(configure(runtime, plugin, invalid_mdns), np::exceptions::invalid_config);
   }
   auto installer = forge::api::core::installer{apis};
   run(runtime, plugin.provide(installer));
   run(runtime, plugin.initialize(context));
   run(runtime, plugin.after_initialize());
   run(runtime, plugin.startup()); // No successful configure after the rejected candidate.
   const auto host = apis.get<np::api>(np::api::ref()).shared();
   const auto diagnostics = apis.get<np::diagnostics_api>(np::diagnostics_api::ref()).shared();
   BOOST_TEST(host->local_endpoints().empty());
   const auto original = diagnostics->snapshot();
   BOOST_TEST(original.topology.low_watermark == 7U);
   BOOST_TEST(original.topology.target_watermark == 9U);
   BOOST_TEST(original.topology.high_watermark == 11U);
   BOOST_TEST(original.topology.max_peer_exchange_peers == 7U);
   for (const auto& [path, setting] : std::array{
            std::pair{"bootstrap-policy.maintenance-interval-ms", value{std::uint64_t{1700}}},
            std::pair{"identity.certificate-secret", value{"new/cert"}},
            std::pair{"peer-store.store", value{"new/store"}},
            std::pair{"path.max-direct-endpoints", value{std::uint64_t{17}}},
            std::pair{"topology.peers.low", value{std::uint64_t{8}}},
            std::pair{"rendezvous.max-discover-limit", value{std::uint64_t{17}}},
            std::pair{"peer-exchange.max-records", value{std::uint64_t{17}}}}) {
      auto candidate = document;
      candidate.set(std::string{section} + "." + path, setting);
      candidate.set(std::string{section} + ".diagnostics.max-peers", std::uint64_t{2});
      BOOST_TEST_CONTEXT(path) { BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::route_conflict); }
      BOOST_TEST(diagnostics->snapshot().topology.low_watermark == 7U);
   }
   plugin.request_stop();
   BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::route_conflict);
   run(runtime, plugin.shutdown());
   BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::route_conflict);
}
