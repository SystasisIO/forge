#include <boost/test/unit_test.hpp>

#include "libp2p_identity_fixture.hxx"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.exceptions;
import forge.net.tcp.connector;
import forge.net.tcp.listener;
import forge.net.quic.connector;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.quic.security;
import forge.net.transport.endpoint;

#include "../../libraries/net/tcp/details/connector_access.hxx"

namespace {
namespace asio = boost::asio;
namespace tcp = forge::net::tcp;
namespace quic = forge::net::quic;
using udp = asio::ip::udp;
using namespace std::chrono_literals;
using namespace asio::experimental::awaitable_operators;

forge::net::transport::endpoint tcp_local(std::string host) {
   return {.host_type = host.find(':') == std::string::npos ? forge::net::transport::endpoint::host_kind::ip4
                                                            : forge::net::transport::endpoint::host_kind::ip6,
           .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
           .host = std::move(host)};
}

quic::server_options test_server_options() {
   const auto identity = forge::tests::p2p::make_identity_fixture("coordinated-transport");
   return {.certificate_pem = identity.certificate_pem, .private_key_pem = identity.private_key_pem};
}

quic::client_options pinned_options(const quic::server_options& server) {
   return {.connect_timeout = 2s,
           .handshake_timeout = 2s,
           .security = {.expected_sha256_fingerprint =
                            quic::certificate_sha256_fingerprint_from_pem(server.certificate_pem)}};
}

asio::awaitable<void> tcp_source_roundtrip(forge::asio::runtime& runtime, std::string host, bool reuse) {
   const auto executor = runtime.context().get_executor();
   auto source = tcp::listener{executor, tcp_local(host), {}, tcp::options{.reuse_port = reuse}};
   auto target = tcp::listener{executor, tcp_local(host)};
   auto source_endpoint = source.local_endpoint();
   auto connector = reuse ? source.make_coordinated_connector(source_endpoint) : tcp::connector{executor};
   auto [outbound, inbound] =
       co_await (connector.async_connect_connection(target.local_endpoint()) && target.async_accept_connection());
   BOOST_TEST(outbound.local_endpoint().host == source_endpoint.host);
   BOOST_TEST(inbound.remote_endpoint().host == source_endpoint.host);
   BOOST_TEST(outbound.local_endpoint().port == inbound.remote_endpoint().port);
   if (reuse) {
      BOOST_TEST(outbound.local_endpoint().port == source_endpoint.port);
   } else {
      BOOST_TEST(outbound.local_endpoint().port != source_endpoint.port);
   }
   co_await (outbound.async_close() && inbound.async_close());
   co_await connector.async_stop();
   co_await source.async_close();
   co_await target.async_close();
}

asio::awaitable<void> tcp_factory_rejections(forge::asio::runtime& runtime) {
   const auto executor = runtime.context().get_executor();
   auto ordinary = tcp::listener{executor, tcp_local("127.0.0.1")};
   BOOST_CHECK_THROW(static_cast<void>(ordinary.make_coordinated_connector(ordinary.local_endpoint())), forge::exceptions::base);
   auto source = tcp::listener{executor, tcp_local("0.0.0.0"), {}, tcp::options{.reuse_port = true}};
   const auto wildcard = source.local_endpoint();
   BOOST_CHECK_THROW(static_cast<void>(source.make_coordinated_connector(wildcard)), forge::exceptions::base);
   auto foreign = wildcard;
   foreign.host = "192.0.2.123";
   BOOST_CHECK_THROW(static_cast<void>(source.make_coordinated_connector(foreign)), forge::exceptions::base);
   auto concrete = wildcard;
   concrete.host = "127.0.0.1";
   auto connector = source.make_coordinated_connector(concrete);
   co_await source.async_close();
   BOOST_TEST(!connector.valid());
   BOOST_CHECK_THROW(static_cast<void>(source.make_coordinated_connector(concrete)), forge::exceptions::base);
   co_await connector.async_stop();
   co_await ordinary.async_close();
}

void tcp_failed_terminal_wait_drains(bool source_close_first) {
   auto context = asio::io_context{};
   auto source = tcp::listener{context.get_executor(), tcp_local("127.0.0.1"), {}, {.reuse_port = true}};
   auto target = tcp::listener{context.get_executor(), tcp_local("127.0.0.1")};
   auto connector = source.make_coordinated_connector(source.local_endpoint());
   auto entered = std::make_shared<forge::asio::notification>();
   auto released = std::make_shared<forge::asio::notification>();
   using access = tcp::detail::connector_access;
   access::hold_attempt_completion_for_test(connector, entered, released);
   auto lifetime = std::make_shared<int>(1);
   const auto weak = std::weak_ptr<int>{lifetime};
   auto dial = asio::co_spawn(context,
       connector.async_connect_connection(target.local_endpoint(), {}, std::move(lifetime)), asio::use_future);
   const auto drive_until = [&context](const auto& ready) {
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      while (!ready() && std::chrono::steady_clock::now() < deadline) {
         context.restart();
         context.run_for(1ms);
      }
      return ready();
   };
   BOOST_REQUIRE(drive_until([&] { return entered->epoch() != 0; }));
   BOOST_REQUIRE(access::pending_connects_for_test(connector) == 1U);
   BOOST_TEST(!weak.expired());
   BOOST_TEST(access::holds_source_for_test(connector));

   // Fail the real terminal wait continuation while the native socket and its
   // lifetime remain owned. The completion callback must not publish drain.
   access::fail_terminal_wait_for_test(connector);
   auto stopped = std::future<void>{};
   auto closed = std::future<void>{};
   if (source_close_first) {
      closed = asio::co_spawn(context, source.async_close(), asio::use_future);
      stopped = asio::co_spawn(context, connector.async_stop(), asio::use_future);
   } else {
      stopped = asio::co_spawn(context, connector.async_stop(), asio::use_future);
      closed = asio::co_spawn(context, source.async_close(), asio::use_future);
   }
   context.restart();
   context.poll();
   BOOST_CHECK(stopped.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(closed.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(dial.wait_for(0ms) != std::future_status::ready);
   BOOST_TEST(access::pending_connects_for_test(connector) == 1U);
   BOOST_TEST(!weak.expired());
   BOOST_TEST(access::holds_source_for_test(connector));

   released->notify();
   BOOST_REQUIRE(drive_until([&] {
      return stopped.wait_for(0ms) == std::future_status::ready &&
             closed.wait_for(0ms) == std::future_status::ready && dial.wait_for(0ms) == std::future_status::ready;
   }));
   BOOST_TEST(weak.expired());
   BOOST_TEST(access::pending_connects_for_test(connector) == 0U);
   BOOST_TEST(!access::holds_source_for_test(connector));
   BOOST_CHECK_THROW(stopped.get(), std::bad_alloc);
   BOOST_CHECK_NO_THROW(closed.get());
   BOOST_CHECK_THROW(static_cast<void>(dial.get()), forge::exceptions::base);
   auto target_closed = asio::co_spawn(context, target.async_close(), asio::use_future);
   BOOST_REQUIRE(drive_until([&] { return target_closed.wait_for(0ms) == std::future_status::ready; }));
   BOOST_CHECK_NO_THROW(target_closed.get());
}

asio::awaitable<void> quic_probe_source(forge::asio::runtime& runtime, std::string host, std::string wildcard) {
   auto source = quic::listener{runtime, {.host = wildcard}, test_server_options()};
   auto local = source.local_endpoint();
   local.host = host;
   auto sink = udp::socket{runtime.context(), udp::endpoint{asio::ip::make_address(host), 0}};
   const auto remote = quic::endpoint{.host = host, .port = sink.local_endpoint().port()};
   auto receive = [&]() -> asio::awaitable<void> {
      for (auto index = 0; index < 3; ++index) {
         auto bytes = std::array<std::uint8_t, 128>{};
         auto from = udp::endpoint{};
         const auto count = co_await sink.async_receive_from(asio::buffer(bytes), from, asio::use_awaitable);
         BOOST_TEST(count == 64U);
         BOOST_TEST(from.address().to_string() == host);
         BOOST_TEST(from.port() == local.port);
         BOOST_TEST((bytes.front() & 0xc0U) == 0U);
      }
   };
   const auto count = co_await (source.async_punch(local, remote, {.timeout = 2s, .max_packets = 3}) && receive());
   BOOST_TEST(count == 3U);
   co_await source.async_stop();
}

asio::awaitable<void> quic_capability_rejections(forge::asio::runtime& runtime) {
   auto source = quic::listener{runtime, {.host = "0.0.0.0"}, test_server_options()};
   auto wildcard = source.local_endpoint();
   BOOST_CHECK_THROW((quic::connector{runtime, source, wildcard}), forge::exceptions::base);
   auto foreign = wildcard;
   foreign.host = "192.0.2.123";
   BOOST_CHECK_THROW((quic::connector{runtime, source, foreign}), forge::exceptions::base);
   auto local = wildcard;
   local.host = "127.0.0.1";
   auto wrong_port = local;
   wrong_port.port = local.port == 65535 ? 65534 : local.port + 1;
   BOOST_CHECK_THROW((quic::connector{runtime, source, wrong_port}), forge::exceptions::base);
   auto connector = quic::connector{runtime, source, local};
   co_await source.async_stop();
   BOOST_CHECK_THROW((quic::connector{runtime, source, local}), forge::exceptions::base);
   auto failed = false;
   try {
      static_cast<void>(co_await connector.async_connect({.host = "127.0.0.1", .port = 12345}));
   } catch (const forge::exceptions::base&) {
      failed = true;
   }
   BOOST_TEST(failed);
}

asio::awaitable<void> quic_punch_stop_drains(forge::asio::runtime& runtime) {
   auto source = quic::listener{runtime, {.host = "127.0.0.1"}, test_server_options()};
   const auto local = source.local_endpoint();
   auto sink = udp::socket{runtime.context(), udp::endpoint{asio::ip::address_v4::loopback(), 0}};
   const auto remote = quic::endpoint{.host = "127.0.0.1", .port = sink.local_endpoint().port()};
   auto lifetime = std::make_shared<int>(1);
   const auto weak = std::weak_ptr<int>{lifetime};
   auto failed = false;
   auto punch = [&]() -> asio::awaitable<void> {
      try {
         static_cast<void>(co_await source.async_punch(
             local, remote, {.timeout = 5s, .max_packets = 256, .lifetime = std::move(lifetime)}));
      } catch (const forge::exceptions::base&) {
         failed = true;
      }
   };
   auto stop = [&]() -> asio::awaitable<void> {
      auto bytes = std::array<std::uint8_t, 128>{};
      auto from = udp::endpoint{};
      static_cast<void>(co_await sink.async_receive_from(asio::buffer(bytes), from, asio::use_awaitable));
      BOOST_TEST(from.port() == local.port);
      co_await source.async_stop();
      // Stop must join admission release, not just cancel the UDP socket.
      BOOST_TEST(weak.expired());
   };
   co_await (punch() && stop());
   BOOST_TEST(failed);
}

asio::awaitable<void> quic_punch_cancellation_preserves_source(forge::asio::runtime& runtime) {
   auto source = quic::listener{runtime, {.host = "127.0.0.1"}, test_server_options()};
   const auto local = source.local_endpoint();
   auto sink = udp::socket{runtime.context(), udp::endpoint{asio::ip::address_v4::loopback(), 0}};
   const auto remote = quic::endpoint{.host = "127.0.0.1", .port = sink.local_endpoint().port()};
   auto lifetime = std::make_shared<int>(1);
   const auto weak = std::weak_ptr<int>{lifetime};
   auto punch = source.async_punch(local, remote, {.timeout = 5s, .max_packets = 256, .lifetime = std::move(lifetime)});
   auto cancel_after_packet = [&]() -> asio::awaitable<void> {
      auto bytes = std::array<std::uint8_t, 128>{};
      auto from = udp::endpoint{};
      static_cast<void>(co_await sink.async_receive_from(asio::buffer(bytes), from, asio::use_awaitable));
      BOOST_TEST(from.port() == local.port);
   };
   // The race cancels and joins the losing punch, without stopping its source FD.
   static_cast<void>(co_await (std::move(punch) || cancel_after_packet()));
   BOOST_TEST(weak.expired());
   const auto sent = co_await source.async_punch(local, remote, {.max_packets = 1});
   BOOST_TEST(sent == 1U);
   co_await source.async_stop();
}

asio::awaitable<void> quic_punch_deadline(forge::asio::runtime& runtime) {
   auto source = quic::listener{runtime, {.host = "127.0.0.1"}, test_server_options()};
   const auto local = source.local_endpoint();
   auto sink = udp::socket{runtime.context(), udp::endpoint{asio::ip::address_v4::loopback(), 0}};
   auto lifetime = std::make_shared<int>(1);
   const auto weak = std::weak_ptr<int>{lifetime};
   auto failed = false;
   try {
      static_cast<void>(
          co_await source.async_punch(local, {.host = "127.0.0.1", .port = sink.local_endpoint().port()},
                                      {.timeout = 1ms, .max_packets = 256, .lifetime = std::move(lifetime)}));
   } catch (const forge::exceptions::base&) {
      failed = true;
   }
   BOOST_TEST(failed);
   BOOST_TEST(weak.expired());
   co_await source.async_stop();
}

asio::awaitable<void> quic_two_shared_dials(forge::asio::runtime& runtime) {
   auto source_options = test_server_options();
   source_options.limits.max_connections = 2;
   auto source = quic::listener{runtime, {.host = "127.0.0.1"}, source_options};
   const auto local = source.local_endpoint();
   auto target_options = test_server_options();
   auto first_registered = std::make_shared<forge::asio::notification>();
   target_options.inbound_admission = [first_registered]() -> std::shared_ptr<void> {
      // A validated Initial is observable only after the source registered its CID.
      first_registered->notify();
      return std::make_shared<int>(1);
   };
   auto target = quic::listener{runtime, {.host = "127.0.0.1"}, target_options};
   auto connector = quic::connector{runtime, source, local};
   const auto options = pinned_options(target_options);
   auto second = [&]() -> asio::awaitable<quic::connection> {
      static_cast<void>(co_await first_registered->async_wait(0));
      co_return co_await connector.async_connect(target.local_endpoint(), options);
   };
   auto [first, next, inbound_first, inbound_next] =
       co_await (connector.async_connect(target.local_endpoint(), options) && second() && target.async_accept() &&
                 target.async_accept());
   BOOST_TEST(first.local_endpoint().port == local.port);
   BOOST_TEST(next.local_endpoint().port == local.port);
   BOOST_TEST(inbound_first.remote_endpoint().port == local.port);
   BOOST_TEST(inbound_next.remote_endpoint().port == local.port);
   co_await (first.async_close() && next.async_close());
   co_await (inbound_first.async_close() && inbound_next.async_close());
   co_await source.async_stop();
   co_await target.async_stop();
}

asio::awaitable<void> quic_stop_drains_two_pending_dials(forge::asio::runtime& runtime) {
   auto source_options = test_server_options();
   source_options.limits.max_connections = 2;
   auto source = quic::listener{runtime, {.host = "127.0.0.1"}, source_options};
   const auto local = source.local_endpoint();
   auto connector = quic::connector{runtime, source, local};
   auto first_sink = udp::socket{runtime.context(), udp::endpoint{asio::ip::address_v4::loopback(), 0}};
   auto second_sink = udp::socket{runtime.context(), udp::endpoint{asio::ip::address_v4::loopback(), 0}};
   auto first_sent = forge::asio::notification{};
   auto stop_started = std::atomic_bool{false};
   auto first_owner = std::make_shared<int>(1);
   auto second_owner = std::make_shared<int>(2);
   const auto first_weak = std::weak_ptr<int>{first_owner};
   const auto second_weak = std::weak_ptr<int>{second_owner};
   auto dial = [&](udp::socket& sink, std::shared_ptr<void> owner, bool second) -> asio::awaitable<void> {
      if (second) {
         static_cast<void>(co_await first_sent.async_wait(0));
      }
      auto options = pinned_options(source_options);
      options.connect_timeout = 5s;
      options.connection_lifetime = std::move(owner);
      try {
         static_cast<void>(co_await connector.async_connect({.host = "127.0.0.1", .port = sink.local_endpoint().port()},
                                                            std::move(options)));
         BOOST_FAIL("silent UDP sink must not complete a QUIC handshake");
      } catch (const forge::exceptions::base&) {
         if (!stop_started.load()) {
            // In particular, a second dial must not be rejected by double billing.
            throw;
         }
      }
   };
   auto stop = [&]() -> asio::awaitable<void> {
      auto bytes = std::array<std::uint8_t, 2048>{};
      auto from = udp::endpoint{};
      static_cast<void>(co_await first_sink.async_receive_from(asio::buffer(bytes), from, asio::use_awaitable));
      BOOST_TEST(from.port() == local.port);
      first_sent.notify();
      static_cast<void>(co_await second_sink.async_receive_from(asio::buffer(bytes), from, asio::use_awaitable));
      BOOST_TEST(from.port() == local.port);
      stop_started.store(true);
      co_await source.async_stop();
      BOOST_TEST(first_weak.expired());
      BOOST_TEST(second_weak.expired());
   };
   co_await (dial(first_sink, std::move(first_owner), false) && dial(second_sink, std::move(second_owner), true) &&
             stop());
}
} // namespace

BOOST_AUTO_TEST_SUITE(coordinated_transport)

BOOST_AUTO_TEST_CASE(tcp_listener_source_port_ipv4) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, tcp_source_roundtrip(runtime, "127.0.0.1", true), 5s));
}

BOOST_AUTO_TEST_CASE(tcp_listener_source_port_ipv6) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, tcp_source_roundtrip(runtime, "::1", true), 5s));
}

BOOST_AUTO_TEST_CASE(tcp_ordinary_dial_keeps_ephemeral_default) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, tcp_source_roundtrip(runtime, "127.0.0.1", false), 5s));
}

BOOST_AUTO_TEST_CASE(tcp_listener_capability_rejects_fabricated_source_and_close) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, tcp_factory_rejections(runtime), 5s));
}

BOOST_AUTO_TEST_CASE(tcp_failed_terminal_wait_keeps_stop_pending_until_attempt_and_source_release) {
   tcp_failed_terminal_wait_drains(false);
}

BOOST_AUTO_TEST_CASE(tcp_failed_terminal_wait_keeps_listener_close_pending_until_attempt_and_source_release) {
   tcp_failed_terminal_wait_drains(true);
}

BOOST_AUTO_TEST_CASE(quic_responder_probes_use_listener_source_ipv4) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_probe_source(runtime, "127.0.0.1", "0.0.0.0"), 5s));
}

BOOST_AUTO_TEST_CASE(quic_responder_probes_use_listener_source_ipv6) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_probe_source(runtime, "::1", "::"), 5s));
}

BOOST_AUTO_TEST_CASE(quic_listener_capability_rejects_fabricated_source_and_close) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_capability_rejections(runtime), 5s));
}

BOOST_AUTO_TEST_CASE(quic_responder_stop_joins_punch_lifetime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_punch_stop_drains(runtime), 5s));
}

BOOST_AUTO_TEST_CASE(quic_responder_cancellation_preserves_listener_socket) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_punch_cancellation_preserves_source(runtime), 5s));
}

BOOST_AUTO_TEST_CASE(quic_responder_deadline_releases_lifetime) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_punch_deadline(runtime), 5s));
}

BOOST_AUTO_TEST_CASE(quic_two_listener_owned_dials_transfer_budget_once) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_two_shared_dials(runtime), 10s));
}

BOOST_AUTO_TEST_CASE(quic_listener_stop_joins_two_pending_dial_lifetimes) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_TEST(forge::asio::blocking::run_for(runtime, quic_stop_drains_two_pending_dials(runtime), 10s));
}

BOOST_AUTO_TEST_SUITE_END()
