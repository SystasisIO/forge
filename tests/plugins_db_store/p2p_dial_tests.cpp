#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.config.core.document;
import forge.config.core.value;
import forge.multiformats.multiaddr;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dialing;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.identify;
import forge.net.p2p.node;
import forge.net.p2p.protocol;
import forge.net.p2p.stream;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.configuration_api;
import forge.plugins.net.p2p.node.diagnostics_api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_api;
import forge.plugins.net.p2p.node.protocol_registration;

#include "../fixtures/local_dns_server.hxx"
#include "details/secure_node_fixture.hxx"

namespace {
namespace p2p = forge::net::p2p;
namespace np = forge::plugins::net::p2p::node;
namespace dns = forge::tests::dns;
using forge::tests::p2p::secure_node_fixture;
using value = forge::config::core::value;
using namespace std::chrono_literals;

struct query_log {
   std::mutex mutex;
   std::vector<dns::parsed_question> questions;
   std::promise<void> first;

   void record(const dns::parsed_question& question) {
      const auto lock = std::scoped_lock{mutex};
      questions.push_back(question);
      if (questions.size() == 1) { first.set_value(); }
   }
   bool saw(std::string_view name) {
      const auto lock = std::scoped_lock{mutex};
      return std::ranges::any_of(questions, [&](const auto& question) {
         return question.type == 16 && question.name == name;
      });
   }
};

class dial_gater final : public p2p::connection_gater {
 public:
   explicit dial_gater(p2p::peer_id expected) : expected_{std::move(expected)} {}
   std::atomic_size_t peers{0};
   std::atomic_size_t addresses{0};
   std::atomic_size_t denied{0};
   std::atomic_size_t foreign{0};
   std::atomic_size_t secured{0};

   bool intercept_peer_dial(const p2p::peer_id& peer) noexcept override {
      ++peers;
      if (peer != expected_) { ++foreign; return false; }
      return true;
   }
   bool intercept_address_dial(const p2p::peer_id& peer, const p2p::endpoint& endpoint) noexcept override {
      ++addresses;
      if (peer != expected_) { ++foreign; return false; }
      if (endpoint.transport.port == 1) { ++denied; return false; }
      return true;
   }
   bool intercept_secured(p2p::connection_direction direction, const p2p::peer_id&,
                           const p2p::connection_endpoints&) noexcept override {
      if (direction == p2p::connection_direction::outbound) { ++secured; }
      return true;
   }

 private:
   const p2p::peer_id expected_;
};

dns::response_answer txt(std::string address) {
   const auto text = "dnsaddr=" + address;
   auto bytes = dns::bytes{};
   for (auto offset = std::size_t{}; offset != text.size();) {
      const auto length = std::min(std::size_t{255}, text.size() - offset);
      bytes.push_back(static_cast<std::uint8_t>(length));
      bytes.insert(bytes.end(), text.begin() + offset, text.begin() + offset + length);
      offset += length;
   }
   return {.type = 16, .value = std::move(bytes), .ttl = 0};
}

using answer_table = std::map<std::string, std::vector<dns::response_answer>>;

dns::local_dns_server::answer_handler answers(std::shared_ptr<query_log> log, answer_table table) {
   return [log = std::move(log), table = std::move(table)](const std::uint8_t* request, std::size_t size)
       -> std::optional<dns::bytes> {
      const auto question = dns::parse_question(request, size);
      if (!question) { throw std::runtime_error{"malformed local DNS request"}; }
      log->record(*question);
      const auto found = table.find(question->name);
      if (question->type != 16 || found == table.end()) {
         return dns::make_failure_response(request, *question, 3);
      }
      return dns::make_response(request, *question, found->second);
   };
}

forge::config::core::document dns_config(std::uint16_t port, std::uint64_t bound = 2) {
   auto document = forge::config::core::document{};
   document.set("plugins.net.p2p.node.dns.resolver.nameservers", value::array_type{
       value::object_type{{"address", "127.0.0.1"}, {"port", static_cast<std::uint64_t>(port)}}});
   document.set("plugins.net.p2p.node.dns.resolution.max-resolved-addresses", bound);
   document.set("plugins.net.p2p.node.dial.max-concurrent-attempts", std::uint64_t{1});
   return document;
}

forge::multiformats::multiaddr root(std::string_view name, const p2p::peer_id& peer) {
   return forge::multiformats::multiaddr::parse("/dnsaddr/" + std::string{name} + "/p2p/" + peer.to_string());
}

std::shared_ptr<dial_gater> install_gater(secure_node_fixture& client, const p2p::peer_id& peer) {
   client.run(client.shell->initialize());
   auto gater = std::make_shared<dial_gater>(peer);
   client.api<np::configuration_api>()->set_connection_gater(gater);
   client.start();
   return gater;
}

boost::asio::awaitable<bool> await_identified(std::shared_ptr<np::diagnostics_api> api,
                                             p2p::peer_id peer, p2p::peer_authentication authentication) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor};
   const auto deadline = std::chrono::steady_clock::now() + 4s;
   do {
      const auto state = api->snapshot();
      if (std::ranges::any_of(state.sessions, [&](const auto& session) {
         return !session.closed && session.remote_peer == peer &&
             session.identify_state == p2p::identify::state::identified &&
             session.authentication == authentication;
      })) { co_return true; }
      timer.expires_after(5ms);
      co_await timer.async_wait(boost::asio::use_awaitable);
   } while (std::chrono::steady_clock::now() < deadline);
   co_return false;
}

np::protocol_registration echo(secure_node_fixture& server, const p2p::protocol_id& protocol) {
   return server.api<np::protocol_api>()->register_protocol(protocol,
       [](p2p::node::incoming_protocol_stream incoming) -> boost::asio::awaitable<void> {
          const auto bytes = co_await incoming.stream.async_read_frame();
          co_await incoming.stream.async_write_frame(bytes);
          co_await incoming.stream.async_close();
       });
}

void exchange(secure_node_fixture& client, secure_node_fixture& server,
              forge::multiformats::multiaddr address, p2p::peer_authentication authentication) {
   const auto peer = server.api<np::api>()->local_peer();
   const auto protocol = p2p::protocol_id{"/stage7/configured-dns/echo/1"};
   auto registration = echo(server, protocol);
   const auto session = client.run(client.api<np::api>()->connect(std::move(address), {
       .expected_peer = peer, .allow_relay = false, .timeout = 4s, .allow_hole_punch = false}), "configured DNS connect");
   BOOST_CHECK(session.remote_peer == peer);
   BOOST_CHECK(session.identify_state == p2p::identify::state::identified);
   BOOST_REQUIRE(client.run(await_identified(client.api<np::diagnostics_api>(), peer, authentication),
                            "client authenticated Identify witness"));
   BOOST_REQUIRE(server.run(await_identified(server.api<np::diagnostics_api>(), client.api<np::api>()->local_peer(),
                                            authentication), "server authenticated Identify witness"));
   auto stream = client.run(client.api<np::protocol_api>()->open(peer, protocol,
       {.allow_relay = false, .timeout = 2s, .allow_hole_punch = false}));
   BOOST_CHECK(stream.authentication() == authentication);
   const auto payload = std::vector<std::uint8_t>{0, 0xff, 2, 0, 4, 5};
   client.run(stream.async_write_frame(payload));
   BOOST_CHECK(client.run(stream.async_read_frame()) == payload);
   client.run(stream.async_close());
}

void no_attempts(secure_node_fixture& client, const std::shared_ptr<dial_gater>& gater) {
   BOOST_TEST(gater->peers.load() == 0U);
   BOOST_TEST(gater->addresses.load() == 0U);
   BOOST_TEST(gater->secured.load() == 0U);
   const auto state = client.api<np::diagnostics_api>()->snapshot();
   BOOST_TEST(state.metrics.handshakes_completed == 0U);
   BOOST_TEST(state.metrics.sessions_opened == 0U);
   BOOST_TEST(state.sessions.empty());
   BOOST_TEST(state.resources.system.outbound_connections == 0U);
}

struct dial_result {
   bool connected = false;
   std::optional<p2p::exceptions::code> error;
};

boost::asio::awaitable<dial_result> observe_connect(std::shared_ptr<np::api> api,
                                                   forge::multiformats::multiaddr address) {
   try {
      static_cast<void>(co_await api->connect(std::move(address), {
          .allow_relay = false, .timeout = 5s, .allow_hole_punch = false}));
      co_return dial_result{.connected = true};
   } catch (const forge::exceptions::base& error) {
      const auto code = p2p::exceptions::code_of(error);
      if (!code) { throw; }
      co_return dial_result{.error = code};
   }
}
} // namespace

BOOST_AUTO_TEST_SUITE(p2p_dial_plugin_test_suite,
                      *boost::unit_test::enable_if<FORGE_HAS_MDBX || FORGE_HAS_ROCKSDB>())

BOOST_AUTO_TEST_CASE(recursive_configured_dns_filters_foreign_peer_and_authenticates_tcp_and_quic) {
   for (const auto& backend : secure_node_fixture::backends()) {
      for (const auto mode : {std::string_view{"noise"}, std::string_view{"tls"}, std::string_view{"quic"}}) {
         BOOST_TEST_CONTEXT(backend << "/" << mode) {
            auto overrides = forge::config::core::document{};
            if (mode == "quic") {
               overrides.set("plugins.net.p2p.node.listen", value::array_type{value{"/ip4/127.0.0.1/udp/0/quic-v1"}});
            }
            secure_node_fixture server{backend, {.include = false, .private_profile = false},
                                       mode == "tls" ? "tls" : "noise", overrides};
            server.start();
            const auto remote = server.api<np::api>();
            const auto endpoint = remote->local_endpoint();
            BOOST_REQUIRE(endpoint);
            const auto expected = remote->local_peer();
            auto foreign = *endpoint;
            foreign.peer = p2p::make_peer_id_from_certificate_pem(
                forge::tests::p2p::make_identity_fixture("foreign-dns-peer").certificate_pem);
            const auto log = std::make_shared<query_log>();
            dns::local_dns_server dns{answers(log, {
                {"_dnsaddr.root.test", {txt(root("leaf.test", expected).to_string())}},
                {"_dnsaddr.leaf.test", {txt(foreign.to_string()), txt(endpoint->to_string())}}})};
            secure_node_fixture client{backend, {.include = false, .private_profile = false},
                                       mode == "tls" ? "tls" : "noise", dns_config(dns.port(), 1)};
            const auto gater = install_gater(client, expected);
            exchange(client, server, root("root.test", expected), mode == "quic" ? p2p::peer_authentication::quic_tls
                : mode == "tls" ? p2p::peer_authentication::libp2p_tls : p2p::peer_authentication::noise);
            BOOST_TEST(log->saw("_dnsaddr.root.test"));
            BOOST_TEST(log->saw("_dnsaddr.leaf.test"));
            BOOST_TEST(gater->foreign.load() == 0U);
            BOOST_TEST(gater->addresses.load() == 1U);
            dns.close();
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(resolution_bound_precedes_attempts_and_post_resolution_gater_rejects_candidate) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         secure_node_fixture server{backend, {.include = false, .private_profile = false}};
         server.start();
         const auto remote = server.api<np::api>();
         const auto endpoint = remote->local_endpoint();
         BOOST_REQUIRE(endpoint);
         const auto expected = remote->local_peer();
         auto denied = *endpoint;
         denied.transport.port = 1;
         auto foreign = denied;
         foreign.peer = p2p::make_peer_id_from_certificate_pem(
             forge::tests::p2p::make_identity_fixture("foreign-bound-peer").certificate_pem);
         const auto log = std::make_shared<query_log>();
         dns::local_dns_server dns{answers(log, {{"_dnsaddr.bounded.test",
             {txt(foreign.to_string()), txt(denied.to_string()), txt(endpoint->to_string())}}})};
         for (const auto bound : {std::uint64_t{1}, std::uint64_t{2}}) {
            BOOST_TEST_CONTEXT("max-resolved-addresses=" << bound) {
               secure_node_fixture client{backend, {.include = false, .private_profile = false}, "noise",
                                          dns_config(dns.port(), bound)};
               const auto gater = install_gater(client, expected);
               const auto before = client.api<np::diagnostics_api>()->snapshot().black_holes;
               if (bound == 1) {
                  BOOST_CHECK_THROW(client.run(client.api<np::api>()->connect(root("bounded.test", expected), {
                      .allow_relay = false, .timeout = 4s, .allow_hole_punch = false})), p2p::exceptions::invalid_options);
                  no_attempts(client, gater);
               } else {
                  exchange(client, server, root("bounded.test", expected), p2p::peer_authentication::noise);
                  BOOST_TEST(gater->addresses.load() == 2U);
                  BOOST_TEST(gater->denied.load() == 1U);
                  BOOST_TEST(gater->foreign.load() == 0U);
                  const auto after = client.api<np::diagnostics_api>()->snapshot();
                  BOOST_TEST(after.metrics.gater_address_dial_rejections == 1U);
                  // Loopback is excluded from detector sampling. Public rejection
                  // neutrality is covered separately by native dial_scheduler_tests.
                  BOOST_TEST(after.black_holes.udp.outcomes == before.udp.outcomes);
                  BOOST_TEST(after.black_holes.ipv6.outcomes == before.ipv6.outcomes);
               }
               BOOST_TEST(log->saw("_dnsaddr.bounded.test"));
            }
         }
         dns.close();
      }
   }
}

BOOST_AUTO_TEST_CASE(explicit_nxdomain_joins_without_any_transport_or_authentication) {
   for (const auto& backend : secure_node_fixture::backends()) {
      BOOST_TEST_CONTEXT(backend) {
         const auto expected = p2p::make_peer_id_from_certificate_pem(
             forge::tests::p2p::make_identity_fixture("absent-dns-peer").certificate_pem);
         const auto log = std::make_shared<query_log>();
         dns::local_dns_server dns{answers(log, {})};
         secure_node_fixture client{backend, {.include = false, .private_profile = false}, "noise", dns_config(dns.port())};
         const auto gater = install_gater(client, expected);
         BOOST_CHECK_THROW(client.run(client.api<np::api>()->connect(root("absent.test", expected), {
             .allow_relay = false, .timeout = 4s, .allow_hole_punch = false})), p2p::exceptions::peer_not_found);
         BOOST_TEST(log->saw("_dnsaddr.absent.test"));
         no_attempts(client, gater);
         // An explicit shutdown joins all remaining native work: no delayed success can outlive it.
         client.run(client.shutdown_node(), "NXDOMAIN native shutdown join");
         BOOST_TEST(gater->secured.load() == 0U);
         dns.close();
      }
   }
}

BOOST_AUTO_TEST_CASE(observed_no_reply_dns_joins_parent_cancellation_and_node_shutdown_separately) {
   for (const auto& backend : secure_node_fixture::backends()) {
      for (const auto parent_cancel : {true, false}) {
         BOOST_TEST_CONTEXT(backend << (parent_cancel ? "/parent-cancel" : "/node-shutdown")) {
            const auto expected = p2p::make_peer_id_from_certificate_pem(
                forge::tests::p2p::make_identity_fixture("held-dns-peer").certificate_pem);
            const auto log = std::make_shared<query_log>();
            auto entered = log->first.get_future();
            dns::local_dns_server dns{[log](const std::uint8_t* request, std::size_t size) -> std::optional<dns::bytes> {
               const auto question = dns::parse_question(request, size);
               if (!question) { throw std::runtime_error{"malformed held DNS request"}; }
               log->record(*question);
               return std::nullopt; // No reply, not a blocked external callback.
            }};
            secure_node_fixture client{backend, {.include = false, .private_profile = false}, "noise", dns_config(dns.port())};
            const auto gater = install_gater(client, expected);
            const auto strand = boost::asio::make_strand(client.shell->runtime().context());
            const auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
            auto connecting = boost::asio::co_spawn(strand,
                observe_connect(client.api<np::api>(), root("held.test", expected)),
                boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_future));
            auto shutdown = std::future<void>{};
            auto cleanup = boost::scope::scope_exit{[&] {
               client.request_stop_node();
               auto failure = std::exception_ptr{};
               try { if (connecting.valid()) { static_cast<void>(secure_node_fixture::join(connecting, "DNS connect cleanup")); } }
               catch (...) { failure = std::current_exception(); }
               try { if (shutdown.valid()) { secure_node_fixture::join(shutdown, "DNS shutdown cleanup"); } }
               catch (...) { if (!failure) { failure = std::current_exception(); } }
               if (failure) { BOOST_ERROR("held DNS cleanup failed after both owned joins"); }
            }};
            secure_node_fixture::join(entered, "actual no-reply DNS query witness");
            BOOST_REQUIRE(log->saw("_dnsaddr.held.test"));
            BOOST_REQUIRE(connecting.wait_for(0s) == std::future_status::timeout);
            no_attempts(client, gater);
            if (parent_cancel) {
               boost::asio::post(strand, [cancellation] { cancellation->emit(boost::asio::cancellation_type::terminal); });
            } else {
               shutdown = boost::asio::co_spawn(client.shell->runtime().context(), client.shutdown_node(),
                                                boost::asio::use_future);
            }
            const auto result = secure_node_fixture::join(connecting, "cancelled configured DNS connect");
            BOOST_TEST(!result.connected);
            BOOST_REQUIRE(result.error);
            BOOST_TEST(static_cast<int>(*result.error) == static_cast<int>(p2p::exceptions::code::canceled));
            if (parent_cancel) {
               no_attempts(client, gater);
               shutdown = boost::asio::co_spawn(client.shell->runtime().context(), client.shutdown_node(),
                                                boost::asio::use_future);
            }
            secure_node_fixture::join(shutdown, "independent configured node shutdown join");
            BOOST_TEST(gater->addresses.load() == 0U);
            BOOST_TEST(gater->secured.load() == 0U);
            BOOST_CHECK_THROW(static_cast<void>(client.api<np::diagnostics_api>()->snapshot()),
                              np::exceptions::plugin_not_initialized);
            dns.close();
         }
      }
   }
}

BOOST_AUTO_TEST_SUITE_END()
