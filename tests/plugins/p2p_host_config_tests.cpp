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
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

module forge.plugins.net.p2p.node.plugin;

import forge.asio.runtime;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;

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
   bool zero_valid = false;
};

constexpr auto fields = std::array{
   numeric_field{"connections.session-low-watermark",
       [](const p2p::node::options& v) -> std::uint64_t { return v.limits.session_low_watermark; }},
   numeric_field{"connections.session-grace-period-ms",
       [](const p2p::node::options& v) -> std::uint64_t { return v.limits.session_grace_period.count(); }, true},
   numeric_field{"connections.session-prune-silence-ms",
       [](const p2p::node::options& v) -> std::uint64_t { return v.limits.session_prune_silence.count(); }},
   numeric_field{"connections.dial-backoff-base-ms",
       [](const p2p::node::options& v) -> std::uint64_t { return v.limits.dial_backoff_base.count(); }},
   numeric_field{"connections.dial-backoff-step-ms",
       [](const p2p::node::options& v) -> std::uint64_t { return v.limits.dial_backoff_step.count(); }},
   numeric_field{"connections.dial-backoff-max-ms",
       [](const p2p::node::options& v) -> std::uint64_t { return v.limits.dial_backoff_max.count(); }},
   numeric_field{"identify.timeout-ms",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.timeout.count(); }},
   numeric_field{"identify.max-message-size",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_message_size; }},
   numeric_field{"identify.max-total-message-size",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_total_message_size; }},
   numeric_field{"identify.max-own-message-size",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_own_message_size; }},
   numeric_field{"identify.max-message-parts",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_message_parts; }},
   numeric_field{"identify.max-protocols",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_protocols; }},
   numeric_field{"identify.max-listen-endpoints",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_listen_endpoints; }},
   numeric_field{"identify.max-protocol-size",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_protocol_size; }},
   numeric_field{"identify.max-version-size",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_version_size; }},
   numeric_field{"identify.max-public-key-size",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_public_key_size; }},
   numeric_field{"identify.max-signed-peer-record-size",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_signed_peer_record_size; }},
   numeric_field{"identify.max-push-operations",
       [](const p2p::node::options& v) -> std::uint64_t { return v.identify.max_push_operations; }},
   numeric_field{"peer-state.max-peers",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_peers; }},
   numeric_field{"peer-state.max-rendezvous",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_rendezvous; }},
   numeric_field{"peer-state.max-pending",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_pending; }},
   numeric_field{"peer-state.max-endpoints-per-peer",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_endpoints_per_peer; }},
   numeric_field{"peer-state.max-protocols-per-peer",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_protocols_per_peer; }},
   numeric_field{"peer-state.max-relay-reservations-per-peer",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_relay_reservations_per_peer; }},
   numeric_field{"peer-state.max-relay-endpoints-per-reservation",
       [](const p2p::node::options& v) -> std::uint64_t {
          return v.peer_state.max_relay_endpoints_per_reservation;
       }},
   numeric_field{"peer-state.max-peer-record-bytes",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_peer_record_bytes; }},
   numeric_field{"peer-state.hydration-page-limit",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.hydration_page_limit; }},
   numeric_field{"peer-state.prune-page-limit",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.prune_page_limit; }},
   numeric_field{"peer-state.max-persistence-waiters",
       [](const p2p::node::options& v) -> std::uint64_t { return v.peer_state.max_persistence_waiters; }},
};

p2p::node::options mapped(const np::config& config, p2p::node::options native = {}) {
   np::apply_connections(native.limits, config.connections);
   np::apply_identify(native, config.identify);
   np::apply_peer_state(native.peer_state, config.peer_state);
   return native;
}

void check_fields(const p2p::node::options& actual, const p2p::node::options& expected) {
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.path) { BOOST_TEST(field.read(actual) == field.read(expected)); }
   }
}

forge::config::core::document valid_document() {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".allow-insecure-test-mode", true);
   document.set(std::string{section} + ".identity.certificate-secret", "host/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "host/key");
   document.set(std::string{section} + ".topology.mode", "static-only");
   return document;
}

void configure(forge::asio::runtime& runtime, np::plugin& plugin,
               const forge::config::core::document& document) {
   auto result = boost::asio::co_spawn(runtime.context(),
       plugin.configure({document, std::string{section}}), boost::asio::use_future);
   if (result.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   result.get();
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_host_config_all_fields_decode_and_map_without_overwriting_other_defaults) {
   const auto defaults = p2p::node::options{};
   check_fields(mapped({}), defaults);
   for (const auto& field : fields) {
      for (const auto number : {std::uint64_t{0}, std::uint64_t{123}}) {
         BOOST_TEST_CONTEXT(field.path << "=" << number) {
            auto document = forge::config::core::document{};
            document.set(field.path, number);
            const auto decoded = forge::config::core::decode<np::config>(document);
            BOOST_REQUIRE(decoded.ok());
            const auto actual = mapped(decoded.value);
            for (const auto& other : fields) {
               BOOST_TEST_CONTEXT(other.path) {
                  BOOST_TEST(other.read(actual) == (&other == &field ? number : other.read(defaults)));
               }
            }
            BOOST_TEST(actual.protocol_version == defaults.protocol_version);
            BOOST_TEST(actual.agent_version == defaults.agent_version);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_host_config_sparse_overrides_keep_derived_watermark_versions_and_persistence_owner) {
   auto native = p2p::node::options{};
   native.limits.max_sessions = 9;
   native.limits.session_low_watermark = 9;
   native.protocol_version = "/prior/1";
   native.agent_version = "prior/1";
   native.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   const auto unchanged = mapped({}, native);
   check_fields(unchanged, native);
   BOOST_CHECK(unchanged.peer_state.persistence == native.peer_state.persistence);
   BOOST_TEST(unchanged.protocol_version == "/prior/1");
   BOOST_TEST(unchanged.agent_version == "prior/1");
   auto config = np::config{};
   config.connections.session_low_watermark = 3;
   config.identify.protocol_version = "/host/2";
   config.identify.agent_version = "host/2";
   config.peer_state.max_peers = 7;
   const auto actual = mapped(config, native);
   BOOST_TEST(actual.limits.max_sessions == 9U);
   BOOST_TEST(actual.limits.session_low_watermark == 3U);
   BOOST_TEST(actual.protocol_version == "/host/2");
   BOOST_TEST(actual.agent_version == "host/2");
   BOOST_TEST(actual.peer_state.max_peers == 7U);
   BOOST_CHECK(actual.peer_state.persistence == native.peer_state.persistence);
   config.identify.protocol_version = "";
   config.identify.agent_version = "";
   const auto empty = mapped(config, native);
   BOOST_TEST(empty.protocol_version.empty());
   BOOST_TEST(empty.agent_version.empty());
}

BOOST_AUTO_TEST_CASE(p2p_host_config_all_fields_reject_bad_numbers_and_native_zero_before_commit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto valid = valid_document();
   configure(runtime, plugin, valid);
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.path) {
         const auto path = std::string{section} + "." + field.path;
         for (const auto& invalid : {value{std::int64_t{-1}}, value{1.5}, value{"18446744073709551616"}}) {
            auto document = valid;
            document.set(path, invalid);
            BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
         }
         auto document = valid;
         document.set(path, std::uint64_t{0});
         if (field.zero_valid) {
            BOOST_CHECK_NO_THROW(configure(runtime, plugin, document));
         } else {
            BOOST_CHECK_THROW(configure(runtime, plugin, document), np::exceptions::invalid_config);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_host_config_checked_duration_and_native_count_conversion_boundaries) {
   const auto maximum_ms = (std::numeric_limits<std::chrono::milliseconds::rep>::max)();
   BOOST_TEST(np::to_ms(static_cast<std::uint64_t>(maximum_ms)).count() == maximum_ms);
   BOOST_CHECK_THROW(static_cast<void>(np::to_ms((std::numeric_limits<std::uint64_t>::max)())),
                     np::exceptions::invalid_config);
   const auto largest_size = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
   auto config = np::config{};
   config.peer_state.max_peers = largest_size;
   BOOST_TEST(mapped(config).peer_state.max_peers == (std::numeric_limits<std::size_t>::max)());
   // Only narrower size_t targets execute this overflow branch.
   if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
      for (const auto& field : fields) {
         if (std::string_view{field.path}.ends_with("-ms")) { continue; }
         auto document = forge::config::core::document{};
         document.set(field.path, (std::numeric_limits<std::uint64_t>::max)());
         const auto decoded = forge::config::core::decode<np::config>(document);
         BOOST_REQUIRE(decoded.ok());
         BOOST_CHECK_THROW(mapped(decoded.value), np::exceptions::invalid_config);
      }
   }
}
