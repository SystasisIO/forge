module;

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/scope/scope_exit.hpp>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.runtime;
import forge.crypto.asymmetric;
import forge.exceptions;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.private_network;
import forge.net.p2p.protocol;
import forge.net.p2p.stream;
import forge.net.pnet.protector;
import forge.net.tcp.connection;
import forge.net.tcp.connector;
import forge.net.tcp.listener;
import forge.net.tls.options;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/libp2p_identity_material.hxx"
#include "../../libraries/net/p2p/details/operation_deadline.hxx"
#include "../../libraries/net/p2p/details/stream_upgrade.hxx"

namespace forge::net::p2p {
namespace {

using security = node::stream_security;
using namespace std::chrono_literals;

node::options security_options(std::string_view name, security policy, bool private_profile) {
   const auto material = forge::tests::p2p::make_identity_fixture(name);
   auto options = node::options{.certificate_pem = material.certificate_pem,
      .private_key_pem = material.private_key_pem};
   options.stream_security = policy;
   options.peer_state.persistence = peer_store::make_memory_persistence();
   options.capabilities.bits = 0;
   options.relay_policy.client_enabled = false;
   options.relay_policy.auto_discovery_enabled = false;
   options.path_policy.allow_relay = false;
   options.path_policy.allow_hole_punch = false;
   if (private_profile) {
      const auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
      options.private_network = private_network::options{
         .protector = std::make_shared<const forge::net::pnet::protector>(forge::net::pnet::pre_shared_key{key})};
   }
   return options;
}

peer_id security_peer(const libp2p_identity_material& identity) {
   return make_peer_id(decode_public_key(identity.public_key));
}

void check_tcp_pair(security dial_policy, security accept_policy, bool private_profile, bool reversed_roles,
                    peer_authentication authentication,
                    std::optional<exceptions::code> failure = std::nullopt, bool wrong_peer = false) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto options = std::array{security_options("security-dial", dial_policy, private_profile),
      security_options("security-accept", accept_policy, private_profile)};
   const auto identities = std::array{make_libp2p_identity_material(options[0]),
      make_libp2p_identity_material(options[1])};
   const auto peers = std::array{security_peer(identities[0]), security_peer(identities[1])};
   auto expected = std::array{peers[1], peers[0]};
   const auto initiator = reversed_roles ? 1U : 0U;
   if (wrong_peer) {
      expected[initiator] = security_peer(make_libp2p_identity_material(
         security_options("security-wrong-peer", security::tls_and_noise, false)));
   }
   auto listener = forge::net::tcp::listener{runtime.context().get_executor(),
      parse_endpoint("/ip4/127.0.0.1/tcp/0").transport};
   auto connector = forge::net::tcp::connector{runtime.context().get_executor()};
   auto accepted = std::future<forge::net::tcp::connection>{};
   auto connections = std::array<forge::net::tcp::connection, 2>{};
   auto pending = std::array<std::future<upgraded_session>, 2>{};
   auto upgraded = std::array<upgraded_session, 2>{};
   auto errors = std::array<std::exception_ptr, 2>{};
   auto secured = std::array<std::atomic_size_t, 2>{};
   const auto cancellation = std::array{std::make_shared<cancellation_latch>(), std::make_shared<cancellation_latch>()};
   auto sent = forge::net::transport::stream{};
   auto received = forge::net::transport::stream{};
   auto accepting_stream = std::future<forge::net::transport::stream>{};
   auto deadline = operation_deadline{runtime.context(), 3s};
   deadline.arm([cancellation] { for (const auto& owner : cancellation) { owner->request_stop(); } });
   auto cleanup = boost::scope::scope_exit{[&] {
      deadline.cancel();
      for (const auto& owner : cancellation) { owner->request_stop(); }
      listener.close();
      if (accepted.valid()) { try { connections[1] = accepted.get(); } catch (...) {} }
      for (auto index = 0U; index < pending.size(); ++index) {
         if (pending[index].valid()) { try { upgraded[index] = pending[index].get(); } catch (...) {} }
      }
      for (const auto& value : upgraded) { if (value.session) { value.session->request_cancel(); } }
      if (accepting_stream.valid()) { try { received = accepting_stream.get(); } catch (...) {} }
      for (auto* value : {&sent, &received}) {
         value->request_cancel();
         try { forge::asio::blocking::run(runtime, value->async_close()); } catch (...) {}
      }
      for (const auto& value : upgraded) {
         if (value.session) { try { forge::asio::blocking::run(runtime, value.session->async_close()); } catch (...) {} }
      }
      for (auto& value : connections) {
         if (value.valid()) { try { forge::asio::blocking::run(runtime, value.async_close()); } catch (...) {} }
      }
      try { forge::asio::blocking::run(runtime, listener.async_close()); } catch (...) {}
      static_cast<void>(deadline.finish());
   }};
   accepted = boost::asio::co_spawn(runtime.context(), listener.async_accept_connection(), boost::asio::use_future);
   connections[0] = forge::asio::blocking::run(runtime, connector.async_connect_connection(listener.local_endpoint()));
   connections[1] = accepted.get();
   for (auto index = 0U; index < pending.size(); ++index) {
      const auto role = index == initiator ? upgrade_role::initiator : upgrade_role::responder;
      pending[index] = boost::asio::co_spawn(runtime.context(),
         upgrade_tcp(std::move(connections[index]), options[index], identities[index], expected[index], role,
            {.context = &runtime.context(), .timeout = 3s, .cancel_current = cancellation[index]},
            {.secured = [&, index](const peer_id&) { secured[index].fetch_add(1); }}), boost::asio::use_future);
   }
   for (auto index = 0U; index < pending.size(); ++index) {
      try { upgraded[index] = pending[index].get(); } catch (...) { errors[index] = std::current_exception(); }
   }
   if (failure) {
      BOOST_REQUIRE(errors[initiator]);
      auto matched = false;
      try { std::rethrow_exception(errors[initiator]); }
      catch (const forge::exceptions::base& error) { matched = exceptions::is(error, *failure); }
      BOOST_REQUIRE(matched);
      BOOST_TEST(secured[initiator].load() == 0U);
      BOOST_TEST(!upgraded[initiator].session);
      if (*failure == exceptions::code::unsupported_protocol) {
         BOOST_TEST(secured[1U - initiator].load() == 0U);
         BOOST_TEST(!upgraded[1U - initiator].session);
      }
      BOOST_TEST(!deadline.timed_out());
      return;
   }
   for (auto index = 0U; index < upgraded.size(); ++index) {
      if (errors[index]) { std::rethrow_exception(errors[index]); }
      const auto& value = upgraded[index];
      BOOST_REQUIRE(value.session);
      BOOST_CHECK(value.peer == expected[index]);
      BOOST_CHECK(value.authentication == authentication);
      BOOST_TEST(value.muxer.value == "/yamux/1.0.0");
      BOOST_REQUIRE(value.security_role);
      BOOST_REQUIRE(value.yamux_role);
      BOOST_REQUIRE(value.role);
      const auto role = index == initiator ? upgrade_role::initiator : upgrade_role::responder;
      BOOST_CHECK(*value.role == role);
      BOOST_CHECK(*value.security_role == (index == initiator ? forge::net::tls::endpoint_role::client
                                                             : forge::net::tls::endpoint_role::server));
      BOOST_CHECK(*value.yamux_role == (index == initiator ? forge::net::yamux::side::initiator
                                                          : forge::net::yamux::side::responder));
      BOOST_TEST(secured[index].load() == 1U);
      cancellation[index]->arm([session = value.session] { session->request_cancel(); });
   }
   accepting_stream = boost::asio::co_spawn(runtime.context(), upgraded[1U - initiator].session->async_accept_stream(),
      boost::asio::use_future);
   sent = forge::asio::blocking::run(runtime, upgraded[initiator].session->async_open_stream());
   const auto payload = std::array<std::uint8_t, 3>{1, 2, 3};
   forge::asio::blocking::run(runtime, sent.async_write(payload));
   received = accepting_stream.get();
   const auto actual = forge::asio::blocking::run(runtime, received.async_read());
   BOOST_CHECK_EQUAL_COLLECTIONS(actual.begin(), actual.end(), payload.begin(), payload.end());
   BOOST_TEST(deadline.finish());
   forge::asio::blocking::run(runtime, sent.async_close());
   forge::asio::blocking::run(runtime, received.async_close());
   for (const auto& value : upgraded) { forge::asio::blocking::run(runtime, value.session->async_close()); }
   forge::asio::blocking::run(runtime, listener.async_close());
   cleanup.set_active(false);
}

void check_public_connect(security policy, bool private_profile, bool quic, bool reverse) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto first_options = security_options("security-public-first", policy, private_profile);
   auto second_options = security_options("security-public-second", quic ? security::tls : policy, private_profile);
   if (quic) { first_options.capabilities.bits = second_options.capabilities.bits = capabilities::direct_quic; }
   auto first = node{runtime, std::move(first_options)};
   auto second = node{runtime, std::move(second_options)};
   auto stop = boost::scope::scope_exit{[&] {
      for (auto* owner : {&first, &second}) { try { forge::asio::blocking::run(runtime, owner->async_stop()); } catch (...) {} }
   }};
   for (auto* owner : {&first, &second}) {
      forge::asio::blocking::run(runtime, owner->async_listen(parse_endpoint(
         quic ? "/ip4/127.0.0.1/udp/0/quic-v1" : "/ip4/127.0.0.1/tcp/0")));
   }
   const auto authentication = quic ? peer_authentication::quic_tls
      : policy == security::noise ? peer_authentication::noise : peer_authentication::libp2p_tls;
   // Every direction gets a fresh pair: an existing preferred session cannot satisfy this dial.
   auto& caller = reverse ? second : first;
   auto& target = reverse ? first : second;
   const auto connected = forge::asio::blocking::run(runtime, caller.async_connect(target.local_endpoints().front(),
      {.expected_peer = target.local_peer(), .allow_relay = false, .timeout = 3s, .allow_hole_punch = false}));
   BOOST_CHECK(connected.remote_peer == target.local_peer());
   const auto native = caller.diagnostics();
   const auto admitted = std::ranges::find_if(native.sessions, [&](const auto& session) {
      return session.id == connected.id && session.remote_peer == target.local_peer();
   });
   BOOST_REQUIRE(admitted != native.sessions.end());
   BOOST_TEST(!admitted->closed);
   BOOST_CHECK(admitted->authentication == authentication);
   forge::asio::blocking::run(runtime, caller.async_ping(target.local_peer()));
   for (const auto* owner : {&first, &second}) {
      const auto snapshot = owner->diagnostics();
      BOOST_REQUIRE(!snapshot.sessions.empty());
      for (const auto& session : snapshot.sessions) { BOOST_CHECK(session.authentication == authentication); }
   }
   forge::asio::blocking::run(runtime, first.async_stop());
   forge::asio::blocking::run(runtime, second.async_stop());
   stop.set_active(false);
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_stream_security)

BOOST_AUTO_TEST_CASE(policy_lists_and_invalid_enum_are_checked_before_node_allocation) {
   const auto defaults = detail::stream_security_protocols(security::tls_and_noise);
   BOOST_REQUIRE_EQUAL(defaults.size(), 2U);
   BOOST_TEST(defaults[0].value == "/tls/1.0.0");
   BOOST_TEST(defaults[1].value == "/noise");
   BOOST_REQUIRE_EQUAL(detail::stream_security_protocols(security::tls).size(), 1U);
   BOOST_TEST(detail::stream_security_protocols(security::tls).front().value == "/tls/1.0.0");
   BOOST_REQUIRE_EQUAL(detail::stream_security_protocols(security::noise).size(), 1U);
   BOOST_TEST(detail::stream_security_protocols(security::noise).front().value == "/noise");
   auto runtime = forge::asio::runtime{};
   auto invalid = node::options{}; // No certificate or persistence: policy validation must precede their construction.
   invalid.stream_security = static_cast<security>(255);
   const auto invalid_options = [](const forge::exceptions::base& error) {
      return exceptions::is(error, exceptions::code::invalid_options);
   };
   BOOST_CHECK_EXCEPTION(validate(invalid), forge::exceptions::base, invalid_options);
   BOOST_CHECK_EXCEPTION(node(runtime, invalid), forge::exceptions::base, invalid_options);
}

BOOST_AUTO_TEST_CASE(tls_only_rejects_relay_client_but_allows_service_only_forwarding) {
   auto options = security_options("security-relay-service", security::tls, false);
   options.relay_policy.client_enabled = true;
   BOOST_CHECK_EXCEPTION(validate(options), forge::exceptions::base, [](const auto& error) {
      return exceptions::is(error, exceptions::code::invalid_options);
   });
   options.relay_policy.client_enabled = false;
   options.relay_policy.service_enabled = true;
   BOOST_CHECK_NO_THROW(validate(options));
}

BOOST_AUTO_TEST_CASE(relay_tls_guards_reject_before_touching_stream_io) {
   auto runtime = forge::asio::runtime{};
   const auto options = security_options("security-relay-guard", security::tls, false);
   const auto identity = make_libp2p_identity_material(options);
   const auto invalid_options = [](const forge::exceptions::base& error) {
      return exceptions::is(error, exceptions::code::invalid_options);
   };
   BOOST_CHECK_EXCEPTION(forge::asio::blocking::run(runtime,
      upgrade_outbound_stream(stream{}, options, identity, std::nullopt)), forge::exceptions::base, invalid_options);
   BOOST_CHECK_EXCEPTION(forge::asio::blocking::run(runtime,
      upgrade_inbound_stream(stream{}, options, identity, std::nullopt)), forge::exceptions::base, invalid_options);
}

BOOST_AUTO_TEST_CASE(native_tcp_and_pnet_authenticate_only_the_negotiated_allowed_security) {
   for (const auto private_profile : {false, true}) {
      for (const auto& [dial, accept, authentication] : std::array{
         std::tuple{security::tls_and_noise, security::tls_and_noise, peer_authentication::libp2p_tls},
         std::tuple{security::tls, security::tls, peer_authentication::libp2p_tls},
         std::tuple{security::noise, security::noise, peer_authentication::noise},
         std::tuple{security::tls_and_noise, security::tls, peer_authentication::libp2p_tls},
         std::tuple{security::tls, security::tls_and_noise, peer_authentication::libp2p_tls},
         std::tuple{security::tls_and_noise, security::noise, peer_authentication::noise},
         std::tuple{security::noise, security::tls_and_noise, peer_authentication::noise}}) {
         check_tcp_pair(dial, accept, private_profile, false, authentication);
      }
   }
}

BOOST_AUTO_TEST_CASE(native_tcp_and_pnet_disjoint_policies_fail_without_authenticated_receipts) {
   for (const auto private_profile : {false, true}) {
      for (const auto reversed : {false, true}) {
         check_tcp_pair(security::tls, security::noise, private_profile, reversed,
            peer_authentication::unverified, exceptions::code::unsupported_protocol);
      }
   }
}

BOOST_AUTO_TEST_CASE(coordinated_tcp_and_pnet_keep_security_responder_on_the_physical_dial_socket) {
   for (const auto private_profile : {false, true}) {
      check_tcp_pair(security::noise, security::noise, private_profile, true, peer_authentication::noise);
      check_tcp_pair(security::tls, security::tls, private_profile, true, peer_authentication::libp2p_tls);
   }
}

BOOST_AUTO_TEST_CASE(default_and_forced_noise_still_reject_wrong_expected_identity) {
   for (const auto private_profile : {false, true}) {
      check_tcp_pair(security::tls_and_noise, security::tls_and_noise, private_profile, false,
         peer_authentication::unverified, exceptions::code::peer_verification_failed, true);
      check_tcp_pair(security::noise, security::noise, private_profile, true,
         peer_authentication::unverified, exceptions::code::peer_verification_failed, true);
   }
}

BOOST_AUTO_TEST_CASE(public_node_options_reach_tcp_and_private_tcp_native_upgrades) {
   for (const auto private_profile : {false, true}) {
      for (const auto policy : {security::tls_and_noise, security::tls, security::noise}) {
         for (const auto reverse : {false, true}) { check_public_connect(policy, private_profile, false, reverse); }
      }
   }
}

BOOST_AUTO_TEST_CASE(noise_only_stream_policy_does_not_replace_quic_tls) {
   for (const auto reverse : {false, true}) { check_public_connect(security::noise, false, true, reverse); }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
