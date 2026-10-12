module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "../quic_p2p/libp2p_identity_fixture.hxx"

module forge.plugins.net.p2p.node.plugin;

import forge.asio.runtime;
import forge.codec.yaml;
import forge.config.core.component;
import forge.config.core.decode;
import forge.config.core.document;
import forge.config.core.value;
import forge.net.p2p.node;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.scoring;
import forge.net.pnet.protector;
import forge.plugins.net.p2p.node.config;
import forge.plugins.net.p2p.node.exceptions;
import forge.schema.object;

#include "../../plugins/net/p2p/node/details/configuration_policy.hxx"

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace p2p = forge::net::p2p;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr std::string_view section = "plugins.net.p2p.node";

forge::config::core::document valid_private_document() {
   auto document = forge::config::core::document{};
   const auto set = [&](std::string_view name, value setting) {
      document.set(std::string{section} + "." + std::string{name}, std::move(setting));
   };
   set("allow-insecure-test-mode", true);
   set("identity.certificate-secret", "private/cert");
   set("identity.private-key-secret", "private/key");
   set("listen", value::array_type{value{"/ip4/127.0.0.1/tcp/0"}});
   set("topology.mode", "static-only");
   set("path.policy", "direct-only");
   set("relay.client-enabled", false);
   set("relay.server-enabled", false);
   set("relay.auto-discovery-enabled", false);
   set("private-network.psk-secret", "private/swarm");
   return document;
}

void configure(forge::asio::runtime& runtime, np::plugin& plugin,
               const forge::config::core::document& document) {
   auto result = boost::asio::co_spawn(runtime.context(),
       plugin.configure({document, std::string{section}}), boost::asio::use_future);
   if (result.wait_for(5s) != std::future_status::ready) { std::terminate(); }
   result.get();
}

forge::config::core::document yaml_candidate(std::string_view text) {
   const auto parsed = forge::codec::yaml::read_document(text);
   BOOST_REQUIRE(parsed.ok());
   const auto* changes = parsed.value.object_at("");
   BOOST_REQUIRE(changes);
   auto document = valid_private_document();
   for (const auto& [name, setting] : *changes) {
      document.set(std::string{section} + "." + name, setting);
   }
   return document;
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_private_network_config_absence_preserves_public_defaults_and_null_is_unselected) {
   const auto decoded = forge::config::core::decode<np::config>({});
   BOOST_REQUIRE(decoded.ok());
   BOOST_TEST(!decoded.value.private_network);
   BOOST_TEST(decoded.value.relay.auto_discovery_enabled);
   auto native = p2p::node::options{};
   np::apply_private_network(native, decoded.value);
   BOOST_TEST(!native.private_network);
   BOOST_TEST(native.capabilities.has(p2p::capabilities::direct_quic));
   BOOST_TEST(native.relay_policy.auto_discovery_enabled);
   const auto parsed = forge::codec::yaml::read_document("private-network: null\n");
   BOOST_REQUIRE(parsed.ok());
   const auto null = forge::config::core::decode<np::config>(parsed.value);
   BOOST_REQUIRE(null.ok());
   BOOST_TEST(!null.value.private_network);
}

BOOST_AUTO_TEST_CASE(p2p_private_network_config_yaml_choices_provision_only_policy_not_key_material) {
   for (const auto* egress : {"deny-external", "allow-internet"}) {
      const auto document = yaml_candidate(std::string{"private-network:\n  psk-secret: private/swarm\n"}
          + "  internet-egress: " + egress + "\n");
      const auto decoded = np::decode_config({document, std::string{section}});
      BOOST_REQUIRE(decoded.private_network);
      BOOST_TEST(decoded.private_network->psk_secret == "private/swarm");
      auto native = p2p::node::options{};
      np::apply_private_network(native, decoded);
      BOOST_REQUIRE(native.private_network);
      BOOST_TEST(!native.private_network->protector);
      const auto expected = std::string_view{egress} == "deny-external"
          ? p2p::private_network::internet_egress_policy::deny_external
          : p2p::private_network::internet_egress_policy::allow_internet;
      BOOST_CHECK(native.private_network->internet_egress == expected);
   }
   const auto decoded = np::decode_config({valid_private_document(), std::string{section}});
   BOOST_REQUIRE(decoded.private_network);
   BOOST_CHECK(decoded.private_network->internet_egress == np::internet_egress_policy::deny_external);
}

BOOST_AUTO_TEST_CASE(p2p_private_network_config_mapping_reconstructs_and_rejects_invalid_programmatic_values) {
   auto config = np::decode_config({valid_private_document(), std::string{section}});
   auto native = p2p::node::options{};
   const auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
   native.private_network.emplace();
   native.private_network->protector = std::make_shared<const forge::net::pnet::protector>(
       forge::net::pnet::pre_shared_key{key});
   np::apply_private_network(native, config);
   BOOST_REQUIRE(native.private_network);
   BOOST_TEST(!native.private_network->protector);
   np::apply_private_network(native, {});
   BOOST_TEST(!native.private_network);
   config.private_network->internet_egress = static_cast<np::internet_egress_policy>(255);
   BOOST_CHECK_THROW(np::apply_private_network(native, config), np::exceptions::invalid_config);
   config.private_network->internet_egress = np::internet_egress_policy::deny_external;
   config.private_network->psk_secret.clear();
   BOOST_CHECK_THROW(np::apply_private_network(native, config), np::exceptions::invalid_config);
}

BOOST_AUTO_TEST_CASE(p2p_private_network_config_strict_yaml_requires_reference_and_rejects_inline_key) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   configure(runtime, plugin, valid_private_document());
   for (const auto* yaml : {
            "private-network: {}\n", "private-network: swarm\n", "private-network: []\n",
            "private-network:\n  psk-secret: ''\n", "private-network:\n  psk-secret: []\n",
            "private-network:\n  psk-secret: private/swarm\n  psk: inline-key-forbidden\n",
            "private-network:\n  psk-secret: private/swarm\n  internet-egress: unknown\n",
            "private-network:\n  psk-secret: private/swarm\n  internet-egress: 1.5\n",
            "private-network:\n  psk-secret: private/swarm\n  internet-egress: -1\n",
            "private-network:\n  psk-secret: private/swarm\n  internet-egress: 18446744073709551616\n",
            "private-netwrok: {}\n", "relay:\n  auto-discovery-enabld: false\n"}) {
      BOOST_TEST_CONTEXT(yaml) {
         BOOST_CHECK_THROW(configure(runtime, plugin, yaml_candidate(yaml)), np::exceptions::invalid_config);
      }
   }
}

BOOST_AUTO_TEST_CASE(p2p_private_network_config_cross_policy_requires_explicit_tcp_direct_only_and_no_relays) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto valid = valid_private_document();
   configure(runtime, plugin, valid);
   for (const auto* path : {"direct-preferred", "relay-only"}) {
      auto candidate = valid;
      candidate.set(std::string{section} + ".path.policy", path);
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
   }
   for (const auto* flag : {"client-enabled", "server-enabled", "auto-discovery-enabled"}) {
      auto candidate = valid;
      candidate.set(std::string{section} + ".relay." + flag, true);
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
   }
   for (const auto* field : {"max-connections", "max-streams-per-connection", "max-queued-bytes",
                            "max-inbound-queued-bytes", "max-inbound-queued-packets"}) {
      for (const auto number : {std::uint64_t{0}, std::uint64_t{1}}) {
         auto candidate = valid;
         candidate.set(std::string{section} + ".quic." + field, number);
         BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
      }
   }
   const auto identity = forge::tests::p2p::make_identity_fixture("private-config-endpoint");
   const auto peer = p2p::make_peer_id_from_certificate_pem(identity.certificate_pem).to_string();
   for (const auto* field : {"listen", "advertised-endpoints", "bootstrap"}) {
      auto candidate = valid;
      candidate.set(std::string{section} + "." + field,
          value::array_type{value{"/ip4/127.0.0.1/udp/4001/quic-v1/p2p/" + peer}});
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
   }
   BOOST_CHECK_NO_THROW(configure(runtime, plugin, valid));
}

BOOST_AUTO_TEST_CASE(p2p_private_network_config_rendezvous_rejects_quic_and_accepts_tcp_for_both_egress_policies) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto plugin = np::plugin{};
   const auto identity = forge::tests::p2p::make_identity_fixture("private-config-rendezvous");
   const auto peer = p2p::make_peer_id_from_certificate_pem(identity.certificate_pem).to_string();
   for (const auto* egress : {"deny-external", "allow-internet"}) {
      auto candidate = valid_private_document();
      candidate.set(std::string{section} + ".private-network.internet-egress", egress);
      candidate.set(std::string{section} + ".rendezvous.role", "client");
      const auto set_point = [&](std::string endpoint) {
         candidate.set(std::string{section} + ".rendezvous.points", value::array_type{
             value::object_type{{"endpoint", std::move(endpoint)},
                                {"namespaces", value::array_type{value{"stage7"}}}}});
      };
      set_point("/ip4/127.0.0.1/tcp/4001/p2p/" + peer);
      BOOST_CHECK_NO_THROW(configure(runtime, plugin, candidate));
      set_point("/ip4/127.0.0.1/udp/4001/quic-v1/p2p/" + peer);
      BOOST_CHECK_THROW(configure(runtime, plugin, candidate), np::exceptions::invalid_config);
   }
}
