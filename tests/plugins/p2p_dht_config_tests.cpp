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
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.app.exceptions;
import forge.app.plugin_lifecycle;
import forge.asio.runtime;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.db.core.record;
import forge.net.p2p.dht;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;

#include "../../plugins/net/p2p/node/details/configuration_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using patch = np::dht_profile_config::records_config;
using records = p2p::dht::record_store::options;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::string_view section = "plugins.net.p2p.node";
constexpr std::string_view protocol = "/configuration/records/kad/1";

struct numeric_field {
   const char* name;
   std::optional<std::uint64_t> patch::* configured;
   std::size_t records::* native;
   std::uint64_t sample;
};
constexpr auto fields = std::array{
   numeric_field{"max-values", &patch::max_values, &records::max_values, 7},
   numeric_field{"max-total-bytes", &patch::max_total_bytes, &records::max_total_bytes, 32768},
   numeric_field{"hydration-page-limit", &patch::hydration_page_limit, &records::hydration_page_limit, 17},
   numeric_field{"prune-page-limit", &patch::prune_page_limit, &records::prune_page_limit, 19},
   numeric_field{"max-hydration-pages", &patch::max_hydration_pages, &records::max_hydration_pages, 23},
   numeric_field{"max-providers", &patch::max_providers, &records::max_providers, 17},
   numeric_field{"max-local-providers", &patch::max_local_providers, &records::max_local_providers, 7},
};
static_assert(std::is_same_v<decltype(patch::max_total_bytes), std::optional<std::uint64_t>>);

np::dht_profile_config custom() {
   auto result = np::dht_profile_config{};
   result.kind = np::dht_profile_kind::custom;
   result.protocol = protocol;
   return result;
}

forge::config::core::document document_with(value::object_type record_fields = {}) {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".peer-store.store", "unbound-store");
   document.set(std::string{section} + ".identity.certificate-secret", "unbound/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "unbound/key");
   document.set(std::string{section} + ".dht.profiles", value::array_type{
       value::object_type{{"kind", "custom"}, {"protocol", std::string{protocol}}, {"mode", "server"},
                          {"records", std::move(record_fields)}}});
   return document;
}

np::config decode(const forge::config::core::document& document) {
   return np::decode_config({document, std::string{section}});
}

p2p::node::options mapped(const std::vector<np::dht_profile_config>& profiles) {
   auto native = p2p::node::options{};
   np::apply_dht_profiles(native, profiles);
   return native;
}

const records& record_options(const p2p::node::options& native, std::string_view id = protocol) {
   const auto found = native.dht_records.find(p2p::protocol_id{.value = std::string{id}});
   BOOST_REQUIRE(found != native.dht_records.end());
   return found->second;
}

void check_records(const records& actual, const records& expected) {
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.name) { BOOST_TEST((actual.*(field.native)) == (expected.*(field.native))); }
   }
   BOOST_TEST(actual.max_record_bytes == expected.max_record_bytes);
   BOOST_TEST(actual.max_providers_per_key == expected.max_providers_per_key);
   BOOST_TEST(actual.max_persistence_waiters == expected.max_persistence_waiters);
   BOOST_CHECK(actual.persistence == expected.persistence);
   BOOST_CHECK(!actual.public_keys);
}

template<class T> T run(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (result.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   return result.get();
}

void configure(forge::asio::runtime& runtime, np::plugin& plugin, const forge::config::core::document& document) {
   run(runtime, plugin.configure({document, std::string{section}}));
}

void configure_fresh(forge::asio::runtime& runtime, const forge::config::core::document& document) {
   auto owner = np::plugin{};
   configure(runtime, owner, document);
}

void check_repeated_config(forge::asio::runtime& runtime, np::plugin& plugin,
                           const forge::config::core::document& document) {
   const auto before = plugin.lifecycle().snapshot();
   BOOST_CHECK_THROW(configure(runtime, plugin, document), forge::app::exceptions::invalid_state);
   const auto after = plugin.lifecycle().snapshot();
   BOOST_CHECK(after.phase == before.phase);
   BOOST_TEST(after.epoch == before.epoch);
   BOOST_TEST(after.generation == before.generation);
   BOOST_TEST(after.completed_operations == before.completed_operations);
   BOOST_TEST(after.admission_open == before.admission_open);
   BOOST_TEST(after.failure.has_value() == before.failure.has_value());
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_dht_config_records_defaults_derive_from_each_declared_profile) {
   auto configured = std::vector{np::dht_profile_config{}, custom()};
   const auto native = mapped(configured);
   BOOST_REQUIRE_EQUAL(native.dht_profiles.size(), 2U);
   BOOST_REQUIRE_EQUAL(native.dht_records.size(), 2U);
   for (const auto& profile : native.dht_profiles) {
      const auto expected = p2p::dht::record_store::options_for(profile);
      check_records(record_options(native, profile.protocol.value), expected);
   }
   const auto empty = mapped({});
   BOOST_TEST(empty.dht_profiles.empty());
   BOOST_TEST(empty.dht_records.empty());
}

BOOST_AUTO_TEST_CASE(p2p_dht_config_every_sparse_record_patch_maps_without_changing_other_budgets) {
   const auto defaults = mapped({custom()});
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.name) {
         auto plugin = np::plugin{};
         const auto document = document_with({{field.name, value{field.sample}}});
         const auto config = decode(document);
         BOOST_REQUIRE_EQUAL(config.dht_profiles.size(), 1U);
         BOOST_REQUIRE((config.dht_profiles.front().records.*(field.configured)).has_value());
         BOOST_TEST(*(config.dht_profiles.front().records.*(field.configured)) == field.sample);
         const auto native = mapped(config.dht_profiles);
         auto expected = record_options(defaults);
         expected.*(field.native) = static_cast<std::size_t>(field.sample);
         check_records(record_options(native), expected);
         // Missing custom value callbacks are allowed in declaration preflight;
         // no providers are bound and no Secrets/ObjectDB operation is started.
         BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
         check_repeated_config(runtime, plugin, document);
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_dht_config_per_profile_isolation_removal_and_failed_mapper_are_atomic) {
   auto first = custom();
   first.records.max_values = 3;
   first.records.max_total_bytes = 1024;
   auto second = custom();
   second.protocol = "/configuration/other/kad/1";
   second.records.max_values = 7;
   auto native = mapped({first, second});
   BOOST_TEST(record_options(native).max_values == 3U);
   BOOST_TEST(record_options(native).max_total_bytes == 1024U);
   BOOST_TEST(record_options(native, second.protocol).max_values == 7U);
   const auto retained = native;
   for (const auto& field : fields) {
      auto invalid = first;
      invalid.records.*(field.configured) = 0;
      BOOST_TEST_CONTEXT(field.name) {
         BOOST_CHECK_THROW(np::apply_dht_profiles(native, {second, invalid}), np::exceptions::invalid_config);
         BOOST_REQUIRE_EQUAL(native.dht_records.size(), retained.dht_records.size());
         BOOST_REQUIRE_EQUAL(native.dht_profiles.size(), retained.dht_profiles.size());
         check_records(record_options(native), record_options(retained));
         check_records(record_options(native, second.protocol), record_options(retained, second.protocol));
      }
   }
   np::apply_dht_profiles(native, {custom()});
   BOOST_REQUIRE_EQUAL(native.dht_records.size(), 1U);
   check_records(record_options(native), p2p::dht::record_store::options_for(native.dht_profiles.front()));
   np::apply_dht_profiles(native, {});
   BOOST_TEST(native.dht_profiles.empty());
   BOOST_TEST(native.dht_records.empty());
}

BOOST_AUTO_TEST_CASE(p2p_dht_config_numeric_types_checked_counts_and_objectdb_page_preflight) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
   for (const auto& field : fields) {
      for (const auto& invalid : std::array{value{std::uint64_t{0}}, value{std::int64_t{-1}}, value{1.5}}) {
         BOOST_TEST_CONTEXT(field.name) {
            BOOST_CHECK_THROW(configure_fresh(runtime, document_with({{field.name, invalid}})),
                              np::exceptions::invalid_config);
         }
      }
      auto configured = custom();
      configured.records.*(field.configured) = maximum;
      if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
         BOOST_CHECK_THROW(mapped({configured}), np::exceptions::invalid_config);
      } else {
         // Mapping only, not an allocation/hydration claim at these large sizes.
         const auto native = mapped({configured});
         BOOST_TEST((record_options(native).*(field.native)) == maximum);
      }
   }
   for (const auto* field : {"hydration-page-limit", "prune-page-limit"}) {
      const auto boundary = static_cast<std::uint64_t>(forge::db::core::max_page_limit);
      BOOST_CHECK_NO_THROW(configure_fresh(runtime, document_with({{field, value{boundary}}})));
      auto oversized = document_with({{field, value{boundary + 1}}});
      BOOST_CHECK_THROW(configure_fresh(runtime, oversized), np::exceptions::invalid_config);
      // Memory-native persistence does not acquire an ObjectDB page contract.
      oversized.set(std::string{section} + ".peer-store.store", "");
      oversized.set(std::string{section} + ".allow-insecure-test-mode", true);
      BOOST_CHECK_NO_THROW(configure_fresh(runtime, oversized));
   }
}

BOOST_AUTO_TEST_CASE(p2p_dht_config_real_yaml_strict_records_and_no_duplicate_protocol_knobs) {
   const auto parsed = forge::codec::yaml::read_document(
       "dht:\n  profiles:\n    - kind: custom\n      protocol: /configuration/records/kad/1\n"
       "      mode: server\n      records: {max-values: 7, max-total-bytes: 32768, hydration-page-limit: 17, "
       "prune-page-limit: 19, max-hydration-pages: 23, max-providers: 17, max-local-providers: 7}\n");
   BOOST_REQUIRE(parsed.ok());
   auto document = document_with();
   const auto* profiles = parsed.value.try_get("dht.profiles");
   BOOST_REQUIRE(profiles);
   document.set(std::string{section} + ".dht.profiles", *profiles);
   const auto native = mapped(decode(document).dht_profiles);
   for (const auto& field : fields) { BOOST_TEST((record_options(native).*(field.native)) == field.sample); }
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   for (const auto* key : {"max-record-bytes", "max-record-size", "max-providers-per-key", "max-persistence-waiters",
                           "max-value", "inline-persistence", "public-keys"}) {
      BOOST_TEST_CONTEXT(key) {
         const auto invalid = document_with({{key, value{std::uint64_t{7}}}});
         BOOST_CHECK_THROW(configure_fresh(runtime, invalid),
                           np::exceptions::invalid_config);
         check_repeated_config(runtime, plugin, invalid);
      }
   }
   auto malformed = document_with();
   malformed.set(std::string{section} + ".dht.profiles", value::array_type{
       value::object_type{{"kind", "custom"}, {"protocol", std::string{protocol}}, {"records", value::array_type{}}}});
   BOOST_CHECK_THROW(configure_fresh(runtime, malformed), np::exceptions::invalid_config);
   check_repeated_config(runtime, plugin, malformed);
}

BOOST_AUTO_TEST_CASE(p2p_dht_config_record_configuration_is_frozen_after_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto accepted = document_with({{"max-values", value{std::uint64_t{3}}}});
   configure(runtime, plugin, accepted);
   plugin.request_stop();
   for (const auto& field : fields) {
      auto candidate = document_with({{field.name, value{field.sample}}});
      candidate.set(std::string{section} + ".diagnostics.max-peers", std::uint64_t{2});
      BOOST_TEST_CONTEXT(field.name) {
         check_repeated_config(runtime, plugin, candidate);
      }
   }
   BOOST_CHECK_THROW(run(runtime, plugin.startup()), forge::app::exceptions::invalid_state);
   run(runtime, plugin.shutdown());
   check_repeated_config(runtime, plugin, accepted);
}
