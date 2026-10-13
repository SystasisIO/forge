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

module forge.plugins.net.p2p.node.plugin;

import forge.asio.runtime;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.endpoint;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.scoring;
import forge.net.transport.limits;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;
import forge.schema.object;

#include "../../plugins/net/p2p/node/details/configuration_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using value = forge::config::core::value;
using transport_limits = forge::net::transport::limits;
using namespace std::chrono_literals;
constexpr std::string_view section = "plugins.net.p2p.node";

struct limit_field {
   const char* name;
   std::optional<std::uint64_t> np::quic_config::* input;
   std::size_t transport_limits::* output;
};

constexpr auto fields = std::array{
   limit_field{"max-connections", &np::quic_config::max_connections, &transport_limits::max_connections},
   limit_field{"max-streams-per-connection", &np::quic_config::max_streams_per_connection,
               &transport_limits::max_streams_per_connection},
   limit_field{"max-queued-bytes", &np::quic_config::max_queued_bytes, &transport_limits::max_queued_bytes},
   limit_field{"max-inbound-queued-bytes", &np::quic_config::max_inbound_queued_bytes,
               &transport_limits::max_inbound_queued_bytes},
   limit_field{"max-inbound-queued-packets", &np::quic_config::max_inbound_queued_packets,
               &transport_limits::max_inbound_queued_packets},
};

struct security_choice {
   const char* name;
   np::tcp_security input;
   p2p::node::stream_security output;
};

constexpr auto security_choices = std::array{
   security_choice{"tls-and-noise", np::tcp_security::tls_and_noise, p2p::node::stream_security::tls_and_noise},
   security_choice{"tls", np::tcp_security::tls, p2p::node::stream_security::tls},
   security_choice{"noise", np::tcp_security::noise, p2p::node::stream_security::noise},
};

p2p::node::options mapped(const np::config& config, p2p::node::options native = {}) {
   np::apply_tcp(native, config.tcp);
   np::apply_quic(native, config.quic);
   return native;
}

void check_limits(const transport_limits& actual, const transport_limits& expected) {
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.name) { BOOST_TEST((actual.*field.output) == (expected.*field.output)); }
   }
   BOOST_TEST(actual.max_frame_size == expected.max_frame_size);
}

forge::config::core::document valid_document() {
   auto document = forge::config::core::document{};
   document.set(std::string{section} + ".allow-insecure-test-mode", true);
   document.set(std::string{section} + ".identity.certificate-secret", "transport/cert");
   document.set(std::string{section} + ".identity.private-key-secret", "transport/key");
   document.set(std::string{section} + ".topology.mode", "static-only");
   return document;
}

template<class T> T run(forge::asio::runtime& runtime, boost::asio::awaitable<T> operation) {
   auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
   if (result.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   return result.get();
}

void configure(forge::asio::runtime& runtime, np::plugin& plugin,
               const forge::config::core::document& document) {
   run(runtime, plugin.configure({document, std::string{section}}));
}

void configure_fresh(forge::asio::runtime& runtime, const forge::config::core::document& document) {
   auto plugin = np::plugin{};
   auto failure = std::exception_ptr{};
   try { configure(runtime, plugin, document); }
   catch (...) { failure = std::current_exception(); }
   // Failed input closes admission too. Each validation probe owns and joins
   // its real cleanup before the next independent candidate is exercised.
   run(runtime, plugin.shutdown());
   if (failure) { std::rethrow_exception(failure); }
}

forge::config::core::document yaml_candidate(std::string_view text) {
   const auto yaml = forge::codec::yaml::read_document(text);
   BOOST_REQUIRE(yaml.ok());
   const auto* changes = yaml.value.object_at("");
   BOOST_REQUIRE(changes);
   auto document = valid_document();
   for (const auto& [name, setting] : *changes) {
      document.set(std::string{section} + "." + name, setting);
   }
   return document;
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_transport_config_all_quic_fields_map_exactly_and_preserve_other_defaults) {
   const auto defaults = p2p::node::options{};
   const auto empty = forge::config::core::decode<np::config>({});
   BOOST_REQUIRE(empty.ok());
   BOOST_TEST(!empty.value.tcp.security);
   for (const auto& field : fields) { BOOST_TEST(!(empty.value.quic.*field.input)); }
   check_limits(mapped(empty.value).transport_limits, defaults.transport_limits);
   BOOST_CHECK(mapped(empty.value).stream_security == defaults.stream_security);
   for (const auto& field : fields) {
      for (const auto number : {std::uint64_t{0}, std::uint64_t{123}}) {
         BOOST_TEST_CONTEXT(field.name << "=" << number) {
            auto document = forge::config::core::document{};
            document.set(std::string{"quic."} + field.name, number);
            const auto decoded = forge::config::core::decode<np::config>(document);
            BOOST_REQUIRE(decoded.ok());
            BOOST_REQUIRE((decoded.value.quic.*field.input).has_value());
            BOOST_TEST(*(decoded.value.quic.*field.input) == number);
            const auto actual = mapped(decoded.value);
            auto expected = defaults.transport_limits;
            expected.*field.output = static_cast<std::size_t>(number);
            check_limits(actual.transport_limits, expected);
            BOOST_CHECK(actual.stream_security == defaults.stream_security);
            BOOST_TEST(actual.limits.max_sessions == defaults.limits.max_sessions);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_transport_config_tcp_enum_schema_choices_and_invalid_programmatic_values) {
   for (const auto& choice : security_choices) {
      const auto yaml = forge::codec::yaml::read_document(std::string{"tcp:\n  security: "} + choice.name + "\n");
      BOOST_REQUIRE(yaml.ok());
      const auto decoded = forge::config::core::decode<np::config>(yaml.value);
      BOOST_REQUIRE(decoded.ok());
      BOOST_REQUIRE(decoded.value.tcp.security.has_value());
      BOOST_CHECK(*decoded.value.tcp.security == choice.input);
      const auto actual = mapped(decoded.value);
      BOOST_CHECK(actual.stream_security == choice.output);
      check_limits(actual.transport_limits, p2p::node::options{}.transport_limits);
   }
   for (const auto invalid : {np::tcp_security{}, static_cast<np::tcp_security>(255)}) {
      auto config = np::config{};
      config.tcp.security = invalid;
      BOOST_CHECK_THROW(mapped(config), np::exceptions::invalid_config);
   }
}

BOOST_AUTO_TEST_CASE(p2p_transport_config_sparse_patches_preserve_preexisting_native_limits_and_security) {
   auto native = p2p::node::options{};
   native.stream_security = p2p::node::stream_security::noise;
   for (const auto& field : fields) { native.transport_limits.*field.output = 789; }
   native.transport_limits.max_frame_size = 456;
   check_limits(mapped({}, native).transport_limits, native.transport_limits);
   BOOST_CHECK(mapped({}, native).stream_security == native.stream_security);
   auto config = np::config{};
   config.quic.max_queued_bytes = 123;
   const auto actual = mapped(config, native);
   auto expected = native.transport_limits;
   expected.max_queued_bytes = 123;
   check_limits(actual.transport_limits, expected);
   BOOST_CHECK(actual.stream_security == native.stream_security);
}

BOOST_AUTO_TEST_CASE(p2p_transport_config_all_limits_reject_negative_fraction_overflow_and_native_zero) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto valid = valid_document();
   configure_fresh(runtime, valid);
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.name) {
         for (const auto& invalid : {value{std::int64_t{-1}}, value{1.5}, value{"18446744073709551616"},
                                     value{std::uint64_t{0}}}) {
            auto document = valid;
            document.set(std::string{section} + ".quic." + field.name, invalid);
            BOOST_CHECK_THROW(configure_fresh(runtime, document), np::exceptions::invalid_config);
         }
         auto document = valid;
         document.set(std::string{section} + ".quic." + field.name, std::uint64_t{1});
         BOOST_CHECK_NO_THROW(configure_fresh(runtime, document));
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_transport_config_checked_native_size_boundaries_leave_internal_frame_default_untouched) {
   const auto maximum = static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)());
   for (const auto& field : fields) {
      BOOST_TEST_CONTEXT(field.name) {
         auto config = np::config{};
         config.quic.*field.input = maximum;
         const auto actual = mapped(config);
         BOOST_TEST((actual.transport_limits.*field.output) == (std::numeric_limits<std::size_t>::max)());
         BOOST_TEST(actual.transport_limits.max_frame_size == p2p::node::options{}.transport_limits.max_frame_size);
         // Only a narrower size_t target executes this overflow branch.
         if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
            config.quic.*field.input = (std::numeric_limits<std::uint64_t>::max)();
            BOOST_CHECK_THROW(mapped(config), np::exceptions::invalid_config);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_transport_config_valid_yaml_and_strict_unknown_or_malformed_policy) {
   const auto valid = yaml_candidate(
       "tcp:\n  security: tls-and-noise\nquic:\n  max-connections: 17\n"
       "  max-streams-per-connection: 19\n  max-queued-bytes: 65536\n"
       "  max-inbound-queued-bytes: 131072\n  max-inbound-queued-packets: 29\n");
   const auto decoded = np::decode_config({valid, std::string{section}});
   BOOST_REQUIRE(decoded.tcp.security);
   BOOST_CHECK(*decoded.tcp.security == np::tcp_security::tls_and_noise);
   const auto native = mapped(decoded);
   BOOST_TEST(native.transport_limits.max_connections == 17U);
   BOOST_TEST(native.transport_limits.max_streams_per_connection == 19U);
   BOOST_TEST(native.transport_limits.max_queued_bytes == 65536U);
   BOOST_TEST(native.transport_limits.max_inbound_queued_bytes == 131072U);
   BOOST_TEST(native.transport_limits.max_inbound_queued_packets == 29U);
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK_NO_THROW(configure_fresh(runtime, valid));
   for (const auto* text : {
            "tcp:\n  securty: noise\n", "tpc: {}\n", "quic:\n  max-connection: 1\n", "quiic: {}\n",
            "quic:\n  max-frame-size: 1\n", "tcp: null\n", "quic: null\n", "tcp: noise\n",
            "quic: []\n", "tcp:\n  security: unknown\n", "tcp:\n  security: ''\n",
            "tcp:\n  security: 255\n", "tcp:\n  security: 1.5\n", "tcp:\n  security: true\n",
            "tcp:\n  security: []\n", "quic:\n  max-queued-bytes: -1\n",
            "quic:\n  max-queued-bytes: 1.5\n", "quic:\n  max-queued-bytes: 18446744073709551616\n"}) {
      BOOST_TEST_CONTEXT(text) {
         // Merge only this malformed policy into otherwise valid identity/persistence config.
         const auto document = yaml_candidate(text);
         BOOST_CHECK_THROW(configure_fresh(runtime, document), np::exceptions::invalid_config);
      }
   }
   auto permitted = valid;
   permitted.set(std::string{section} + ".enabled", true);
   permitted.set("plugins.other.quic.max-frame-size", std::uint64_t{1});
   BOOST_CHECK_NO_THROW(configure_fresh(runtime, permitted));
}

BOOST_AUTO_TEST_CASE(p2p_transport_config_tls_only_requires_native_relay_client_disabled) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto document = valid_document();
   document.set(std::string{section} + ".tcp.security", "tls");
   BOOST_CHECK_THROW(configure_fresh(runtime, document), np::exceptions::invalid_config);
   document.set(std::string{section} + ".relay.client-enabled", false);
   BOOST_CHECK_NO_THROW(configure_fresh(runtime, document));
}

BOOST_AUTO_TEST_CASE(p2p_transport_config_native_quic_stream_wire_bound_is_checked_before_commit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto document = valid_document();
   constexpr auto maximum = std::uint64_t{1} << 60;
   if constexpr (std::numeric_limits<std::size_t>::digits > 60) {
      document.set(std::string{section} + ".quic.max-streams-per-connection", maximum);
      BOOST_CHECK_NO_THROW(configure_fresh(runtime, document));
      document.set(std::string{section} + ".quic.max-streams-per-connection", maximum + 1);
      BOOST_CHECK_THROW(configure_fresh(runtime, document), np::exceptions::invalid_config);
   } else {
      document.set(std::string{section} + ".quic.max-streams-per-connection",
                   static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)()));
      BOOST_CHECK_NO_THROW(configure_fresh(runtime, document));
      document.set(std::string{section} + ".quic.max-streams-per-connection", maximum);
      BOOST_CHECK_THROW(configure_fresh(runtime, document), np::exceptions::invalid_config);
   }
}
