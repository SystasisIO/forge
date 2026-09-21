#include <boost/test/unit_test.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/v6_only.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <array>
#include <chrono>
#include <future>
#include <optional>
#include <utility>

#if defined(__APPLE__)
#include <sys/socket.h>
#include <netinet/in.h>
#endif

import forge.net.transport.datagram_io;

namespace {

namespace asio = boost::asio;
namespace io = forge::net::transport::datagram_io;
using udp = asio::ip::udp;
using namespace std::chrono_literals;

template <typename T>
void finish(asio::io_context& context, std::future<T>& result) {
   context.run_for(2s);
   const auto ready = result.wait_for(0s) == std::future_status::ready;
   if (!ready) {
      context.stop();
   }
   BOOST_REQUIRE_MESSAGE(ready, "datagram operation exceeded two-second test bound");
   result.get();
   context.restart();
}

template <typename T>
asio::awaitable<void> expect_error(asio::awaitable<T> operation, boost::system::error_code expected) {
   try {
      static_cast<void>(co_await std::move(operation));
      BOOST_FAIL("datagram operation unexpectedly succeeded");
   } catch (const boost::system::system_error& error) {
      BOOST_CHECK(error.code() == expected);
   }
}

void roundtrip(bool ipv6) {
   auto context = asio::io_context{};
   const auto protocol = ipv6 ? udp::v6() : udp::v4();
   const auto loopback = ipv6 ? asio::ip::address{asio::ip::address_v6::loopback()}
                              : asio::ip::address{asio::ip::address_v4::loopback()};
   auto server = udp::socket{context, protocol};
   if (ipv6) {
      server.set_option(asio::ip::v6_only{true});
   }
   // Configuration before bind is supported; metadata uses the eventual port.
   io::configure(server);
   server.bind(udp::endpoint{protocol, 0});
   auto client = udp::socket{context, udp::endpoint{loopback, 0}};
   io::configure(client);
   auto test = [&]() -> asio::awaitable<void> {
      const auto payload = std::array<char, 4>{'t', 'e', 's', 't'};
      const auto destination = udp::endpoint{loopback, server.local_endpoint().port()};
      BOOST_CHECK_EQUAL(co_await io::async_send(client, asio::buffer(payload), destination), payload.size());
      auto buffer = std::array<char, 16>{};
      const auto request = co_await io::async_receive(server, asio::buffer(buffer));
      BOOST_CHECK_EQUAL(request.size, payload.size());
      BOOST_CHECK(request.remote == client.local_endpoint());
      BOOST_CHECK(request.local == destination);
      BOOST_CHECK_NE(request.interface_index, 0U);
      BOOST_CHECK_EQUAL_COLLECTIONS(buffer.begin(), buffer.begin() + request.size, payload.begin(), payload.end());
      const auto local = io::source{.address = request.local.address(),
                                    .interface_index = ipv6 ? request.interface_index : 0};
#if defined(__APPLE__)
      auto previous_interface = unsigned{};
      auto option_size = static_cast<socklen_t>(sizeof(previous_interface));
      if (!ipv6) {
         BOOST_REQUIRE_EQUAL(::getsockopt(server.native_handle(), IPPROTO_IP, IP_BOUND_IF,
                                         &previous_interface, &option_size), 0);
      }
#endif
      BOOST_CHECK_EQUAL(co_await io::async_send(server, asio::buffer(buffer.data(), request.size), request.remote, local),
                        payload.size());
#if defined(__APPLE__)
      if (!ipv6) {
         auto restored_interface = unsigned{};
         option_size = sizeof(restored_interface);
         BOOST_REQUIRE_EQUAL(::getsockopt(server.native_handle(), IPPROTO_IP, IP_BOUND_IF,
                                         &restored_interface, &option_size), 0);
         BOOST_CHECK_EQUAL(restored_interface, previous_interface);
      }
#endif
      const auto reply = co_await io::async_receive(client, asio::buffer(buffer));
      BOOST_CHECK(reply.remote == destination);
      BOOST_CHECK(reply.local == client.local_endpoint());
      BOOST_CHECK_EQUAL(reply.interface_index, request.interface_index);
      BOOST_CHECK(server.local_endpoint().address().is_unspecified());

      // Kernel-selected mode must remain usable after the explicit-source send.
      BOOST_CHECK_EQUAL(co_await io::async_send(server, asio::const_buffer{}, request.remote, std::nullopt), 0U);
      const auto empty = co_await io::async_receive(client, asio::mutable_buffer{});
      BOOST_CHECK_EQUAL(empty.size, 0U);
      BOOST_CHECK(empty.remote == destination);
   };
   auto result = asio::co_spawn(context, test, asio::use_future);
   finish(context, result);
}

} // namespace

BOOST_AUTO_TEST_SUITE(datagram_io_tests)

BOOST_AUTO_TEST_CASE(wildcard_ipv4_destination_and_explicit_reply_source) {
   roundtrip(false);
}

BOOST_AUTO_TEST_CASE(wildcard_ipv6_destination_and_explicit_reply_source) {
   roundtrip(true);
}

#if defined(__APPLE__)
BOOST_AUTO_TEST_CASE(darwin_ipv4_interface_requires_prebound_owner_and_never_mutates_options) {
   auto context = asio::io_context{};
   const auto loopback = asio::ip::address_v4::loopback();
   auto wildcard = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   auto exact = udp::socket{context, udp::endpoint{loopback, 0}};
   auto sticky = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   auto client = udp::socket{context, udp::endpoint{loopback, 0}};
   io::configure(wildcard);
   io::configure(exact);
   io::configure(sticky);
   io::configure(client);
   const auto bound_index = [](udp::socket& socket) {
      unsigned index = 0;
      auto length = static_cast<socklen_t>(sizeof(index));
      BOOST_REQUIRE_EQUAL(::getsockopt(socket.native_handle(), IPPROTO_IP, IP_BOUND_IF, &index, &length), 0);
      return index;
   };
   auto test = [&]() -> asio::awaitable<void> {
      auto payload = std::array<char, 1>{'x'};
      static_cast<void>(co_await io::async_send(client, asio::buffer(payload),
          udp::endpoint{loopback, wildcard.local_endpoint().port()}));
      const auto request = co_await io::async_receive(wildcard, asio::buffer(payload));
      const auto source = io::source{.address = loopback, .interface_index = request.interface_index};
      BOOST_REQUIRE_NE(source.interface_index, 0U);
      BOOST_CHECK_EQUAL(bound_index(wildcard), 0U);
      co_await expect_error(io::async_send(wildcard, asio::buffer(payload), request.remote, source),
                            asio::error::operation_not_supported);
      BOOST_CHECK_EQUAL(bound_index(wildcard), 0U);
      BOOST_CHECK_EQUAL(client.available(), 0U);

      // A source-bound owner preserves inp_laddr when pktinfo selects the link.
      static_cast<void>(co_await io::async_send(exact, asio::buffer(payload), request.remote, source));
      const auto exact_reply = co_await io::async_receive(client, asio::buffer(payload));
      BOOST_CHECK(exact_reply.remote == exact.local_endpoint());
      BOOST_CHECK_EQUAL(exact_reply.interface_index, source.interface_index);
      BOOST_CHECK_EQUAL(bound_index(exact), 0U);
      co_await expect_error(io::async_send(exact, asio::buffer(payload), request.remote,
          io::source{.address = asio::ip::make_address("192.0.2.1"), .interface_index = source.interface_index}),
          asio::error::operation_not_supported);

      // Owner configuration before any I/O on this socket, not an async_send bypass.
      const auto index = static_cast<unsigned>(source.interface_index);
      BOOST_REQUIRE_EQUAL(::setsockopt(sticky.native_handle(), IPPROTO_IP, IP_BOUND_IF, &index, sizeof(index)), 0);
      static_cast<void>(co_await io::async_send(sticky, asio::buffer(payload), request.remote, source));
      const auto sticky_reply = co_await io::async_receive(client, asio::buffer(payload));
      BOOST_CHECK(sticky_reply.remote.address() == loopback);
      BOOST_CHECK_EQUAL(sticky_reply.remote.port(), sticky.local_endpoint().port());
      BOOST_CHECK_EQUAL(sticky_reply.interface_index, source.interface_index);
      BOOST_CHECK_EQUAL(bound_index(sticky), index);
      const auto other_index = source.interface_index == 1 ? 2U : 1U;
      co_await expect_error(io::async_send(sticky, asio::buffer(payload), request.remote,
          io::source{.address = loopback, .interface_index = other_index}), asio::error::operation_not_supported);
      BOOST_CHECK_EQUAL(bound_index(sticky), index);
      BOOST_CHECK_EQUAL(client.available(), 0U);
   };
   auto result = asio::co_spawn(context, test, asio::use_future);
   finish(context, result);
}
#endif

BOOST_AUTO_TEST_CASE(truncation_consumes_only_the_bad_datagram) {
   auto context = asio::io_context{};
   auto receiver = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   auto sender = udp::socket{context, udp::v4()};
   io::configure(receiver);
   io::configure(sender);
   auto test = [&]() -> asio::awaitable<void> {
      const auto destination = udp::endpoint{asio::ip::address_v4::loopback(), receiver.local_endpoint().port()};
      const auto packet = std::array<char, 8>{};
      auto small = std::array<char, 2>{};
      static_cast<void>(co_await io::async_send(sender, asio::buffer(packet), destination));
      co_await expect_error(io::async_receive(receiver, asio::buffer(small)), asio::error::message_size);
      static_cast<void>(co_await io::async_send(sender, asio::buffer(packet), destination));
      co_await expect_error(io::async_receive(receiver, asio::mutable_buffer{}), asio::error::message_size);
      static_cast<void>(co_await io::async_send(sender, asio::buffer(small), destination));
      BOOST_CHECK_EQUAL((co_await io::async_receive(receiver, asio::buffer(small))).size, small.size());
   };
   auto result = asio::co_spawn(context, test, asio::use_future);
   finish(context, result);
}

BOOST_AUTO_TEST_CASE(receive_requires_packet_info_configuration) {
   auto context = asio::io_context{};
   auto receiver = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   auto sender = udp::socket{context, udp::v4()};
   auto test = [&]() -> asio::awaitable<void> {
      const auto destination = udp::endpoint{asio::ip::address_v4::loopback(), receiver.local_endpoint().port()};
      auto packet = std::array<char, 1>{};
      static_cast<void>(co_await io::async_send(sender, asio::buffer(packet), destination));
      co_await expect_error(io::async_receive(receiver, asio::buffer(packet)), asio::error::invalid_argument);
   };
   auto result = asio::co_spawn(context, test, asio::use_future);
   finish(context, result);
}

BOOST_AUTO_TEST_CASE(invalid_explicit_sources_and_scope_conflicts_are_rejected) {
   auto context = asio::io_context{};
   auto v4 = udp::socket{context, udp::v4()};
   auto v6 = udp::socket{context, udp::v6()};
   auto test = [&]() -> asio::awaitable<void> {
      const auto peer4 = udp::endpoint{asio::ip::address_v4::loopback(), 4001};
      const auto peer6 = udp::endpoint{asio::ip::address_v6::loopback(), 4001};
      for (const auto& address : {asio::ip::make_address("0.0.0.0"), asio::ip::make_address("224.0.0.251"),
                                asio::ip::make_address("255.255.255.255"),
                                asio::ip::make_address("::1")}) {
         co_await expect_error(io::async_send(v4, {}, peer4, io::source{.address = address}),
                               asio::error::invalid_argument);
      }
      for (const auto& address : {asio::ip::make_address("::"), asio::ip::make_address("ff02::fb"),
                                asio::ip::make_address("127.0.0.1")}) {
         co_await expect_error(io::async_send(v6, {}, peer6, io::source{.address = address}),
                               asio::error::invalid_argument);
      }
      auto scoped = asio::ip::make_address_v6("fe80::1");
      scoped.scope_id(7);
      co_await expect_error(io::async_send(v6, {}, peer6, io::source{.address = scoped, .interface_index = 8}),
                            asio::error::invalid_argument);
      co_await expect_error(io::async_send(v6, {}, udp::endpoint{scoped, 4001},
                                          io::source{.address = asio::ip::address_v6::loopback(), .interface_index = 8}),
                            asio::error::invalid_argument);
      co_await expect_error(io::async_send(v6, {}, peer6, io::source{.address = asio::ip::make_address("fe80::1")}),
                            asio::error::invalid_argument);
      co_await expect_error(io::async_send(v6, {}, udp::endpoint{asio::ip::make_address("fe80::2"), 4001}),
                            asio::error::invalid_argument);
      co_await expect_error(io::async_send(v4, {}, peer6), asio::error::invalid_argument);
      co_await expect_error(io::async_send(v4, {}, udp::endpoint{udp::v4(), 4001}), asio::error::invalid_argument);
   };
   auto result = asio::co_spawn(context, test, asio::use_future);
   finish(context, result);
}

BOOST_AUTO_TEST_CASE(cancel_pending_receive_keeps_socket_usable) {
   auto context = asio::io_context{};
   auto receiver = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   io::configure(receiver);
   auto cancellation = asio::cancellation_signal{};
   auto timer = asio::steady_timer{context, 5ms};
   timer.async_wait([&](boost::system::error_code error) {
      if (!error) {
         cancellation.emit(asio::cancellation_type::terminal);
      }
   });
   auto buffer = std::array<char, 1>{};
   auto result = asio::co_spawn(context,
       expect_error(io::async_receive(receiver, asio::buffer(buffer)), asio::error::operation_aborted),
       asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
   finish(context, result);
   BOOST_CHECK(receiver.is_open());
   auto sender = udp::socket{context, udp::v4()};
   auto test = [&]() -> asio::awaitable<void> {
      const auto destination = udp::endpoint{asio::ip::address_v4::loopback(), receiver.local_endpoint().port()};
      static_cast<void>(co_await io::async_send(sender, asio::buffer(buffer), destination));
      BOOST_CHECK_EQUAL((co_await io::async_receive(receiver, asio::buffer(buffer))).size, 1U);
   };
   auto again = asio::co_spawn(context, test, asio::use_future);
   finish(context, again);
}

BOOST_AUTO_TEST_CASE(precancel_prevents_native_receive_and_send_even_when_ready) {
   auto context = asio::io_context{};
   auto receiver = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   auto sender = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   io::configure(receiver);
   io::configure(sender);
   auto cancellation = asio::cancellation_signal{};
   auto test = [&]() -> asio::awaitable<void> {
      co_await asio::this_coro::throw_if_cancelled(false);
      const auto destination = udp::endpoint{asio::ip::address_v4::loopback(), receiver.local_endpoint().port()};
      auto buffer = std::array<char, 1>{'x'};
      static_cast<void>(co_await io::async_send(sender, asio::buffer(buffer), destination));
      cancellation.emit(asio::cancellation_type::terminal);
      co_await expect_error(io::async_receive(receiver, asio::buffer(buffer)), asio::error::operation_aborted);
      co_await expect_error(io::async_send(sender, asio::buffer(buffer), destination), asio::error::operation_aborted);
      co_await asio::this_coro::reset_cancellation_state();
      BOOST_CHECK_EQUAL((co_await io::async_receive(receiver, asio::buffer(buffer))).size, 1U);
      BOOST_CHECK_EQUAL(receiver.available(), 0U);
   };
   auto result = asio::co_spawn(context, test, asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
   finish(context, result);
}

BOOST_AUTO_TEST_CASE(socket_close_cancels_a_pending_receive) {
   auto context = asio::io_context{};
   auto receiver = udp::socket{context, udp::endpoint{udp::v4(), 0}};
   io::configure(receiver);
   auto timer = asio::steady_timer{context, 5ms};
   timer.async_wait([&](boost::system::error_code error) {
      if (!error) {
         receiver.close();
      }
   });
   auto buffer = std::array<char, 1>{};
   auto result = asio::co_spawn(context,
       expect_error(io::async_receive(receiver, asio::buffer(buffer)), asio::error::operation_aborted), asio::use_future);
   finish(context, result);
}

BOOST_AUTO_TEST_SUITE_END()
