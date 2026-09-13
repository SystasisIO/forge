module;

#include <boost/test/unit_test.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/use_future.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.runtime;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.yamux.exceptions;

#include "../../libraries/net/p2p/details/length_delimited.hxx"

namespace {
namespace asio = boost::asio;
namespace p2p = forge::net::p2p;
using namespace std::chrono_literals;

template <typename T> T bounded(forge::asio::runtime& runtime, asio::awaitable<T> operation) {
   auto future = asio::co_spawn(runtime.context(), std::move(operation), asio::use_future);
   BOOST_REQUIRE(future.wait_for(5s) == std::future_status::ready);
   return future.get();
}

struct dial_counter final : p2p::connection_gater {
   std::atomic_size_t peers{0};
   std::atomic_size_t addresses{0};
   bool intercept_peer_dial(const p2p::peer_id&) noexcept override {
      ++peers;
      return true;
   }
   bool intercept_address_dial(const p2p::peer_id&, const p2p::endpoint& address) noexcept override {
      ++addresses;
      // Fail closed even if a regression dials before receiving challenge data.
      return address.transport.host != "8.8.8.8";
   }
};

p2p::node::options options_for(const std::string& name, bool service) {
   auto identity = forge::tests::p2p::make_identity_fixture(name);
   auto options = p2p::node::options{};
   options.certificate_pem = std::move(identity.certificate_pem);
   options.private_key_pem = std::move(identity.private_key_pem);
   options.allow_insecure_test_mode = false;
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   options.limits.topology.operating_mode = p2p::topology::mode::static_only;
   options.reachability_policy.client_v1_enabled = false;
   options.reachability_policy.client_v2_enabled = false;
   options.reachability_policy.ping_enabled = false;
   options.reachability_policy.service_v1_enabled = service;
   options.reachability_policy.service_v2_enabled = service;
   options.reachability_policy.timeout = 10s;
   options.lifecycle.listen = {p2p::parse_endpoint("/ip4/127.0.0.1/tcp/0")};
   return options;
}

struct service_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   std::shared_ptr<dial_counter> dials = std::make_shared<dial_counter>();
   p2p::node client{runtime, options_for("autonat-service-client", false)};
   p2p::node service;

   explicit service_fixture(bool enabled = true, std::size_t pending = 4)
       : service(runtime, service_options(enabled, pending)) {
      static_cast<void>(bounded(runtime, client.async_start()));
      static_cast<void>(bounded(runtime, service.async_start()));
      static_cast<void>(bounded(runtime, client.async_connect(service.local_endpoints().front())));
   }

   p2p::node::options service_options(bool enabled, std::size_t pending) {
      auto options = options_for("autonat-service-server", enabled);
      options.connection_gater = dials;
      options.reachability_policy.max_pending_probes = pending;
      return options;
   }

   ~service_fixture() {
      try {
         bounded(runtime, client.async_stop());
         bounded(runtime, service.async_stop());
      } catch (...) {
         runtime.stop();
         BOOST_ERROR("AutoNAT service fixture failed bounded cleanup");
      }
   }

   p2p::stream open(const p2p::protocol_id& protocol) {
      auto result = bounded(runtime, client.async_open_protocol_stream(service.local_peer(), protocol));
      BOOST_CHECK(result.authentication() != p2p::peer_authentication::unverified);
      return result;
   }

   void no_dial() {
      BOOST_TEST(dials->peers.load() == 0U);
      BOOST_TEST(dials->addresses.load() == 0U);
      BOOST_TEST(service.diagnostics().resources.active_dials == 0U);
      BOOST_CHECK(service.reachability_status().effective == p2p::reachability::state::unknown);
   }
};

asio::awaitable<std::vector<std::uint8_t>> read_message(p2p::stream& channel) {
   auto buffer = std::vector<std::uint8_t>{};
   co_return co_await p2p::async_read_length_delimited(channel, buffer, 4104);
}

std::vector<std::uint8_t> v1_request(p2p::peer_id peer, std::vector<p2p::endpoint> addresses) {
   return p2p::reachability::codec::encode_v1({
       .kind = p2p::reachability::message::message_kind::dial,
       .peer = p2p::reachability::peer_info{.peer = std::move(peer), .endpoints = std::move(addresses)}});
}

std::vector<std::uint8_t> v2_request(std::vector<p2p::endpoint> addresses) {
   return p2p::reachability::codec::encode_v2({
       .type = p2p::reachability::v2::message::kind::dial_request,
       .dial_request = p2p::reachability::v2::dial_request{.endpoints = std::move(addresses), .nonce = 42}});
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_node_autonat_service)

BOOST_AUTO_TEST_CASE(service_disabled_rejects_actual_protocol_streams) {
   auto fixture = service_fixture{false};
   for (const auto& protocol : {p2p::builtins::autonat_v1, p2p::builtins::autonat_v2_dial_request}) {
      BOOST_CHECK_THROW(static_cast<void>(fixture.open(protocol)), p2p::exceptions::unsupported_protocol);
   }
   fixture.no_dial();
}

BOOST_AUTO_TEST_CASE(authenticated_loopback_service_refuses_nonpublic_targets_without_dialing) {
   const auto targets = std::vector<p2p::endpoint>{
       p2p::parse_endpoint("/ip4/127.0.0.1/tcp/4001"),
       p2p::parse_endpoint("/ip4/10.0.0.1/tcp/4001"),
       p2p::parse_endpoint("/ip4/169.254.169.254/tcp/80"),
       p2p::parse_endpoint("/ip4/0.0.0.0/tcp/4001"),
       p2p::parse_endpoint("/ip6/::1/tcp/4001"),
       p2p::parse_endpoint("/ip6/64:ff9b::a00:1/tcp/4001")};
   for (const auto v2 : {false, true}) {
      auto fixture = service_fixture{};
      auto channel = fixture.open(v2 ? p2p::builtins::autonat_v2_dial_request : p2p::builtins::autonat_v1);
      const auto request = v2 ? v2_request(targets) : v1_request(fixture.client.local_peer(), targets);
      bounded(fixture.runtime, channel.async_write(std::span<const std::uint8_t>{request}));
      const auto bytes = bounded(fixture.runtime, read_message(channel));
      if (v2) {
         const auto response = p2p::reachability::codec::decode_v2(bytes);
         BOOST_REQUIRE(response.dial_response);
         BOOST_CHECK(response.dial_response->status == p2p::reachability::v2::response_status::dial_refused);
      } else {
         const auto response = p2p::reachability::codec::decode_v1(bytes);
         BOOST_REQUIRE(response.response);
         BOOST_CHECK(response.response->status == p2p::reachability::dial_status::dial_refused);
         BOOST_TEST(!response.response->endpoint.has_value());
      }
      bounded(fixture.runtime, channel.async_close());
      // These are refusal controls, never proof of Internet reachability.
      fixture.no_dial();
   }
}

BOOST_AUTO_TEST_CASE(v1_rejects_peer_identity_different_from_authenticated_caller) {
   auto fixture = service_fixture{};
   auto channel = fixture.open(p2p::builtins::autonat_v1);
   BOOST_CHECK(fixture.client.local_peer() != fixture.service.local_peer());
   const auto request = v1_request(fixture.service.local_peer(), fixture.client.local_endpoints());
   bounded(fixture.runtime, channel.async_write(std::span<const std::uint8_t>{request}));
   const auto response = p2p::reachability::codec::decode_v1(bounded(fixture.runtime, read_message(channel)));
   BOOST_REQUIRE(response.response);
   BOOST_CHECK(response.response->status == p2p::reachability::dial_status::bad_request);
   BOOST_TEST(!response.response->endpoint.has_value());
   bounded(fixture.runtime, channel.async_close());
   fixture.no_dial();
}

BOOST_AUTO_TEST_CASE(v2_pending_bound_rejects_second_peer_and_cancel_never_reaches_dial) {
   auto fixture = service_fixture{true, 1};
   auto pending = fixture.open(p2p::builtins::autonat_v2_dial_request);
   // No external traffic: without the demanded dial data the service cannot
   // reach probe_autonat. The actual response proves admission before overflow.
   const auto request = v2_request({p2p::parse_endpoint("/ip4/8.8.8.8/tcp/4001")});
   bounded(fixture.runtime, pending.async_write(std::span<const std::uint8_t>{request}));
   const auto challenge = p2p::reachability::codec::decode_v2(bounded(fixture.runtime, read_message(pending)));
   BOOST_REQUIRE(challenge.dial_data_request);
   BOOST_TEST(challenge.dial_data_request->bytes >= 30'000U);
   BOOST_TEST(challenge.dial_data_request->bytes <= 100'000U);
   fixture.no_dial();

   auto other = p2p::node{fixture.runtime, options_for("autonat-service-second-client", false)};
   static_cast<void>(bounded(fixture.runtime, other.async_start()));
   static_cast<void>(bounded(fixture.runtime, other.async_connect(fixture.service.local_endpoints().front())));
   auto overflow = bounded(fixture.runtime,
       other.async_open_protocol_stream(fixture.service.local_peer(), p2p::builtins::autonat_v2_dial_request));
   const auto rejected = p2p::reachability::codec::decode_v2(bounded(fixture.runtime, read_message(overflow)));
   BOOST_CHECK(rejected.type == p2p::reachability::v2::message::kind::dial_response);
   BOOST_REQUIRE(rejected.dial_response);
   BOOST_CHECK(rejected.dial_response->status == p2p::reachability::v2::response_status::request_rejected);
   BOOST_TEST(!rejected.dial_data_request.has_value());
   pending.request_cancel();
   try {
      bounded(fixture.runtime, pending.async_close());
   } catch (const forge::net::yamux::exceptions::stream_reset&) {
      // The deliberately canceled challenge stream may already be reset.
   }
   bounded(fixture.runtime, overflow.async_close());
   bounded(fixture.runtime, other.async_stop());
   bounded(fixture.runtime, fixture.service.async_stop());
   fixture.no_dial();
   const auto resources = fixture.service.diagnostics().resources;
   BOOST_TEST(resources.services.memory == 0U);
   BOOST_TEST(resources.streams.memory == 0U);
   BOOST_TEST(resources.system.inbound_streams == 0U);
   BOOST_TEST(resources.system.outbound_streams == 0U);
}

BOOST_AUTO_TEST_SUITE_END()
