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
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.asio.runtime;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.document;
import forge.config.core.value;
import forge.multiformats.varint;
import forge.net.p2p.node;
import forge.net.p2p.endpoint;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;

#include "../../plugins/net/p2p/node/details/configuration_policy.hxx"
#include "../../plugins/net/p2p/node/details/pubsub_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace pubsub = forge::net::p2p::pubsub;
using wire = np::pubsub_config::wire_config;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::string_view section = "plugins.net.p2p.node";

struct wire_field {
   const char* name;
   std::optional<std::uint64_t> wire::* configured;
   std::size_t pubsub::limits::* native;
   std::uint64_t sample;
};
constexpr auto fields = std::array{
   wire_field{"max-rpc-size", &wire::max_rpc_size, &pubsub::limits::max_rpc_size, 2 * 1024 * 1024},
   wire_field{"max-outbound-queue-bytes", &wire::max_outbound_queue_bytes, &pubsub::limits::max_outbound_queue_bytes, 32768},
   wire_field{"max-remote-topics-per-peer", &wire::max_remote_topics_per_peer, &pubsub::limits::max_remote_topics_per_peer, 17},
   wire_field{"max-remote-topic-entries", &wire::max_remote_topic_entries, &pubsub::limits::max_remote_topic_entries, 37},
   wire_field{"max-iwant-promises", &wire::max_iwant_promises, &pubsub::limits::max_iwant_promises, 41},
};

struct group_field {
   const char* path;
   std::optional<std::uint64_t>& (*configured)(np::pubsub_config&);
   std::size_t pubsub::limits::* native;
   std::uint64_t sample;
};
constexpr auto group_fields = std::array{
   group_field{"validation.max-attempts", [](np::pubsub_config& c) -> auto& { return c.validation.max_attempts; },
               &pubsub::limits::max_validation_attempts, 7},
   group_field{"validation.max-redeliveries", [](np::pubsub_config& c) -> auto& { return c.validation.max_redeliveries; },
               &pubsub::limits::max_validation_redeliveries, 11},
   group_field{"validation.max-requests", [](np::pubsub_config& c) -> auto& { return c.validation.max_requests; },
               &pubsub::limits::max_validation_requests, 13},
   group_field{"idontwant.threshold", [](np::pubsub_config& c) -> auto& { return c.idontwant.threshold; },
               &pubsub::limits::idontwant_threshold, 512},
   group_field{"idontwant.ttl-heartbeats", [](np::pubsub_config& c) -> auto& { return c.idontwant.ttl_heartbeats; },
               &pubsub::limits::idontwant_ttl, 7},
   group_field{"idontwant.max-entries-per-peer", [](np::pubsub_config& c) -> auto& { return c.idontwant.max_entries_per_peer; },
               &pubsub::limits::max_idontwant_entries_per_peer, 17},
   group_field{"idontwant.max-bytes-per-peer", [](np::pubsub_config& c) -> auto& { return c.idontwant.max_bytes_per_peer; },
               &pubsub::limits::max_idontwant_bytes_per_peer, 8192},
   group_field{"idontwant.max-entries", [](np::pubsub_config& c) -> auto& { return c.idontwant.max_entries; },
               &pubsub::limits::max_idontwant_entries, 37},
   group_field{"idontwant.max-bytes", [](np::pubsub_config& c) -> auto& { return c.idontwant.max_bytes; },
               &pubsub::limits::max_idontwant_bytes, 32768},
   group_field{"partial.max-groups", [](np::pubsub_config& c) -> auto& { return c.partial.max_groups; },
               &pubsub::limits::max_partial_groups, 37},
   group_field{"partial.max-groups-per-topic", [](np::pubsub_config& c) -> auto& { return c.partial.max_groups_per_topic; },
               &pubsub::limits::max_partial_groups_per_topic, 17},
   group_field{"partial.max-group-bytes", [](np::pubsub_config& c) -> auto& { return c.partial.max_group_bytes; },
               &pubsub::limits::max_partial_group_bytes, 32768},
   group_field{"partial.max-metadata-size", [](np::pubsub_config& c) -> auto& { return c.partial.max_metadata_size; },
               &pubsub::limits::max_partial_metadata_size, 8192},
   group_field{"partial.ttl-heartbeats", [](np::pubsub_config& c) -> auto& { return c.partial.ttl_heartbeats; },
               &pubsub::limits::partial_group_ttl, 7},
   group_field{"partial.max-callbacks", [](np::pubsub_config& c) -> auto& { return c.partial.max_callbacks; },
               &pubsub::limits::max_partial_callbacks, 3},
   group_field{"partial.max-callback-bytes", [](np::pubsub_config& c) -> auto& { return c.partial.max_callback_bytes; },
               &pubsub::limits::max_partial_callback_bytes, 32768},
};

struct retry_field {
   const char* path;
   std::optional<std::uint64_t> np::pubsub_config::validation_config::* configured;
   std::chrono::milliseconds pubsub::limits::* native;
   std::uint64_t sample;
};
constexpr auto retry_fields = std::array{
   retry_field{"validation.retry-initial-delay-ms", &np::pubsub_config::validation_config::retry_initial_delay_ms,
               &pubsub::limits::validation_retry_initial_delay, 750},
   retry_field{"validation.retry-max-delay-ms", &np::pubsub_config::validation_config::retry_max_delay_ms,
               &pubsub::limits::validation_retry_max_delay, 7000},
};

pubsub::options mapped(const np::pubsub_config& settings = {}) {
   np::detail::validate_pubsub_config(settings);
   return np::detail::pubsub_options_for(settings);
}

forge::config::core::document document_with(value::object_type settings = {}) {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".peer-store.store", "unbound-store");
   document.set(std::string{section} + ".identity.certificate-secret", "unbound/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "unbound/key");
   document.set(std::string{section} + ".pubsub", std::move(settings));
   return document;
}

np::pubsub_config decoded(const forge::config::core::document& document) {
   return np::decode_config({document, std::string{section}}).pubsub;
}

template<class T> T run(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto future = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (future.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   return future.get();
}

void configure(forge::asio::runtime& runtime, np::plugin& plugin,
               const forge::config::core::document& document) {
   run(runtime, plugin.configure({document, std::string{section}}));
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_native_defaults_and_checked_rpc_envelope) {
   const auto actual = mapped();
   const auto defaults = pubsub::options{};
   BOOST_CHECK(actual.preferred == defaults.preferred);
   BOOST_CHECK(actual.signatures == defaults.signatures);
   BOOST_TEST(actual.allow_v1_0_fallback == defaults.allow_v1_0_fallback);
   BOOST_TEST(actual.flood_publish == defaults.flood_publish);
   BOOST_TEST(actual.peer_exchange == defaults.peer_exchange);
   BOOST_TEST(!actual.partial_messages);
   BOOST_CHECK(!actual.scoring);
   BOOST_TEST(actual.limits.max_data_size == defaults.limits.max_data_size);
   BOOST_TEST(actual.limits.max_message_size == actual.limits.max_data_size + 1024);
   const auto minimum = actual.limits.max_message_size + 1 +
       forge::multiformats::varint_encoded_size(actual.limits.max_message_size);
   BOOST_TEST(actual.limits.max_rpc_size == std::max(defaults.limits.max_rpc_size, minimum));
   for (const auto& field : fields) {
      if (field.configured != &wire::max_rpc_size) {
         BOOST_TEST((actual.limits.*(field.native)) == (defaults.limits.*(field.native)));
      }
   }
   // Codec framing evidence only; real signed delivery belongs to the configured-node fixture.
   auto message = pubsub::message{.data = std::vector<std::uint8_t>(actual.limits.max_data_size, 0x71),
                                  .subject = {"payload-bound"}};
   BOOST_CHECK_NO_THROW((void)pubsub::codec::encode(pubsub::rpc{.messages = {message}}, actual));
   auto settings = np::pubsub_config{};
   settings.wire.max_rpc_size = minimum;
   BOOST_TEST(mapped(settings).limits.max_rpc_size == minimum);
   settings.wire.max_rpc_size = minimum - 1;
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   settings = {};
   settings.max_message_size = (std::numeric_limits<std::uint64_t>::max)();
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   settings.max_message_size = (std::numeric_limits<std::size_t>::max)() - 1024;
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config); // RPC framing, not message addition.
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_modes_partial_inference_and_invalid_enums) {
   for (const auto& [configured, expected] : std::array{
            std::pair{np::pubsub_version::v1_0, pubsub::version::v1_0},
            std::pair{np::pubsub_version::v1_1, pubsub::version::v1_1},
            std::pair{np::pubsub_version::v1_2, pubsub::version::v1_2},
            std::pair{np::pubsub_version::v1_3, pubsub::version::v1_3}}) {
      auto settings = np::pubsub_config{};
      settings.preferred_version = configured;
      BOOST_CHECK(mapped(settings).preferred == expected);
      settings.partial_messages = true;
      if (expected == pubsub::version::v1_3) { BOOST_CHECK_NO_THROW(mapped(settings)); }
      else { BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config); }
   }
   auto settings = np::pubsub_config{};
   settings.partial_messages = true;
   BOOST_CHECK(mapped(settings).preferred == pubsub::version::v1_3);
   settings.partial_messages = false;
   settings.allow_v1_0_fallback = false;
   settings.flood_publish = true;
   settings.peer_exchange = true;
   const auto options = mapped(settings);
   BOOST_TEST(!options.allow_v1_0_fallback);
   BOOST_TEST(options.flood_publish);
   BOOST_TEST(options.peer_exchange);
   settings.preferred_version = static_cast<np::pubsub_version>(99);
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_receive_policy_keeps_publish_default_and_rejects_strict_conflicts) {
   for (const auto& [configured, expected] : std::array{
            std::pair{np::pubsub_signature_policy::strict_sign, pubsub::signature_policy::strict_sign},
            std::pair{np::pubsub_signature_policy::strict_no_sign, pubsub::signature_policy::strict_no_sign},
            std::pair{np::pubsub_signature_policy::lax_sign, pubsub::signature_policy::lax_sign},
            std::pair{np::pubsub_signature_policy::lax_no_sign, pubsub::signature_policy::lax_no_sign}}) {
      for (const auto sign : {false, true}) {
         auto settings = np::pubsub_config{};
         settings.signature_policy = configured;
         settings.sign_publishes = sign;
         if ((expected == pubsub::signature_policy::strict_sign && !sign) ||
             (expected == pubsub::signature_policy::strict_no_sign && sign)) {
            BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
         } else { BOOST_CHECK(mapped(settings).signatures == expected); }
         BOOST_TEST(settings.sign_publishes == sign);
      }
   }
   auto settings = np::pubsub_config{};
   settings.sign_publishes = false;
   BOOST_CHECK(mapped(settings).signatures == pubsub::signature_policy::lax_no_sign);
   settings.signature_policy = static_cast<np::pubsub_signature_policy>(99);
   BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_each_wire_field_sparse_removal_zero_and_narrowing) {
   const auto defaults = mapped();
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.name) {
         auto settings = np::pubsub_config{};
         settings.wire.*(field.configured) = field.sample;
         BOOST_TEST((mapped(settings).limits.*(field.native)) == field.sample);
         settings.wire = {};
         BOOST_TEST((mapped(settings).limits.*(field.native)) == (defaults.limits.*(field.native)));
         settings.wire.*(field.configured) = 0;
         BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
         settings.wire.*(field.configured) = (std::numeric_limits<std::uint64_t>::max)();
         if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
         } else {
            // Mapping/validation only: no allocation at UINT64_MAX and no 32-bit runtime claim.
            BOOST_TEST((mapped(settings).limits.*(field.native)) == (std::numeric_limits<std::uint64_t>::max)());
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_real_yaml_strict_mode_wire_fields_and_types) {
   const auto parsed = forge::codec::yaml::read_document(
       "pubsub:\n  enabled: true\n  preferred-version: v1-2\n  allow-v1-0-fallback: false\n"
       "  signature-policy: strict-no-sign\n  sign-publishes: false\n  flood-publish: true\n"
       "  peer-exchange: true\n  wire: {max-rpc-size: 2097152, max-outbound-queue-bytes: 32768, "
       "max-remote-topics-per-peer: 17, max-remote-topic-entries: 37, max-iwant-promises: 41}\n");
   BOOST_REQUIRE(parsed.ok());
   const auto* configured = parsed.value.try_get("pubsub");
   BOOST_REQUIRE(configured);
   auto document = document_with();
   document.set(std::string{section} + ".pubsub", *configured);
   const auto options = mapped(decoded(document));
   BOOST_CHECK(options.preferred == pubsub::version::v1_2);
   BOOST_CHECK(options.signatures == pubsub::signature_policy::strict_no_sign);
   for (const auto& field : fields) { BOOST_TEST((options.limits.*(field.native)) == field.sample); }
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   for (const auto* key : {"wir", "wire.max-rpc", "wire.max-data-size", "wire.max-topics",
                           "preferred", "signature-policy-wrong", "validation.max-attempt"}) {
      auto invalid = document;
      invalid.set(std::string{section} + ".pubsub." + key, std::uint64_t{7});
      BOOST_TEST_CONTEXT(key) { BOOST_CHECK_THROW(configure(runtime, plugin, invalid), np::exceptions::invalid_config); }
   }
   for (const auto& field : fields) {
      for (const auto& invalid_value : {value{std::int64_t{-1}}, value{1.5}, value{true}}) {
         auto invalid = document;
         invalid.set(std::string{section} + ".pubsub.wire." + field.name, invalid_value);
         BOOST_CHECK_THROW(configure(runtime, plugin, invalid), np::exceptions::invalid_config);
      }
   }
   document.set(std::string{section} + ".pubsub.preferred-version", "v1-9");
   BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_first_group_configuration_is_frozen_after_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto accepted = document_with({{"enabled", true}});
   configure(runtime, plugin, accepted);
   plugin.request_stop();
   for (const auto& [key, setting] : std::array{
            std::pair{"preferred-version", value{"v1-2"}}, std::pair{"signature-policy", value{"lax-sign"}},
            std::pair{"allow-v1-0-fallback", value{false}}, std::pair{"flood-publish", value{true}},
            std::pair{"peer-exchange", value{true}}, std::pair{"wire.max-outbound-queue-bytes", value{std::uint64_t{32768}}}}) {
      auto candidate = accepted;
      candidate.set(std::string{section} + ".pubsub." + key, setting);
      candidate.set(std::string{section} + ".diagnostics.max-peers", std::uint64_t{2});
      BOOST_TEST_CONTEXT(key) { BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::route_conflict); }
   }
   run(runtime, plugin.shutdown());
   BOOST_CHECK_THROW(configure(runtime, plugin, accepted), np::exceptions::route_conflict);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_validation_extensions_defaults_mapping_and_removal) {
   const auto defaults = pubsub::limits{};
   for (const auto& field : group_fields) {
      BOOST_TEST_CONTEXT(field.path) {
         auto settings = np::pubsub_config{};
         BOOST_TEST((mapped(settings).limits.*(field.native)) == (defaults.*(field.native)));
         field.configured(settings) = field.sample;
         BOOST_TEST((mapped(settings).limits.*(field.native)) == field.sample);
         field.configured(settings).reset();
         BOOST_TEST((mapped(settings).limits.*(field.native)) == (defaults.*(field.native)));
      }
   }
   for (const auto& field : retry_fields) {
      auto settings = np::pubsub_config{};
      settings.validation.*(field.configured) = field.sample;
      BOOST_TEST((mapped(settings).limits.*(field.native)).count() == field.sample);
      settings.validation = {};
      BOOST_CHECK((mapped(settings).limits.*(field.native)) == (defaults.*(field.native)));
   }
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_validation_extensions_native_bounds_types_and_overflow) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   for (const auto& field : group_fields) {
      BOOST_TEST_CONTEXT(field.path) {
         auto document = document_with();
         document.set(std::string{section} + ".pubsub." + field.path, field.sample);
         BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
         document.set(std::string{section} + ".pubsub." + field.path, std::uint64_t{0});
         if (std::string_view{field.path} == "idontwant.threshold") {
            BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
         } else { BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config); }
         for (const auto& invalid : {value{std::int64_t{-1}}, value{1.5}, value{true}}) {
            document.set(std::string{section} + ".pubsub." + field.path, invalid);
            BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
         }
         auto settings = np::pubsub_config{};
         field.configured(settings) = (std::numeric_limits<std::uint64_t>::max)();
         if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
         } else {
            BOOST_TEST((mapped(settings).limits.*(field.native)) == (std::numeric_limits<std::uint64_t>::max)());
         }
      }
   }
   for (const auto& field : retry_fields) {
      auto document = document_with();
      document.set(std::string{section} + ".pubsub." + field.path, std::uint64_t{0});
      BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
      auto settings = np::pubsub_config{};
      settings.validation.*(field.configured) = (std::numeric_limits<std::uint64_t>::max)();
      BOOST_CHECK_THROW(mapped(settings), np::exceptions::invalid_config);
   }
   const auto inconsistent = document_with({{"validation", value::object_type{
       {"retry-initial-delay-ms", std::uint64_t{750}}, {"retry-max-delay-ms", std::uint64_t{749}}}}});
   BOOST_CHECK_THROW(configure(runtime, plugin, inconsistent), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_retry_clock_bounds_reject_during_resource_free_preflight) {
   const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(
       std::chrono::steady_clock::duration::max());
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   // Identity/store references remain unbound: configure must validate before resource acquisition.
   auto accepted = document_with({{"enabled", true}});
   accepted.set(std::string{section} + ".pubsub.validation.retry-initial-delay-ms", std::uint64_t{1});
   accepted.set(std::string{section} + ".pubsub.validation.retry-max-delay-ms",
                static_cast<std::uint64_t>(maximum.count()));
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, accepted));
   for (const auto& field : retry_fields) {
      auto boundary = accepted;
      boundary.set(std::string{section} + ".pubsub." + field.path,
                   static_cast<std::uint64_t>(maximum.count()));
      BOOST_CHECK_NO_THROW(configure(runtime, plugin, boundary));
      BOOST_TEST((mapped(decoded(boundary)).limits.*(field.native)).count() == maximum.count());
      if (maximum < std::chrono::milliseconds::max()) {
         for (const auto invalid : {maximum + 1ms, std::chrono::milliseconds::max()}) {
            auto candidate = boundary;
            candidate.set(std::string{section} + ".pubsub." + field.path,
                          static_cast<std::uint64_t>(invalid.count()));
            candidate.set(std::string{section} + ".diagnostics.max-peers", std::uint64_t{2});
            BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
         }
      }
      BOOST_CHECK_NO_THROW(configure(runtime, plugin, accepted));
   }
   run(runtime, plugin.shutdown());
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_validation_extensions_real_yaml_and_no_algorithm_knobs) {
   const auto parsed = forge::codec::yaml::read_document(
       "pubsub:\n  validation: {max-attempts: 7, max-redeliveries: 11, max-requests: 13, "
       "retry-initial-delay-ms: 750, retry-max-delay-ms: 7000}\n"
       "  idontwant: {threshold: 512, ttl-heartbeats: 7, max-entries-per-peer: 17, max-bytes-per-peer: 8192, "
       "max-entries: 37, max-bytes: 32768}\n"
       "  partial: {max-groups: 37, max-groups-per-topic: 17, max-group-bytes: 32768, max-metadata-size: 8192, "
       "ttl-heartbeats: 7, max-callbacks: 3, max-callback-bytes: 32768}\n");
   BOOST_REQUIRE(parsed.ok());
   const auto* configured = parsed.value.try_get("pubsub");
   BOOST_REQUIRE(configured);
   auto document = document_with();
   document.set(std::string{section} + ".pubsub", *configured);
   const auto options = mapped(decoded(document));
   for (const auto& field : group_fields) { BOOST_TEST((options.limits.*(field.native)) == field.sample); }
   for (const auto& field : retry_fields) { BOOST_TEST((options.limits.*(field.native)).count() == field.sample); }
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
   for (const auto* key : {"validation.max-validation-queue", "idontwant.ttl-seconds", "partial.ttl-seconds",
                           "partial.max-group-id-size", "partial.max-gossip-peers", "mesh-n", "history-length",
                           "heartbeat-interval-ms", "gossip-factor"}) {
      auto invalid = document;
      invalid.set(std::string{section} + ".pubsub." + key, std::uint64_t{7});
      BOOST_TEST_CONTEXT(key) { BOOST_CHECK_THROW(configure(runtime, plugin, invalid), np::exceptions::invalid_config); }
   }
}

BOOST_AUTO_TEST_CASE(p2p_pubsub_config_validation_extensions_frozen_after_stop) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto accepted = document_with({{"enabled", true}});
   configure(runtime, plugin, accepted);
   plugin.request_stop();
   for (const auto& field : group_fields) {
      auto candidate = accepted;
      candidate.set(std::string{section} + ".pubsub." + field.path, field.sample);
      candidate.set(std::string{section} + ".diagnostics.max-peers", std::uint64_t{2});
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::route_conflict);
   }
   for (const auto& field : retry_fields) {
      auto candidate = accepted;
      candidate.set(std::string{section} + ".pubsub." + field.path, field.sample);
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::route_conflict);
   }
   run(runtime, plugin.shutdown());
}
