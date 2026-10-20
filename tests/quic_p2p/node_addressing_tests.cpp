#include <boost/test/unit_test.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/address_v6.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

#include "../fixtures/local_dns_server.hxx"
#include "libp2p_identity_fixture.hxx"

import forge.asio.blocking;
import forge.asio.runtime;
import forge.exceptions;
import forge.multiformats.multiaddr;
import forge.net.dns.types;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.dialing;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.pnet.protector;

namespace {

namespace asio = boost::asio;
namespace p2p = forge::net::p2p;
namespace dns_fixture = forge::tests::dns;
using tcp = asio::ip::tcp;
using namespace std::chrono_literals;

class dns_candidate_gate final : public p2p::connection_gater {
 public:
   explicit dns_candidate_gate(std::optional<p2p::peer_id> forbidden = {}, bool reject_peer = true)
       : forbidden_(std::move(forbidden)), reject_peer_(reject_peer) {}

   bool intercept_peer_dial(const p2p::peer_id& peer) noexcept override {
      if (forbidden_ == peer) {
         ++forbidden_peer_calls;
         return !reject_peer_;
      }
      ++allowed_peer_calls;
      return true;
   }

   bool intercept_address_dial(const p2p::peer_id& peer, const p2p::endpoint& address) noexcept override {
      if (address.peer != peer || address.transport.host_type != p2p::endpoint::host_kind::ip4 ||
          address.transport.host != "127.0.0.1") {
         unexpected_address = true;
      }
      if (forbidden_ == peer) {
         ++forbidden_address_calls;
         return false;
      }
      ++allowed_address_calls;
      return true;
   }

   bool intercept_accept(const p2p::connection_endpoints&) noexcept override {
      ++accept_calls;
      return true;
   }

   std::atomic_size_t forbidden_peer_calls{0};
   std::atomic_size_t forbidden_address_calls{0};
   std::atomic_size_t allowed_peer_calls{0};
   std::atomic_size_t allowed_address_calls{0};
   std::atomic_size_t accept_calls{0};
   std::atomic_bool unexpected_address{false};

 private:
   const std::optional<p2p::peer_id> forbidden_;
   const bool reject_peer_;
};

class ipv4_dial_gate final : public p2p::connection_gater {
 public:
   ipv4_dial_gate() : entered(entered_.get_future()), released_(release_.get_future().share()) {}

   bool intercept_peer_dial(const p2p::peer_id&) noexcept override {
      ++peer_calls;
      return true;
   }

   bool intercept_address_dial(const p2p::peer_id&, const p2p::endpoint& address) noexcept override {
      if (address.transport.host_type == p2p::endpoint::host_kind::ip6 && address.transport.host == "::1") {
         ++ipv6_calls;
         return true;
      }
      if (address.transport.host_type == p2p::endpoint::host_kind::ip4 && address.transport.host == "127.0.0.1") {
         if (ipv4_calls.fetch_add(1) == 0) {
            entered_.set_value();
         }
         return released_.wait_for(8s) == std::future_status::ready;
      }
      unexpected_address = true;
      return false;
   }

   void release() noexcept {
      if (!released_once_.exchange(true)) {
         release_.set_value();
      }
   }

 private:
   std::promise<void> entered_;
   std::promise<void> release_;

 public:
   std::future<void> entered;
   std::atomic_size_t peer_calls{0};
   std::atomic_size_t ipv4_calls{0};
   std::atomic_size_t ipv6_calls{0};
   std::atomic_bool unexpected_address{false};

 private:
   std::shared_future<void> released_;
   std::atomic_bool released_once_{false};
};

class stalled_ipv6_peer final {
 public:
   stalled_ipv6_peer(forge::asio::runtime& runtime, std::uint16_t port)
       : state_(std::make_shared<state>(runtime, port)), accepted(state_->accepted.get_future()),
         finished(asio::co_spawn(state_->executor, run(state_), asio::use_future)) {}

   ~stalled_ipv6_peer() {
      if (!finished.valid()) {
         return;
      }
      asio::post(state_->executor, [state = state_] {
         auto ignored = boost::system::error_code{};
         state->acceptor.close(ignored);
         state->socket.close(ignored);
      });
      try {
         static_cast<void>(finished.get());
      } catch (...) {
         // Failed assertions still cancel and join the owned socket worker.
      }
   }

 private:
   struct state {
      state(forge::asio::runtime& runtime, std::uint16_t port)
          : executor(asio::make_strand(runtime.context())), acceptor(executor), socket(executor) {
         acceptor.open(tcp::v6());
         acceptor.set_option(asio::ip::v6_only{true});
         acceptor.bind(tcp::endpoint{asio::ip::address_v6::loopback(), port});
         acceptor.listen();
      }

      asio::strand<asio::io_context::executor_type> executor;
      tcp::acceptor acceptor;
      tcp::socket socket;
      std::promise<void> accepted;
   };

   static asio::awaitable<std::size_t> run(std::shared_ptr<state> state) {
      co_await state->acceptor.async_accept(state->socket, asio::use_awaitable);
      state->acceptor.close();
      state->accepted.set_value();
      auto received = std::size_t{};
      auto buffer = std::array<std::uint8_t, 4096>{};
      // Consume handshake bytes without replying. Only client terminal close
      // ends the successful path; no timer closes the accepted socket for it.
      for (;;) {
         auto error = boost::system::error_code{};
         received += co_await state->socket.async_read_some(asio::buffer(buffer),
                                                           asio::redirect_error(asio::use_awaitable, error));
         if (error == asio::error::eof || error == asio::error::connection_reset) {
            state->socket.close();
            co_return received;
         }
         if (error) {
            throw boost::system::system_error{error};
         }
      }
   }

   std::shared_ptr<state> state_;

 public:
   std::future<void> accepted;
   std::future<std::size_t> finished;
   std::uint16_t port() const { return state_->acceptor.local_endpoint().port(); }
};

p2p::node::options addressing_options(std::string_view name, bool private_profile) {
   auto identity = forge::tests::p2p::make_identity_fixture(name);
   auto options = p2p::node::options{};
   options.certificate_pem = std::move(identity.certificate_pem);
   options.private_key_pem = std::move(identity.private_key_pem);
   options.allow_insecure_test_mode = false;
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   if (private_profile) {
      auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
      for (auto index = std::size_t{}; index < key.size(); ++index) {
         key[index] = static_cast<std::uint8_t>(index);
      }
      options.private_network = p2p::private_network::options{
          .protector = std::make_shared<const forge::net::pnet::protector>(forge::net::pnet::pre_shared_key{key})};
      options.capabilities = p2p::capability_set{.bits = p2p::capabilities::peer_exchange};
      options.relay_policy.service_enabled = false;
      options.relay_policy.client_enabled = false;
      options.relay_policy.auto_discovery_enabled = false;
      options.path_policy.allow_relay = false;
      options.path_policy.allow_hole_punch = false;
   }
   return options;
}

asio::awaitable<void> echo_once(p2p::node::incoming_protocol_stream incoming) {
   const auto frame = co_await incoming.stream.async_read_frame();
   co_await incoming.stream.async_write_frame(frame);
}

void check_dnsaddr_candidate_gating(bool with_sibling) {
   for (const auto private_profile : {false, true}) {
      for (const auto reject_peer : {true, false}) {
         BOOST_TEST_CONTEXT("private=" << private_profile << " peer denial=" << reject_peer
                                       << " allowed sibling=" << with_sibling) {
            auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
            auto first_gate = std::make_shared<dns_candidate_gate>();
            auto second_gate = std::make_shared<dns_candidate_gate>();
            auto first_options = addressing_options("candidate-gating-first", private_profile);
            auto second_options = addressing_options("candidate-gating-second", private_profile);
            first_options.connection_gater = first_gate;
            second_options.connection_gater = second_gate;
            auto first = p2p::node{runtime, std::move(first_options)};
            auto second = p2p::node{runtime, std::move(second_options)};
            for (auto* server : {&first, &second}) {
               forge::asio::blocking::run(runtime, server->async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
               BOOST_REQUIRE(server->local_endpoint());
               BOOST_REQUIRE((server->local_endpoint()->peer == server->local_peer()));
            }
            BOOST_REQUIRE(first.local_endpoint());
            BOOST_REQUIRE(second.local_endpoint());
            // Equal-family TCP targets rank by port. Reject the first candidate
            // so success cannot hide a missing gate on the forbidden peer.
            auto* forbidden = &first;
            auto* allowed = &second;
            auto forbidden_gate = first_gate;
            if (first.local_endpoint()->transport.port > second.local_endpoint()->transport.port) {
               std::swap(forbidden, allowed);
               forbidden_gate = second_gate;
            }
            BOOST_REQUIRE((forbidden->local_peer() != allowed->local_peer()));
            auto records = std::vector<std::string>{forbidden->local_endpoint()->to_string()};
            if (with_sibling) {
               records.push_back(allowed->local_endpoint()->to_string());
            }
            auto questions = std::make_shared<std::atomic_size_t>(0);
            auto dns = dns_fixture::local_dns_server{
                [records = std::move(records), questions](const std::uint8_t* data, std::size_t size)
                    -> std::optional<dns_fixture::bytes> {
                   const auto question = dns_fixture::parse_question(data, size);
                   if (!question) {
                      return std::nullopt;
                   }
                   if (question->name != "_dnsaddr.candidate-gating.test" || question->type != 16) {
                      return dns_fixture::make_failure_response(data, *question, 3);
                   }
                   ++*questions;
                   auto answers = std::vector<dns_fixture::response_answer>{};
                   for (const auto& address : records) {
                      const auto text = "dnsaddr=" + address;
                      auto value = dns_fixture::bytes{};
                      for (auto offset = std::size_t{}; offset < text.size();) {
                         const auto remaining = text.size() - offset;
                         const auto count = remaining < 255 ? remaining : std::size_t{255};
                         value.push_back(static_cast<std::uint8_t>(count));
                         value.insert(value.end(), text.begin() + offset, text.begin() + offset + count);
                         offset += count;
                      }
                      answers.push_back({.type = 16, .value = std::move(value)});
                   }
                   return dns_fixture::make_response(data, *question, answers);
                }};
            auto gate = std::make_shared<dns_candidate_gate>(forbidden->local_peer(), reject_peer);
            auto options = addressing_options("candidate-gating-client", private_profile);
            options.connection_gater = gate;
            options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns.port()}};
            options.direct_dial.max_concurrent_attempts = 1;
            options.limits.resources.max_dial_attempts = 1;
            auto client = p2p::node{runtime, std::move(options)};
            auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{&client, [&](void*) noexcept {
               for (auto* owner : {&client, &first, &second}) {
                  owner->request_stop();
               }
               for (auto* owner : {&client, &first, &second}) {
                  try {
                     forge::asio::blocking::run(runtime, owner->async_stop());
                  } catch (...) {
                     BOOST_ERROR("DNS candidate gating fixture failed to join node shutdown");
                  }
               }
            }};
            const auto root = forge::multiformats::multiaddr::parse("/dnsaddr/candidate-gating.test");
            auto connecting = asio::co_spawn(
                runtime.context(), client.async_connect(root, p2p::node::connect_options{
                    .allow_relay = false, .timeout = 2s, .direct_attempt_timeout = 1s,
                    .max_direct_endpoints = 2, .allow_hole_punch = false}), asio::use_future);
            BOOST_REQUIRE(connecting.wait_for(4s) == std::future_status::ready);
            if (with_sibling) {
               const auto session = connecting.get();
               BOOST_TEST(session.remote_peer.to_string() == allowed->local_peer().to_string());
               const auto record = client.peers().find(allowed->local_peer());
               BOOST_REQUIRE(record);
               BOOST_TEST(std::ranges::any_of(record->endpoints, [&](const auto& entry) {
                  return entry.address.to_string() == root.to_string() && entry.sources.learned && entry.successes > 0;
               }));
            } else {
               BOOST_CHECK_THROW(static_cast<void>(connecting.get()), p2p::exceptions::connection_rejected);
               const auto snapshot = client.diagnostics();
               BOOST_TEST(snapshot.sessions.empty());
               BOOST_TEST(snapshot.resources.system.outbound_connections == 0U);
               BOOST_TEST(snapshot.resources.system.file_descriptors == 0U);
            }
            BOOST_TEST(questions->load() == 1U);
            BOOST_TEST(gate->forbidden_peer_calls.load() == 1U);
            BOOST_TEST(gate->forbidden_address_calls.load() == (reject_peer ? 0U : 1U));
            BOOST_TEST(gate->allowed_peer_calls.load() == (with_sibling ? 1U : 0U));
            BOOST_TEST(gate->allowed_address_calls.load() == (with_sibling ? 1U : 0U));
            BOOST_TEST(!gate->unexpected_address.load());
            BOOST_TEST(forbidden_gate->accept_calls.load() == 0U);
            BOOST_TEST(client.metrics().path_direct_attempts == (with_sibling ? 1U : 0U));
            BOOST_TEST(client.metrics().gater_peer_dial_rejections == (reject_peer ? 1U : 0U));
            BOOST_TEST(client.metrics().gater_address_dial_rejections == (reject_peer ? 0U : 1U));
            BOOST_TEST(!client.peers().find(forbidden->local_peer()).has_value());
            const auto snapshot = client.diagnostics();
            BOOST_TEST(snapshot.resources.active_dials == 0U);
            BOOST_TEST(snapshot.resources.transient.outbound_connections == 0U);
            BOOST_TEST(snapshot.black_holes.udp.outcomes == 0U);
            BOOST_TEST(snapshot.black_holes.ipv6.outcomes == 0U);
            dns.close();
         }
      }
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(node_addressing_tests)

BOOST_AUTO_TEST_CASE(p2p_suffixless_dnsaddr_candidate_peer_and_address_denials_precede_transport) {
   check_dnsaddr_candidate_gating(false);
}

BOOST_AUTO_TEST_CASE(p2p_suffixless_dnsaddr_candidate_denial_does_not_pin_sibling_peer) {
   check_dnsaddr_candidate_gating(true);
}

BOOST_AUTO_TEST_CASE(p2p_dual_stack_dns_tcp_race_preserves_logical_ownership_and_drains_loser) {
   for (const auto private_profile : {false, true}) {
      BOOST_TEST_CONTEXT("private profile=" << private_profile) {
         auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
         auto a_queries = std::make_shared<std::atomic_size_t>(0);
         auto aaaa_queries = std::make_shared<std::atomic_size_t>(0);
         auto dns = dns_fixture::local_dns_server{
             [a_queries, aaaa_queries](const std::uint8_t* data, std::size_t size) -> std::optional<dns_fixture::bytes> {
                const auto question = dns_fixture::parse_question(data, size);
                if (!question) {
                   return std::nullopt;
                }
                if (question->name != "dual-stack.test") {
                   return dns_fixture::make_failure_response(data, *question, 3);
                }
                auto answers = std::vector<dns_fixture::response_answer>{};
                if (question->type == 1) {
                   ++*a_queries;
                   answers.push_back({.type = 1, .value = {127, 0, 0, 1}});
                } else if (question->type == 28) {
                   ++*aaaa_queries;
                   auto value = dns_fixture::bytes(16, 0);
                   value.back() = 1;
                   answers.push_back({.type = 28, .value = std::move(value)});
                }
                return dns_fixture::make_response(data, *question, answers);
             }};
         auto server = p2p::node{runtime, addressing_options("dual-stack-server", private_profile)};
         auto gate = std::make_shared<ipv4_dial_gate>();
         auto client_options = addressing_options("dual-stack-client", private_profile);
         client_options.connection_gater = gate;
         client_options.dns_resolver.nameservers = {{.address = "127.0.0.1", .port = dns.port()}};
         client_options.limits.resources.max_dial_attempts = 1;
         client_options.limits.resources.max_dial_attempts_per_peer = 1;
         client_options.direct_dial.max_concurrent_attempts = 2;
         auto client = p2p::node{runtime, std::move(client_options)};
         auto cleanup = std::unique_ptr<void, std::function<void(void*)>>{&client, [&](void*) noexcept {
            gate->release();
            client.request_stop();
            server.request_stop();
            for (auto* owner : {&client, &server}) {
               try {
                  forge::asio::blocking::run(runtime, owner->async_stop());
               } catch (...) {
                  BOOST_ERROR("dual-stack fixture failed to join node shutdown");
               }
            }
         }};
         server.register_protocol_handler(p2p::builtins::echo, echo_once);
         forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
         const auto listening = server.local_endpoint();
         BOOST_REQUIRE(listening);
         auto stalled = stalled_ipv6_peer{runtime, listening->transport.port};
         const auto root = forge::multiformats::multiaddr::parse(
             "/dns/dual-stack.test/tcp/" + std::to_string(listening->transport.port) + "/p2p/" + server.local_peer().to_string());
         server.set_advertised_endpoints({p2p::parse_endpoint(root.to_string())});
         const auto started = std::chrono::steady_clock::now();
         auto connecting = asio::co_spawn(
             runtime.context(), client.async_connect(root, p2p::node::connect_options{
                 .expected_peer = server.local_peer(), .allow_relay = false, .timeout = 20s,
                 .direct_attempt_timeout = 15s, .max_direct_endpoints = 2, .allow_hole_punch = false}),
             asio::use_future);
         BOOST_REQUIRE(stalled.accepted.wait_until(started + 5s) == std::future_status::ready);
         stalled.accepted.get();
         BOOST_REQUIRE(gate->entered.wait_until(started + 5s) == std::future_status::ready);
         gate->entered.get();
         BOOST_TEST(gate->peer_calls.load() == 1U);
         BOOST_TEST(gate->ipv6_calls.load() == 1U);
         BOOST_TEST(gate->ipv4_calls.load() == 1U);
         BOOST_TEST(!gate->unexpected_address.load());
         const auto pending = client.diagnostics().resources;
         BOOST_TEST(pending.active_dials == 1U);
         BOOST_TEST(pending.system.outbound_connections == 1U);
         BOOST_TEST(pending.system.file_descriptors == 1U);
         BOOST_CHECK(connecting.wait_for(0s) == std::future_status::timeout);
         BOOST_CHECK(stalled.finished.wait_for(0s) == std::future_status::timeout);
         gate->release();
         BOOST_REQUIRE(connecting.wait_until(started + 8s) == std::future_status::ready);
         const auto session = connecting.get();
         const auto connected = client.diagnostics();
         BOOST_TEST(session.remote_peer.to_string() == server.local_peer().to_string());
         BOOST_TEST(connected.resources.active_dials == 0U);
         BOOST_TEST(connected.resources.transient.outbound_connections == 0U);
         BOOST_TEST(connected.resources.system.outbound_connections == 1U);
         BOOST_TEST(connected.resources.system.file_descriptors == 1U);
         BOOST_TEST(client.metrics().path_direct_attempts == 2U);
         BOOST_REQUIRE(stalled.finished.wait_until(started + 10s) == std::future_status::ready);
         BOOST_TEST(stalled.finished.get() > 0U);
         const auto winner = std::ranges::find_if(connected.sessions, [&](const auto& value) {
            return !value.closed && value.remote_peer == server.local_peer();
         });
         BOOST_REQUIRE(winner != connected.sessions.end());
         BOOST_REQUIRE(winner->remote_endpoint);
         BOOST_TEST(winner->remote_endpoint->transport.host == "127.0.0.1");
         auto stream = forge::asio::blocking::run(runtime, client.async_open_protocol_stream(
             server.local_peer(), p2p::builtins::echo, p2p::node::open_options{.allow_relay = false, .timeout = 3s,
                                                                          .allow_hole_punch = false}));
         const auto payload = std::vector<std::uint8_t>{'d', 'u', 'a', 'l'};
         forge::asio::blocking::run(runtime, stream.async_write_frame(payload));
         const auto echoed = forge::asio::blocking::run(runtime, stream.async_read_frame());
         BOOST_TEST(echoed == payload, boost::test_tools::per_element());
         BOOST_CHECK(std::chrono::steady_clock::now() - started < 12s);
         BOOST_TEST(client.metrics().path_direct_attempts == 3U);
         BOOST_TEST(client.metrics().handshakes_completed == 1U);
         BOOST_TEST(client.metrics().direct_failures == 0U);
         BOOST_TEST(gate->peer_calls.load() == 1U);
         BOOST_TEST(a_queries->load() > 0U);
         BOOST_TEST(aaaa_queries->load() > 0U);
         const auto peer = client.peers().find(server.local_peer());
         BOOST_REQUIRE(peer);
         BOOST_TEST(peer->failures == 0U);
         const auto source = std::ranges::find_if(peer->endpoints, [&](const auto& value) {
            return value.address.to_string() == root.to_string();
         });
         BOOST_REQUIRE(source != peer->endpoints.end());
         BOOST_TEST(source->successes >= 1U);
         BOOST_TEST(source->failures == 0U);
         BOOST_TEST(source->sources.learned);
         for (const auto& address : peer->endpoints) {
            if (address.address.to_string() != root.to_string()) {
               BOOST_TEST(!address.sources.learned);
               BOOST_TEST(address.successes == 0U);
               BOOST_TEST(address.failures == 0U);
            }
         }
         forge::asio::blocking::run(runtime, stream.async_close());
         forge::asio::blocking::run(runtime, client.async_stop());
         forge::asio::blocking::run(runtime, server.async_stop());
         BOOST_TEST(client.diagnostics().resources.active_dials == 0U);
         BOOST_TEST(client.diagnostics().resources.system.outbound_connections == 0U);
         BOOST_TEST(client.diagnostics().resources.system.file_descriptors == 0U);
         dns.close();
      }
   }
}

BOOST_AUTO_TEST_CASE(operation_hints_open_unknown_peer_with_failed_root_and_no_durable_root_facts) {
   for (const auto private_profile : {false, true}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
      auto server = p2p::node{runtime, addressing_options("hint-target", private_profile)};
      auto options = addressing_options("hint-client", private_profile);
      options.direct_dial.max_concurrent_attempts = 1;
      auto client = p2p::node{runtime, std::move(options)};
      forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
      const auto good = *server.local_endpoint();
      auto refused = good;
      auto reserved = tcp::acceptor{runtime.context()};
      reserved.open(tcp::v4());
      auto found = false;
      for (auto port = std::uint16_t{10240}; port < 10340 && port < good.transport.port; ++port) {
         auto error = boost::system::error_code{};
         reserved.bind({boost::asio::ip::address_v4::loopback(), port}, error);
         if (!error) { refused.transport.port = port; found = true; break; }
      }
      BOOST_REQUIRE(found);
      reserved.close();
      BOOST_CHECK(!client.peers().find(server.local_peer()));
      auto stream = forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(),
         p2p::builtins::ping, {.allow_relay = false, .timeout = 3s, .max_direct_endpoints = 2,
            .allow_hole_punch = false, .address_hints = {refused.to_multiaddr(), good.to_multiaddr()}}));
      BOOST_CHECK(stream.authentication() != p2p::peer_authentication::unverified);
      BOOST_CHECK_GE(client.metrics().direct_failures, 1U);
      forge::asio::blocking::run(runtime, stream.async_close());
      if (const auto row = client.peers().find(server.local_peer())) {
         for (const auto& endpoint : row->endpoints) {
            BOOST_CHECK_EQUAL(endpoint.successes, 0U);
            BOOST_CHECK_EQUAL(endpoint.failures, 0U);
            BOOST_CHECK(!endpoint.sources.learned);
            BOOST_CHECK(endpoint.address.components() != refused.to_multiaddr().components());
         }
      }
      forge::asio::blocking::run(runtime, client.async_stop());
      forge::asio::blocking::run(runtime, server.async_stop());
      BOOST_CHECK_EQUAL(client.diagnostics().resources.active_dials, 0U);
   }
}

BOOST_AUTO_TEST_CASE(operation_hints_enforce_bounds_identity_and_gater_before_publication) {
   auto runtime = forge::asio::runtime{};
   auto server = p2p::node{runtime, addressing_options("hint-auth-target", false)};
   forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   const auto endpoint = server.local_endpoint()->to_multiaddr();
   auto gate = std::make_shared<dns_candidate_gate>(server.local_peer());
   auto gated_options = addressing_options("hint-gated", false);
   gated_options.connection_gater = gate;
   gated_options.dns_resolution.bounds.max_roots = 1;
   auto gated = p2p::node{runtime, std::move(gated_options)};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, gated.async_open_protocol_stream(server.local_peer(),
      p2p::builtins::ping, {.address_hints = {endpoint, endpoint}})), p2p::exceptions::invalid_options);
   BOOST_CHECK_EQUAL(gate->forbidden_peer_calls.load(), 0U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, gated.async_open_protocol_stream(server.local_peer(),
      p2p::builtins::ping, {.allow_relay = false, .address_hints = {endpoint}})), p2p::exceptions::connection_rejected);
   BOOST_CHECK_GE(gate->forbidden_peer_calls.load(), 1U);
   BOOST_CHECK(!gated.peers().find(server.local_peer()));
   auto wrong = p2p::node{runtime, addressing_options("hint-wrong-identity", false)};
   const auto expected = p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
      .data = std::vector<std::uint8_t>(32, 77)});
   auto substituted = *server.local_endpoint();
   substituted.peer = expected;
   BOOST_CHECK_EXCEPTION(forge::asio::blocking::run(runtime, wrong.async_open_protocol_stream(expected,
      p2p::builtins::ping, {.allow_relay = false, .address_hints = {substituted.to_multiaddr()}})),
      forge::exceptions::base, [](const auto& error) {
         return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_verification_failed;
      });
   BOOST_CHECK(wrong.diagnostics().sessions.empty());
   if (const auto row = wrong.peers().find(expected)) BOOST_CHECK(row->endpoints.empty());
   auto restricted = p2p::node{runtime, addressing_options("hint-private-circuit", true)};
   auto circuit = *server.local_endpoint();
   circuit.peer = server.local_peer();
   circuit.relayed = p2p::endpoint::circuit{expected};
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, restricted.async_open_protocol_stream(expected,
      p2p::builtins::ping, {.address_hints = {circuit.to_multiaddr()}})), p2p::exceptions::invalid_options);
   BOOST_CHECK_EQUAL(restricted.diagnostics().resources.active_dials, 0U);
   forge::asio::blocking::run(runtime, restricted.async_stop());
   forge::asio::blocking::run(runtime, wrong.async_stop());
   forge::asio::blocking::run(runtime, gated.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(operation_hints_cannot_crowd_an_independently_known_root_out_of_the_shared_bound) {
   auto runtime = forge::asio::runtime{};
   auto server = p2p::node{runtime, addressing_options("hint-known-server", false)};
   auto options = addressing_options("hint-known-client", false);
   options.dns_resolution.bounds.max_roots = 1;
   auto client = p2p::node{runtime, std::move(options)};
   forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   const auto known = *server.local_endpoint();
   client.peers().learn_endpoint(server.local_peer(), known);
   auto untrusted = known;
   untrusted.transport.port = 1;
   auto stream = forge::asio::blocking::run(runtime, client.async_open_protocol_stream(server.local_peer(),
      p2p::builtins::ping, {.allow_relay = false, .timeout = 2s, .allow_hole_punch = false,
         .address_hints = {untrusted.to_multiaddr()}}));
   BOOST_CHECK(stream.authentication() != p2p::peer_authentication::unverified);
   BOOST_CHECK_EQUAL(client.metrics().direct_failures, 0U);
   forge::asio::blocking::run(runtime, stream.async_close());
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(operation_hints_pin_open_identity_even_in_explicit_insecure_memory_mode) {
   auto runtime = forge::asio::runtime{};
   auto server = p2p::node{runtime, addressing_options("hint-memory-server", false)};
   auto options = addressing_options("hint-memory-client", false);
   options.allow_insecure_test_mode = true;
   options.stream_security = p2p::node::stream_security::noise;
   auto client = p2p::node{runtime, std::move(options)};
   forge::asio::blocking::run(runtime, server.async_listen(p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")));
   const auto expected = p2p::make_peer_id({.type = p2p::public_key::type::ed25519,
      .data = std::vector<std::uint8_t>(32, 76)});
   auto hint = *server.local_endpoint();
   hint.peer = expected;
   BOOST_CHECK_EXCEPTION(forge::asio::blocking::run(runtime, client.async_open_protocol_stream(expected,
      p2p::builtins::ping, {.allow_relay = false, .timeout = 2s, .allow_hole_punch = false,
         .address_hints = {hint.to_multiaddr()}})), forge::exceptions::base, [](const auto& error) {
         return p2p::exceptions::code_of(error) == p2p::exceptions::code::peer_verification_failed;
      });
   BOOST_CHECK(!client.peers().find(expected));
   // Reject misattribution, not the independently authenticated connection.
   BOOST_CHECK_NO_THROW(static_cast<void>(forge::asio::blocking::run(runtime, client.async_ping(server.local_peer()))));
   forge::asio::blocking::run(runtime, client.async_stop());
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(operation_hints_direct_deadline_and_cancellation_join_native_handshake) {
   for (const auto cancel : {false, true}) {
      auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
      auto client = p2p::node{runtime, addressing_options("hint-cancel-client", false)};
      auto identity = p2p::node{runtime, addressing_options("hint-stalled-identity", false)};
      auto stalled = stalled_ipv6_peer{runtime, 0};
      const auto hint = forge::multiformats::multiaddr::parse("/ip6/::1/tcp/" + std::to_string(stalled.port()) +
         "/p2p/" + identity.local_peer().to_string());
      auto signal = boost::asio::cancellation_signal{};
      auto executor = boost::asio::make_strand(runtime.context());
      auto opened = boost::asio::co_spawn(executor, client.async_open_protocol_stream(identity.local_peer(),
         p2p::builtins::ping, {.allow_relay = false, .timeout = cancel ? 3s : 400ms,
            .allow_hole_punch = false, .address_hints = {hint}}),
         boost::asio::bind_cancellation_slot(signal.slot(), boost::asio::use_future));
      BOOST_REQUIRE(stalled.accepted.wait_for(2s) == std::future_status::ready);
      if (cancel) boost::asio::post(executor, [&] { signal.emit(boost::asio::cancellation_type::all); });
      BOOST_REQUIRE(opened.wait_for(2s) == std::future_status::ready);
      try { static_cast<void>(opened.get()); BOOST_FAIL("stalled hinted handshake unexpectedly opened"); }
      catch (const forge::exceptions::base& error) {
         BOOST_CHECK(p2p::exceptions::code_of(error) == (cancel ? p2p::exceptions::code::canceled : p2p::exceptions::code::timeout));
      }
      BOOST_REQUIRE(stalled.finished.wait_for(2s) == std::future_status::ready);
      BOOST_CHECK_GT(stalled.finished.get(), 0U);
      BOOST_CHECK_EQUAL(client.diagnostics().resources.active_dials, 0U);
      BOOST_CHECK_EQUAL(client.diagnostics().resources.transient.outbound_connections, 0U);
      BOOST_CHECK(!client.peers().find(identity.local_peer()));
      forge::asio::blocking::run(runtime, client.async_stop());
      forge::asio::blocking::run(runtime, identity.async_stop());
   }
}

BOOST_AUTO_TEST_SUITE_END()
