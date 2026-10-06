#include <boost/test/unit_test.hpp>
#include "libp2p_identity_fixture.hxx"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.exceptions;
import forge.net.p2p.connection_gater;
import forge.net.p2p.diagnostics;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.net.p2p.private_network;
import forge.net.pnet.protector;
import forge.net.tcp.listener;
import forge.net.tcp.connector;
import forge.net.tcp.connection;
import forge.net.tls.options;
import forge.net.transport.endpoint;
import forge.net.yamux.options;

namespace {
namespace p2p = forge::net::p2p;
namespace asio = boost::asio;
using namespace std::chrono_literals;
using coordinated_options = p2p::node::coordinated_connect_options;

class dial_observer final : public p2p::connection_gater {
 public:
   std::atomic_size_t entered = 0;
   bool allow = true;
   bool intercept_peer_dial(const p2p::peer_id&) noexcept override {
      entered.fetch_add(1, std::memory_order_release);
      return allow;
   }
};

p2p::node::options native_options(std::string_view name, std::shared_ptr<dial_observer> observer = {}) {
   const auto identity = forge::tests::p2p::make_identity_fixture(name);
   auto options = p2p::node::options{
       .certificate_pem = identity.certificate_pem, .private_key_pem = identity.private_key_pem,
       .connection_gater = std::move(observer)};
   options.peer_state.persistence = p2p::peer_store::make_memory_persistence();
   return options;
}

bool eventually(const auto& ready) {
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   do {
      if (ready()) { return true; }
      std::this_thread::sleep_for(1ms);
   } while (std::chrono::steady_clock::now() < deadline);
   return ready();
}

p2p::endpoint listen(forge::asio::runtime& runtime, p2p::node& node, std::string_view protocol = "tcp") {
   forge::asio::blocking::run(runtime, node.async_listen(
       p2p::parse_endpoint(std::string{"/ip4/127.0.0.1/"} + std::string{protocol} + "/0" +
                           (protocol == "udp" ? "/quic-v1" : ""))));
   return node.local_endpoints().front();
}

void require_fact(const p2p::node& node, const p2p::peer_id& peer, const p2p::endpoint& local,
                  const p2p::endpoint& remote, p2p::diagnostics::session_direction direction) {
   const auto snapshot = node.diagnostics();
   BOOST_REQUIRE_EQUAL(snapshot.sessions.size(), 1U);
   const auto& session = snapshot.sessions.front();
   BOOST_CHECK(session.remote_peer == peer);
   BOOST_REQUIRE(session.local_endpoint);
   BOOST_REQUIRE(session.remote_endpoint);
   BOOST_TEST(session.local_endpoint->transport.port == local.transport.port);
   BOOST_TEST(session.remote_endpoint->transport.port == remote.transport.port);
   BOOST_CHECK(session.direction == direction);
   BOOST_CHECK(session.authentication != p2p::peer_authentication::unverified);
}

void check_late_tcp_upgrade_roles(bool private_profile) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto observed = std::make_shared<dial_observer>();
   auto source_options = native_options("coordinated-late-tcp-source", observed);
   auto target_options = native_options("coordinated-late-tcp-target");
   if (private_profile) {
      const auto key = std::array<std::uint8_t, forge::net::pnet::pre_shared_key_size>{};
      const auto protector = std::make_shared<const forge::net::pnet::protector>(forge::net::pnet::pre_shared_key{key});
      for (auto* options : {&source_options, &target_options}) {
         options->capabilities.bits = p2p::capabilities::peer_exchange;
         options->relay_policy.service_enabled = false;
         options->relay_policy.client_enabled = false;
         options->relay_policy.auto_discovery_enabled = false;
         options->path_policy.allow_relay = false;
         options->path_policy.allow_hole_punch = false;
         options->private_network = p2p::private_network::options{.protector = protector};
      }
   }
   auto source = p2p::node{runtime, std::move(source_options)};
   auto target = p2p::node{runtime, std::move(target_options)};
   const auto local = listen(runtime, source);
   auto reservation = forge::net::tcp::listener{runtime.context().get_executor(),
       forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
                                      .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                      .host = "127.0.0.1"}};
   const auto remote = p2p::endpoint{.transport = reservation.local_endpoint()};
   forge::asio::blocking::run(runtime, reservation.async_close());
   auto waiting = asio::co_spawn(runtime.context(), source.async_connect_coordinated(remote,
       {.expected_peer = target.local_peer(), .local_source = local, .timeout = 3s}), asio::use_future);
   BOOST_REQUIRE(eventually([&] {
      return observed->entered.load(std::memory_order_acquire) != 0 &&
             source.diagnostics().resources.transient.outbound_connections == 0;
   }));
   std::this_thread::sleep_for(200ms);
   BOOST_CHECK(waiting.wait_for(0ms) != std::future_status::ready);
   // No listener existed for the first SYN. A later connect arrives on the
   // source's accept side but must still negotiate security as the initiator.
   forge::asio::blocking::run(runtime, target.async_listen(remote));
   const auto result = forge::asio::blocking::run(runtime, target.async_connect_coordinated(local,
       {.expected_peer = source.local_peer(), .local_source = remote,
        .side = coordinated_options::role::responder, .timeout = 3s}));
   BOOST_REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
   const auto incoming = waiting.get();
   BOOST_CHECK(incoming.remote_peer == target.local_peer());
   BOOST_CHECK(result.remote_peer == source.local_peer());
   BOOST_REQUIRE(incoming.security_role);
   BOOST_REQUIRE(incoming.yamux_role);
   BOOST_REQUIRE(result.security_role);
   BOOST_REQUIRE(result.yamux_role);
   BOOST_CHECK(*incoming.security_role == forge::net::tls::endpoint_role::client);
   BOOST_CHECK(*incoming.yamux_role == forge::net::yamux::side::initiator);
   BOOST_CHECK(*result.security_role == forge::net::tls::endpoint_role::server);
   BOOST_CHECK(*result.yamux_role == forge::net::yamux::side::responder);
   require_fact(source, target.local_peer(), local, remote, p2p::diagnostics::session_direction::inbound);
   require_fact(target, source.local_peer(), remote, local, p2p::diagnostics::session_direction::outbound);
   const auto source_snapshot = source.diagnostics();
   const auto target_snapshot = target.diagnostics();
   BOOST_CHECK(source_snapshot.sessions.front().security_role == incoming.security_role);
   BOOST_CHECK(source_snapshot.sessions.front().yamux_role == incoming.yamux_role);
   BOOST_CHECK(target_snapshot.sessions.front().security_role == result.security_role);
   BOOST_CHECK(target_snapshot.sessions.front().yamux_role == result.yamux_role);
   // Both nodes prefer TLS before Noise. PNET protects the transport but does
   // not change that security preference; this case does not prove Noise.
   const auto authentication = p2p::peer_authentication::libp2p_tls;
   BOOST_CHECK(source_snapshot.sessions.front().authentication == authentication);
   BOOST_CHECK(target_snapshot.sessions.front().authentication == authentication);
   forge::asio::blocking::run(runtime, source.async_ping(target.local_peer()));
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, target.async_stop());
}

} // namespace

BOOST_AUTO_TEST_CASE(coordinated_node_tcp_uses_real_listener_source_and_one_logical_permit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto options = native_options("coordinated-one-permit-source");
   options.limits.resources.max_dial_attempts = 1;
   auto source = p2p::node{runtime, std::move(options)};
   auto target = p2p::node{runtime, native_options("coordinated-one-permit-target")};
   const auto local = listen(runtime, source);
   const auto remote = listen(runtime, target);
   const auto result = forge::asio::blocking::run(runtime, source.async_connect_coordinated(remote,
       {.expected_peer = target.local_peer(), .local_source = local, .timeout = 3s}));
   BOOST_CHECK(result.remote_peer == target.local_peer());
   BOOST_CHECK(result.muxer == p2p::protocol_id{.value = "/yamux/1.0.0"});
   require_fact(source, target.local_peer(), local, remote, p2p::diagnostics::session_direction::outbound);
   BOOST_TEST(source.diagnostics().resources.active_dials == 0U);
   forge::asio::blocking::run(runtime, source.async_ping(target.local_peer()));
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, target.async_stop());
}

BOOST_AUTO_TEST_CASE(coordinated_node_quic_responder_waits_exact_authenticated_listener_inbound) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto observed = std::make_shared<dial_observer>();
   auto responder = p2p::node{runtime, native_options("coordinated-quic-responder", observed)};
   auto initiator = p2p::node{runtime, native_options("coordinated-quic-initiator")};
   const auto server = listen(runtime, responder, "udp");
   const auto client = listen(runtime, initiator, "udp");
   auto waiting = asio::co_spawn(runtime.context(), responder.async_connect_coordinated(client,
       {.expected_peer = initiator.local_peer(), .local_source = server,
        .side = coordinated_options::role::responder, .timeout = 3s}), asio::use_future);
   // The production peer-dial hook runs in the worker after synchronous lease
   // installation. It observes readiness; it does not change transport roles.
   BOOST_REQUIRE(eventually([&] { return observed->entered.load(std::memory_order_acquire) != 0; }));
   const auto connected = forge::asio::blocking::run(runtime, initiator.async_connect_coordinated(server,
       {.expected_peer = responder.local_peer(), .local_source = client, .timeout = 3s}));
   BOOST_REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK(waiting.get().remote_peer == initiator.local_peer());
   BOOST_CHECK(connected.remote_peer == responder.local_peer());
   require_fact(responder, initiator.local_peer(), server, client, p2p::diagnostics::session_direction::inbound);
   require_fact(initiator, responder.local_peer(), client, server, p2p::diagnostics::session_direction::outbound);
   forge::asio::blocking::run(runtime, initiator.async_ping(responder.local_peer()));
   forge::asio::blocking::run(runtime, initiator.async_stop());
   forge::asio::blocking::run(runtime, responder.async_stop());
}

BOOST_AUTO_TEST_CASE(coordinated_node_late_tcp_inbound_keeps_initiator_security_role_after_outgoing_refusal) {
   check_late_tcp_upgrade_roles(false);
}

BOOST_AUTO_TEST_CASE(coordinated_node_private_tcp_outbound_keeps_native_tls_and_yamux_responder_facts) {
   check_late_tcp_upgrade_roles(true);
}

BOOST_AUTO_TEST_CASE(coordinated_node_caller_cancel_drains_exact_late_tcp_inbound_security_worker) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto observed = std::make_shared<dial_observer>();
   auto source = p2p::node{runtime, native_options("coordinated-cancel-inbound", observed)};
   const auto identity = forge::tests::p2p::make_identity_fixture("coordinated-cancel-inbound-peer");
   const auto expected = p2p::make_peer_id_from_certificate_pem(identity.certificate_pem);
   const auto local = listen(runtime, source);
   auto reservation = forge::net::tcp::listener{runtime.context().get_executor(),
       forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
           .protocol = forge::net::transport::endpoint::protocol_kind::tcp, .host = "127.0.0.1"}};
   const auto remote = reservation.local_endpoint();
   forge::asio::blocking::run(runtime, reservation.async_close());
   const auto baseline = source.diagnostics().resources.system.file_descriptors;
   const auto strand = asio::make_strand(runtime.context());
   auto signal = asio::cancellation_signal{};
   auto waiting = asio::co_spawn(strand, source.async_connect_coordinated(p2p::endpoint{.transport = remote},
       {.expected_peer = expected, .local_source = local, .timeout = 10s}),
       asio::bind_cancellation_slot(signal.slot(), asio::use_future));
   BOOST_REQUIRE(eventually([&] {
      return observed->entered.load(std::memory_order_acquire) != 0 &&
          source.diagnostics().resources.transient.outbound_connections == 0;
   }));
   // The outgoing socket was refused. A real accepted socket now owns the
   // exact leased tuple and stalls in the security handshake, not a fake session.
   auto raw = forge::net::tcp::listener{runtime.context().get_executor(), remote, {}, {.reuse_port = true}};
   auto connector = raw.make_coordinated_connector(remote);
   auto native = forge::asio::blocking::run(runtime, connector.async_connect_connection(local.transport));
   BOOST_REQUIRE(eventually([&] { return source.diagnostics().resources.transient.inbound_connections == 1; }));
   BOOST_CHECK(waiting.wait_for(0ms) != std::future_status::ready);
   asio::post(strand, [&signal] { signal.emit(asio::cancellation_type::total); });
   BOOST_REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
   try { static_cast<void>(waiting.get()); BOOST_FAIL("canceled inbound handshake published a session"); }
   catch (const forge::exceptions::base& error) {
      const auto code = p2p::exceptions::code_of(error);
      BOOST_REQUIRE(code);
      BOOST_CHECK(*code == p2p::exceptions::code::canceled);
   }
   const auto drained = source.diagnostics();
   BOOST_TEST(drained.resources.active_dials == 0U);
   BOOST_TEST(drained.resources.transient.inbound_connections == 0U);
   BOOST_TEST(drained.resources.transient.outbound_connections == 0U);
   BOOST_TEST(drained.resources.system.file_descriptors == baseline);
   BOOST_TEST(drained.sessions.empty());
   forge::asio::blocking::run(runtime, native.async_close());
   forge::asio::blocking::run(runtime, connector.async_stop());
   forge::asio::blocking::run(runtime, raw.async_close());
   forge::asio::blocking::run(runtime, source.async_stop());
}

BOOST_AUTO_TEST_CASE(coordinated_node_caller_cancel_drains_native_dial_not_unrelated_same_peer_session) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto source = p2p::node{runtime, native_options("coordinated-cancel-source")};
   auto target = p2p::node{runtime, native_options("coordinated-cancel-target")};
   const auto local = listen(runtime, source);
   auto blackhole = forge::net::tcp::listener{runtime.context().get_executor(),
       forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
                                      .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                      .host = "127.0.0.1"}};
   const auto remote = p2p::endpoint{.transport = blackhole.local_endpoint()};
   const auto strand = asio::make_strand(runtime.context());
   auto signal = asio::cancellation_signal{};
   auto waiting = asio::co_spawn(strand, source.async_connect_coordinated(remote,
       {.expected_peer = target.local_peer(), .local_source = local, .timeout = 10s}),
       asio::bind_cancellation_slot(signal.slot(), asio::use_future));
   auto native = forge::asio::blocking::run(runtime, blackhole.async_accept_connection());
   BOOST_TEST(source.diagnostics().sessions.empty());
   static_cast<void>(forge::asio::blocking::run(runtime, target.async_connect(local,
       {.expected_peer = source.local_peer(), .allow_relay = false, .timeout = 3s, .allow_hole_punch = false})));
   const auto unrelated = source.diagnostics();
   BOOST_REQUIRE_EQUAL(unrelated.sessions.size(), 1U);
   BOOST_REQUIRE(unrelated.sessions.front().security_role);
   BOOST_REQUIRE(unrelated.sessions.front().yamux_role);
   BOOST_CHECK(*unrelated.sessions.front().security_role == forge::net::tls::endpoint_role::server);
   BOOST_CHECK(*unrelated.sessions.front().yamux_role == forge::net::yamux::side::responder);
   BOOST_CHECK(waiting.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(source.diagnostics().resources.active_dials == 1U);
   asio::post(strand, [&signal] { signal.emit(asio::cancellation_type::total); });
   BOOST_REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(waiting.get()), forge::exceptions::base);
   BOOST_TEST(source.diagnostics().resources.active_dials == 0U);
   const auto retained = source.diagnostics();
   BOOST_REQUIRE_EQUAL(retained.sessions.size(), 1U);
   BOOST_TEST(retained.sessions.front().id == unrelated.sessions.front().id);
   BOOST_CHECK(retained.sessions.front().security_role == unrelated.sessions.front().security_role);
   BOOST_CHECK(retained.sessions.front().yamux_role == unrelated.sessions.front().yamux_role);
   BOOST_CHECK_NO_THROW(forge::asio::blocking::run(runtime, source.async_ping(target.local_peer())));
   forge::asio::blocking::run(runtime, native.async_close());
   forge::asio::blocking::run(runtime, blackhole.async_close());
   forge::asio::blocking::run(runtime, source.async_stop());
   forge::asio::blocking::run(runtime, target.async_stop());
}

BOOST_AUTO_TEST_CASE(coordinated_node_rejects_tuple_conflicts_capacity_nine_and_joins_eight_on_shutdown) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto observed = std::make_shared<dial_observer>();
   observed->allow = false;
   auto source = p2p::node{runtime, native_options("coordinated-bounds-source", observed)};
   const auto identity = forge::tests::p2p::make_identity_fixture("coordinated-bounds-peer");
   const auto expected = p2p::make_peer_id_from_certificate_pem(identity.certificate_pem);
   const auto local = listen(runtime, source);
   auto waiting = std::vector<std::future<p2p::node::session_info>>{};
   auto remote = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/40000");
   for (std::size_t index = 0; index != 8; ++index) {
      remote.transport.port = static_cast<std::uint16_t>(40000 + index);
      waiting.push_back(asio::co_spawn(runtime.context(), source.async_connect_coordinated(remote,
          {.expected_peer = expected, .local_source = local, .timeout = 10s}), asio::use_future));
   }
   BOOST_REQUIRE(eventually([&] { return observed->entered.load() == 8; }));
   BOOST_TEST(source.diagnostics().resources.active_dials == 8U);
   const auto rejected = [&](p2p::endpoint endpoint, p2p::exceptions::code expected_code) {
      try {
         static_cast<void>(forge::asio::blocking::run(runtime, source.async_connect_coordinated(endpoint,
             {.expected_peer = expected, .local_source = local, .timeout = 1s})));
         BOOST_FAIL("conflicting/over-capacity coordinated operation was accepted");
      } catch (const forge::exceptions::base& error) {
         const auto code = p2p::exceptions::code_of(error);
         BOOST_REQUIRE(code);
         BOOST_CHECK(*code == expected_code);
      }
   };
   rejected(remote, p2p::exceptions::code::invalid_options);
   remote.transport.port = 40008;
   rejected(remote, p2p::exceptions::code::backpressure_rejected);
   forge::asio::blocking::run(runtime, source.async_stop());
   for (auto& result : waiting) {
      BOOST_REQUIRE(result.wait_for(0ms) == std::future_status::ready);
      BOOST_CHECK_THROW(static_cast<void>(result.get()), forge::exceptions::base);
   }
   BOOST_TEST(source.diagnostics().resources.active_dials == 0U);
}

BOOST_AUTO_TEST_CASE(coordinated_node_deadline_cancels_actual_security_stream_and_keeps_listener) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto source = p2p::node{runtime, native_options("coordinated-deadline-source")};
   const auto identity = forge::tests::p2p::make_identity_fixture("coordinated-deadline-peer");
   const auto expected = p2p::make_peer_id_from_certificate_pem(identity.certificate_pem);
   const auto local = listen(runtime, source);
   auto blackhole = forge::net::tcp::listener{runtime.context().get_executor(),
       forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
                                      .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                      .host = "127.0.0.1"}};
   const auto baseline = source.diagnostics().resources.system.file_descriptors;
   auto waiting = asio::co_spawn(runtime.context(), source.async_connect_coordinated(
       p2p::endpoint{.transport = blackhole.local_endpoint()},
       {.expected_peer = expected, .local_source = local, .timeout = 300ms}), asio::use_future);
   auto native = forge::asio::blocking::run(runtime, blackhole.async_accept_connection());
   BOOST_TEST(source.diagnostics().sessions.empty());
   BOOST_REQUIRE(waiting.wait_for(2s) == std::future_status::ready);
   try { static_cast<void>(waiting.get()); BOOST_FAIL("native handshake should reach its overall deadline"); }
   catch (const forge::exceptions::base& error) {
      const auto code = p2p::exceptions::code_of(error);
      BOOST_REQUIRE(code);
      BOOST_CHECK(*code == p2p::exceptions::code::timeout);
   }
   BOOST_TEST(source.diagnostics().resources.active_dials == 0U);
   BOOST_TEST(source.diagnostics().resources.system.file_descriptors == baseline);
   BOOST_TEST(source.diagnostics().sessions.empty());
   BOOST_REQUIRE_EQUAL(source.local_endpoints().size(), 1U);
   forge::asio::blocking::run(runtime, native.async_close());
   forge::asio::blocking::run(runtime, blackhole.async_close());
   forge::asio::blocking::run(runtime, source.async_stop());
}

BOOST_AUTO_TEST_CASE(coordinated_node_rejects_missing_identity_dns_wildcard_wrong_source_and_circuit) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto source = p2p::node{runtime, native_options("coordinated-validation-source")};
   const auto identity = forge::tests::p2p::make_identity_fixture("coordinated-validation-peer");
   const auto expected = p2p::make_peer_id_from_certificate_pem(identity.certificate_pem);
   const auto local = listen(runtime, source);
   const auto remote = p2p::parse_endpoint("/ip4/127.0.0.1/tcp/40001");
   const auto rejected = [&](p2p::endpoint address, coordinated_options options) {
      BOOST_CHECK_THROW(static_cast<void>(forge::asio::blocking::run(runtime,
          source.async_connect_coordinated(std::move(address), std::move(options)))), forge::exceptions::base);
      BOOST_TEST(source.diagnostics().resources.active_dials == 0U);
   };
   rejected(remote, {.local_source = local});
   rejected(p2p::parse_endpoint("/dns4/localhost/tcp/40001"), {.expected_peer = expected, .local_source = local});
   auto wildcard = local;
   wildcard.transport.host = "0.0.0.0";
   rejected(remote, {.expected_peer = expected, .local_source = wildcard});
   auto foreign = local;
   foreign.transport.port = static_cast<std::uint16_t>(local.transport.port == 65535 ? 65534 : local.transport.port + 1);
   rejected(remote, {.expected_peer = expected, .local_source = foreign});
   auto circuit = remote;
   circuit.relayed = p2p::endpoint::circuit{.target = expected};
   rejected(circuit, {.expected_peer = expected, .local_source = local});
   auto wrong_peer = remote;
   wrong_peer.peer = source.local_peer();
   rejected(wrong_peer, {.expected_peer = expected, .local_source = local});
   rejected(remote, {.expected_peer = expected, .local_source = local, .timeout = 0ms});
   forge::asio::blocking::run(runtime, source.async_stop());
}
