#include <boost/test/unit_test.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "../fixtures/local_dns_server.hxx"
#include "details/secure_node_fixture.hxx"

import forge.app.exceptions;
import forge.config.core.component;
import forge.config.core.value;
import forge.multiformats.multiaddr;
import forge.net.p2p.endpoint;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;

namespace {
namespace np = forge::plugins::net::p2p::node;
namespace dns = forge::tests::dns;
using application = forge::tests::p2p::secure_node_fixture;
using value = forge::config::core::value;
using namespace std::chrono_literals;
constexpr auto section = "plugins.net.p2p.node.";

value nameservers(std::uint16_t port) {
   return value::array_type{value::object_type{{"address", "127.0.0.1"}, {"port", std::uint64_t{port}}}};
}

dns::response_answer txt(std::string text) {
   if (text.size() > 255) { throw std::length_error{"DNS configuration fixture TXT exceeds one chunk"}; }
   auto bytes = dns::bytes{static_cast<std::uint8_t>(text.size())};
   bytes.insert(bytes.end(), text.begin(), text.end());
   return {.type = 16, .value = std::move(bytes)};
}

boost::asio::awaitable<bool> await_peers(std::shared_ptr<np::diagnostics_api> diagnostics) {
   auto timer = boost::asio::steady_timer{co_await boost::asio::this_coro::executor};
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   do {
      if (diagnostics->snapshot({.max_peers = 16}).peers.size() >= 2U) { co_return true; }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   co_return false;
}
} // namespace

BOOST_AUTO_TEST_CASE(p2p_dial_configuration_rollback_and_freeze_preserve_resolver_and_diagnostics,
                    *boost::unit_test::enable_if<FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB>()) {
   for (const auto& backend : application::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         application first{backend, {.private_profile = false}};
         application second{backend, {.private_profile = false}};
         first.start();
         second.start();
         const auto first_api = first.api<np::api>();
         const auto second_api = second.api<np::api>();
         BOOST_REQUIRE(first_api->local_endpoint());
         BOOST_REQUIRE(second_api->local_endpoint());
         auto first_endpoint = *first_api->local_endpoint();
         auto second_endpoint = *second_api->local_endpoint();
         first_endpoint.peer = first_api->local_peer();
         second_endpoint.peer = second_api->local_peer();
         const auto first_address = first_endpoint.to_string();
         const auto second_address = second_endpoint.to_string();
         auto first_queries = std::atomic_size_t{0};
         auto second_queries = std::atomic_size_t{0};
         auto dns_server = dns::local_dns_server{[&](const std::uint8_t* data, std::size_t size)
             -> std::optional<dns::bytes> {
            const auto question = dns::parse_question(data, size);
            if (!question) { return std::nullopt; }
            if (question->type == 16 && question->name == "_dnsaddr.first.stage7.test") {
               ++first_queries;
               return dns::make_response(data, *question, {txt("dnsaddr=" + first_address)});
            }
            if (question->type == 16 && question->name == "_dnsaddr.second.stage7.test") {
               ++second_queries;
               return dns::make_response(data, *question, {txt("dnsaddr=" + second_address)});
            }
            return dns::make_failure_response(data, *question, 3);
         }};
         auto overrides = forge::config::core::document{};
         overrides.set(std::string{section} + "dns.resolver.nameservers", nameservers(dns_server.port()));
         overrides.set(std::string{section} + "dns.resolution.max-dns-lookups", std::uint64_t{2});
         overrides.set(std::string{section} + "diagnostics.max-peers", std::uint64_t{1});
         application client{backend, {.private_profile = false}, "noise", overrides};
         const auto accepted = client.config;
         auto rejected = accepted;
         rejected.set(std::string{section} + "dns.resolver.nameservers", nameservers(1));
         rejected.set(std::string{section} + "path.max-direct-endpoints", std::uint64_t{0});
         rejected.set(std::string{section} + "diagnostics.max-peers", std::uint64_t{2});
         // Validate initial input independently; the configured owner cannot
         // accept another input, even before startup.
         auto invalid_node = np::plugin{};
         BOOST_CHECK_THROW(client.run(invalid_node.configure({rejected, "plugins.net.p2p.node"})),
                           np::exceptions::invalid_config);
         client.run(invalid_node.shutdown());
         BOOST_CHECK_THROW(client.configure_node(rejected), forge::app::exceptions::invalid_state);
         client.start();
         const auto connect = [&](application& local, std::string name, const auto& remote) {
            const auto session = local.run(local.api<np::api>()->connect(forge::multiformats::multiaddr::parse(
                "/dnsaddr/" + name + "/p2p/" + remote->local_peer().to_string()),
                {.expected_peer = remote->local_peer(), .allow_relay = false, .timeout = 3s,
                 .allow_hole_punch = false}));
            BOOST_CHECK(session.remote_peer == remote->local_peer());
            BOOST_CHECK(session.identify_state == forge::net::p2p::identify::state::identified);
         };
         connect(client, "first.stage7.test", first_api);
         BOOST_TEST(first_queries.load() > 0U);
         for (const auto& [field, setting] : std::vector<std::pair<std::string, value>>{
                  {"dns.resolver.nameservers", nameservers(1)},
                  {"dns.resolution.max-dns-lookups", std::uint64_t{1}},
                  {"dial.max-concurrent-attempts", std::uint64_t{1}},
                  {"path.max-direct-endpoints", std::uint64_t{1}}}) {
            rejected = accepted;
            rejected.set(std::string{section} + field, setting);
            rejected.set(std::string{section} + "diagnostics.max-peers", std::uint64_t{2});
            BOOST_TEST_CONTEXT(field) {
               BOOST_CHECK_THROW(client.configure_node(rejected), forge::app::exceptions::invalid_state);
            }
         }
         connect(client, "second.stage7.test", second_api);
         BOOST_TEST(second_queries.load() > 0U);
         const auto diagnostics = client.api<np::diagnostics_api>();
         BOOST_REQUIRE(client.run(await_peers(diagnostics)));
         BOOST_TEST(diagnostics->snapshot().peers.size() == 1U);
         auto diagnostics_only = accepted;
         diagnostics_only.set(std::string{section} + "diagnostics.max-peers", std::uint64_t{2});
         BOOST_CHECK_THROW(client.configure_node(diagnostics_only), forge::app::exceptions::invalid_state);
         BOOST_TEST(diagnostics->snapshot().peers.size() == 1U);
         BOOST_TEST(diagnostics->snapshot({.max_peers = 2}).peers.size() == 2U);

         auto larger_bounds = overrides;
         larger_bounds.set(std::string{section} + "diagnostics.max-peers", std::uint64_t{2});
         application larger_client{backend, {.private_profile = false}, "noise", larger_bounds};
         larger_client.start();
         connect(larger_client, "first.stage7.test", first_api);
         connect(larger_client, "second.stage7.test", second_api);
         const auto larger_diagnostics = larger_client.api<np::diagnostics_api>();
         BOOST_REQUIRE(larger_client.run(await_peers(larger_diagnostics)));
         BOOST_TEST(larger_diagnostics->snapshot().peers.size() == 2U);
         dns_server.close();
      }
   }
}
