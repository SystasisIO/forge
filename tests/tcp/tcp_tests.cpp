#include <boost/test/unit_test.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <net/if.h>
#endif

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.net.tcp.connection;
import forge.net.tcp.connector;
import forge.net.tcp.exceptions;
import forge.net.tcp.listener;
import forge.net.tcp.options;
import forge.net.tcp.transport;
import forge.net.transport.buffer;
import forge.net.transport.endpoint;
import forge.net.transport.exceptions;
import forge.net.transport.frame;
import forge.net.transport.registry;
import forge.net.transport.stream;

namespace {

using bytes = std::vector<std::uint8_t>;

[[nodiscard]] bytes text_bytes(std::string_view value) {
   return {value.begin(), value.end()};
}

[[nodiscard]] forge::net::transport::endpoint loopback(std::uint16_t port) {
   return forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
                                          .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                          .host = "127.0.0.1",
                                          .port = port};
}

[[nodiscard]] forge::net::transport::endpoint dns4_loopback(std::uint16_t port) {
   return forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::dns4,
                                          .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                          .host = "localhost",
                                          .port = port};
}

[[nodiscard]] forge::net::transport::endpoint invalid_quic_endpoint() {
   return forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip4,
                                          .protocol = forge::net::transport::endpoint::protocol_kind::quic_v1,
                                          .host = "127.0.0.1",
                                          .port = 1};
}

#if defined(__APPLE__) || defined(__linux__)
struct native_interface {
   std::string name;
   unsigned int index = 0;
};

[[nodiscard]] std::optional<native_interface> first_native_interface() {
   auto* interfaces = if_nameindex();
   if (interfaces == nullptr) {
      return std::nullopt;
   }

   auto result = std::optional<native_interface>{};
   for (auto* current = interfaces; current->if_index != 0; ++current) {
      if (current->if_name == nullptr) {
         continue;
      }
      const auto index = if_nametoindex(current->if_name);
      if (index != 0) {
         result = native_interface{.name = current->if_name, .index = index};
         break;
      }
   }
   if_freenameindex(interfaces);
   return result;
}
#endif

boost::asio::awaitable<void> tcp_roundtrip() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   const auto local = listener.local_endpoint();
   BOOST_CHECK(local.protocol == forge::net::transport::endpoint::protocol_kind::tcp);
   BOOST_CHECK_EQUAL(local.host, "127.0.0.1");
   BOOST_CHECK(local.port != 0);

   auto accept = boost::asio::co_spawn(executor, listener.async_accept(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect(local);
   auto server = co_await std::move(accept);

   BOOST_CHECK(client.local_endpoint.protocol == forge::net::transport::endpoint::protocol_kind::tcp);
   BOOST_CHECK_EQUAL(client.remote_endpoint.port, local.port);
   BOOST_CHECK_EQUAL(server.local_endpoint.port, local.port);
   BOOST_CHECK_EQUAL(server.remote_endpoint.port, client.local_endpoint.port);
   BOOST_CHECK(client.stream.valid());
   BOOST_CHECK(server.stream.valid());

   const auto ping = text_bytes("ping");
   co_await client.stream.async_write(ping);
   auto received_ping = co_await server.stream.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_ping.begin(), received_ping.end(), ping.begin(), ping.end());

   const auto pong = text_bytes("pong");
   co_await server.stream.async_write(pong);
   auto received_pong = co_await client.stream.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_pong.begin(), received_pong.end(), pong.begin(), pong.end());

   const auto chunk_payload = text_bytes("chunk payload");
   co_await client.stream.async_write(forge::net::transport::chunk{chunk_payload});
   auto received_chunk = co_await server.stream.async_read_chunk();
   const auto received_chunk_bytes = received_chunk.to_vector();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_chunk_bytes.begin(), received_chunk_bytes.end(), chunk_payload.begin(),
                                 chunk_payload.end());

   const auto framed = text_bytes("framed payload");
   co_await client.stream.async_write_frame(framed);
   auto received_frame = co_await server.stream.async_read_frame();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_frame.begin(), received_frame.end(), framed.begin(), framed.end());

   const auto framed_chunk = text_bytes("framed chunk payload");
   co_await client.stream.async_write_frame(forge::net::transport::chunk{framed_chunk});
   auto received_frame_chunk = co_await server.stream.async_read_frame_chunk();
   const auto received_frame_chunk_bytes = received_frame_chunk.to_vector();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_frame_chunk_bytes.begin(), received_frame_chunk_bytes.end(),
                                 framed_chunk.begin(), framed_chunk.end());

   co_await client.stream.async_close();
   co_await server.stream.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> tcp_read_chunk_limit_is_behavioral() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0), {}, forge::net::tcp::options{.read_chunk_size = 4}};
   const auto local = listener.local_endpoint();

   auto accept = boost::asio::co_spawn(executor, listener.async_accept(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor, forge::net::tcp::options{.read_chunk_size = 4}};
   auto client = co_await connector.async_connect(local);
   auto server = co_await std::move(accept);

   const auto outbound = text_bytes("abcdef");
   const auto expected = text_bytes("abcd");
   co_await client.stream.async_write(outbound);
   auto chunk = co_await server.stream.async_read();
   BOOST_CHECK_EQUAL(chunk.size(), 4U);
   BOOST_CHECK_EQUAL_COLLECTIONS(chunk.begin(), chunk.end(), expected.begin(), expected.end());

   co_await client.stream.async_close();
   co_await server.stream.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> tcp_connection_roundtrip_and_handoff() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   const auto local = listener.local_endpoint();

   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(local);
   auto server = co_await std::move(accept);

   BOOST_CHECK(client.valid());
   BOOST_CHECK(server.valid());
   BOOST_CHECK_EQUAL(client.remote_endpoint().port, local.port);
   BOOST_CHECK_EQUAL(server.local_endpoint().port, local.port);

   const auto ping = text_bytes("connection ping");
   co_await client.async_write(ping);
   auto received_ping = co_await server.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received_ping.begin(), received_ping.end(), ping.begin(), ping.end());

   const auto stale_payload = text_bytes("stale facade write");
   auto stale_write = client.async_write(stale_payload);
   auto stream_connection = std::move(client).into_transport_stream();
   BOOST_CHECK(stream_connection.stream.valid());
   BOOST_CHECK_THROW((void)co_await std::move(stale_write), forge::net::tcp::exceptions::closed);
   const auto framed = text_bytes("connection framed");
   co_await stream_connection.stream.async_write_frame(framed);
   auto received_frame = co_await server.async_read();
   auto decoded = forge::net::transport::decode_frame(received_frame);
   BOOST_REQUIRE(decoded.status == forge::net::transport::frame_decode_status::complete);
   BOOST_CHECK_EQUAL_COLLECTIONS(decoded.payload.begin(), decoded.payload.end(), framed.begin(), framed.end());

   co_await stream_connection.stream.async_close();
   co_await server.async_close();
   co_await listener.async_close();

   auto release_listener = forge::net::tcp::listener{executor, loopback(0)};
   auto release_accept =
       boost::asio::co_spawn(executor, release_listener.async_accept_connection(), boost::asio::use_awaitable);
   auto release_lifetime = std::make_shared<int>(1);
   auto released_lifetime_observer = std::weak_ptr<int>{release_lifetime};
   auto release_client =
       co_await connector.async_connect_connection(release_listener.local_endpoint(), {}, release_lifetime);
   auto release_server = co_await std::move(release_accept);
   release_lifetime.reset();
   BOOST_CHECK_THROW((void)std::move(release_client).release_socket(), forge::net::tcp::exceptions::invalid_options);
   BOOST_CHECK(release_client.valid());
   BOOST_CHECK(!released_lifetime_observer.expired());
   const auto retained_payload = text_bytes("lifetime retained after rejected socket handoff");
   co_await release_client.async_write(retained_payload);
   auto retained = co_await release_server.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(retained.begin(), retained.end(), retained_payload.begin(), retained_payload.end());
   auto transferred_lifetime = std::shared_ptr<void>{};
   auto socket = std::move(release_client).release_socket(transferred_lifetime);
   BOOST_CHECK(socket.is_open());
   BOOST_CHECK(!released_lifetime_observer.expired());
   auto ignored = boost::system::error_code{};
   socket.close(ignored);
   transferred_lifetime.reset();
   const auto release_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!released_lifetime_observer.expired() && std::chrono::steady_clock::now() < release_deadline) {
      auto release_wait = boost::asio::steady_timer{executor};
      release_wait.expires_after(std::chrono::milliseconds{1});
      co_await release_wait.async_wait(boost::asio::use_awaitable);
   }
   BOOST_CHECK(released_lifetime_observer.expired());
   co_await release_server.async_close();
   co_await release_listener.async_close();
}

boost::asio::awaitable<void> tcp_registry_roundtrip() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto registry = forge::net::transport::registry{};
   forge::net::tcp::register_stream(registry, executor);

   auto listener = co_await registry.async_listen_stream(loopback(0));
   auto accept = boost::asio::co_spawn(executor, listener.async_accept(), boost::asio::use_awaitable);
   auto client = co_await registry.async_connect_stream(listener.local_endpoint());
   auto server = co_await std::move(accept);

   const auto payload = text_bytes("registry");
   co_await client.stream.async_write(payload);
   auto received = co_await server.stream.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());

   co_await client.stream.async_close();
   co_await server.stream.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> cancel_unblocks_accept() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(std::chrono::milliseconds{25});
   boost::asio::co_spawn(
       executor,
       [&listener, timer = std::move(timer)]() mutable -> boost::asio::awaitable<void> {
          co_await timer.async_wait(boost::asio::use_awaitable);
          listener.cancel();
       },
       boost::asio::detached);

   try {
      (void)co_await listener.async_accept();
      BOOST_FAIL("accept should be canceled");
   } catch (const forge::net::tcp::exceptions::canceled&) {
      co_return;
   }
}

boost::asio::awaitable<void> close_unblocks_accept() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(std::chrono::milliseconds{25});
   boost::asio::co_spawn(
       executor,
       [&listener, timer = std::move(timer)]() mutable -> boost::asio::awaitable<void> {
          co_await timer.async_wait(boost::asio::use_awaitable);
          co_await listener.async_close();
       },
       boost::asio::detached);

   try {
      (void)co_await listener.async_accept();
      BOOST_FAIL("accept should be closed");
   } catch (const forge::net::tcp::exceptions::closed&) {
      co_return;
   }
}

boost::asio::awaitable<void> close_releases_bound_port() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   const auto local = listener.local_endpoint();

   listener.close();
   BOOST_CHECK(!listener.valid());
   co_await listener.async_close();

   auto rebound = forge::net::tcp::listener{executor, local};
   BOOST_CHECK(rebound.valid());
   co_await rebound.async_close();
}

boost::asio::awaitable<void> connection_cancel_unblocks_pending_read() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await std::move(accept);

   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(std::chrono::milliseconds{25});
   boost::asio::co_spawn(
       executor,
       [&server, timer = std::move(timer)]() mutable -> boost::asio::awaitable<void> {
          co_await timer.async_wait(boost::asio::use_awaitable);
          server.cancel();
       },
       boost::asio::detached);

   try {
      (void)co_await server.async_read();
      BOOST_FAIL("read should be canceled");
   } catch (const forge::net::tcp::exceptions::canceled&) {
   }

   co_await client.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> connection_request_cancel_before_terminal_worker_arms_is_sticky() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await std::move(accept);

   // No suspension occurs between connection publication and this request.
   client.request_cancel();
   BOOST_TEST(!client.valid());
   co_await client.async_close();

   BOOST_CHECK_THROW((void)co_await server.async_read(), forge::net::tcp::exceptions::closed);
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> stream_request_cancel_before_terminal_worker_arms_is_sticky() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await std::move(accept);
   auto handed_off = std::move(client).into_transport_stream();

   // No suspension occurs between publication and this request.
   handed_off.stream.request_cancel();
   BOOST_TEST(!handed_off.stream.valid());
   co_await handed_off.stream.async_close();

   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> transport_stream_destruction_completes_owned_terminal_worker() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await std::move(accept);
   auto peer_read = boost::asio::co_spawn(executor, server.async_read(), boost::asio::use_awaitable);

   {
      auto handed_off = std::move(client).into_transport_stream();
      BOOST_TEST(handed_off.stream.valid());
   }

   BOOST_CHECK_THROW((void)co_await std::move(peer_read), forge::net::tcp::exceptions::closed);
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> foreign_thread_connection_request_cancel_unblocks_pending_read() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await std::move(accept);

   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(std::chrono::milliseconds{25});
   boost::asio::co_spawn(
       executor,
       [&server, timer = std::move(timer)]() mutable -> boost::asio::awaitable<void> {
          co_await timer.async_wait(boost::asio::use_awaitable);
          auto cancel_thread = std::thread{[&server] { server.request_cancel(); }};
          cancel_thread.join();
       },
       boost::asio::detached);

   try {
      (void)co_await server.async_read();
      BOOST_FAIL("foreign-thread cancel should unblock tcp read");
   } catch (const forge::net::tcp::exceptions::canceled&) {
   }

   co_await server.async_close();
   co_await client.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> foreign_thread_listener_close_unblocks_pending_accept() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto timer = boost::asio::steady_timer{executor};
   timer.expires_after(std::chrono::milliseconds{25});
   boost::asio::co_spawn(
       executor,
       [&listener, timer = std::move(timer)]() mutable -> boost::asio::awaitable<void> {
          co_await timer.async_wait(boost::asio::use_awaitable);
          auto close_thread = std::thread{[&listener] { listener.close(); }};
          close_thread.join();
       },
       boost::asio::detached);

   try {
      (void)co_await listener.async_accept_connection();
      BOOST_FAIL("foreign-thread close should unblock tcp accept");
   } catch (const forge::net::tcp::exceptions::closed&) {
   }
   co_await listener.async_close();
}

boost::asio::awaitable<void> active_connection_io_rejects_native_handoff() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await std::move(accept);
   auto read_done = forge::asio::notification{};
   const auto observed = read_done.epoch();
   auto read_error = std::exception_ptr{};
   auto read_result = bytes{};

   boost::asio::co_spawn(
       executor,
       [&]() -> boost::asio::awaitable<void> {
          try {
             read_result = co_await server.async_read();
          } catch (...) {
             read_error = std::current_exception();
          }
          read_done.notify();
       },
       boost::asio::detached);

   // The read owns its native-operation reservation before this timer expires.
   auto started = boost::asio::steady_timer{executor};
   started.expires_after(std::chrono::milliseconds{25});
   co_await started.async_wait(boost::asio::use_awaitable);
   BOOST_CHECK_THROW((void)std::move(server).into_transport_stream(), forge::net::tcp::exceptions::io_error);
   BOOST_TEST(server.valid());

   const auto payload = text_bytes("handoff preparation preserves the connection");
   co_await client.async_write(payload);
   (void)co_await read_done.async_wait(observed);
   BOOST_CHECK(!read_error);
   BOOST_CHECK_EQUAL_COLLECTIONS(read_result.begin(), read_result.end(), payload.begin(), payload.end());

   auto handed_off = std::move(server).into_transport_stream();
   BOOST_TEST(handed_off.stream.valid());
   const auto response = text_bytes("handoff retry");
   co_await handed_off.stream.async_write(response);
   auto received = co_await client.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), response.begin(), response.end());

   co_await handed_off.stream.async_close();
   co_await client.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> late_request_cancel_does_not_close_handed_off_native_ownership() {
   constexpr auto race_iterations = std::size_t{32};
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto connector = forge::net::tcp::connector{executor};
   auto successful_transport_handoffs = std::size_t{};

   for (auto iteration = std::size_t{}; iteration < race_iterations; ++iteration) {
      auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
      auto client = co_await connector.async_connect_connection(listener.local_endpoint());
      auto server = co_await std::move(accept);
      auto start = std::barrier{2};
      auto handoff_finished = std::atomic_bool{false};
      auto cancel_thread = std::thread{[&, iteration] {
         start.arrive_and_wait();
         if (iteration == 0) {
            while (!handoff_finished.load(std::memory_order_acquire)) {
               std::this_thread::yield();
            }
         }
         client.request_cancel();
      }};
      start.arrive_and_wait();

      auto handed_off = std::optional<forge::net::transport::stream_connection>{};
      try {
         handed_off.emplace(std::move(client).into_transport_stream());
      } catch (const forge::net::tcp::exceptions::closed&) {
      }
      handoff_finished.store(true, std::memory_order_release);
      cancel_thread.join();
      co_await client.async_close();

      if (handed_off) {
         ++successful_transport_handoffs;
         const auto payload = bytes{static_cast<std::uint8_t>(iteration)};
         co_await handed_off->stream.async_write(payload);
         auto received = co_await server.async_read();
         BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());
         co_await handed_off->stream.async_close();
      }
      co_await server.async_close();
   }

   BOOST_TEST(successful_transport_handoffs > 0U);

   auto accept = boost::asio::co_spawn(executor, listener.async_accept_connection(), boost::asio::use_awaitable);
   auto client = co_await connector.async_connect_connection(listener.local_endpoint());
   auto server = co_await std::move(accept);
   auto socket = std::move(client).release_socket();
   auto cancel_thread = std::thread{[&client] { client.request_cancel(); }};
   cancel_thread.join();
   co_await client.async_close();

   BOOST_TEST(socket.is_open());
   const auto payload = text_bytes("released socket remains open");
   co_await boost::asio::async_write(socket, boost::asio::buffer(payload), boost::asio::use_awaitable);
   auto received = co_await server.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());

   auto ignored = boost::system::error_code{};
   socket.close(ignored);
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> connector_cancel_rejects_future_connects() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto connector = forge::net::tcp::connector{executor};

   connector.cancel();
   BOOST_CHECK(!connector.valid());
   BOOST_CHECK_THROW((void)co_await connector.async_connect_connection(listener.local_endpoint()),
                     forge::net::tcp::exceptions::closed);

   co_await listener.async_close();
}

boost::asio::awaitable<void> connector_request_cancel_before_terminal_worker_arms_is_sticky() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto connector = forge::net::tcp::connector{executor};

   // Construction publishes the worker, but it has not run on this executor yet.
   connector.request_cancel();
   BOOST_TEST(!connector.valid());
   BOOST_CHECK_THROW((void)co_await connector.async_connect_connection(listener.local_endpoint()),
                     forge::net::tcp::exceptions::closed);

   co_await listener.async_close();
}

boost::asio::awaitable<void> foreign_thread_connector_request_cancel_races_resolve_connect_handoff() {
   constexpr auto race_iterations = std::size_t{16};
   auto executor = co_await boost::asio::this_coro::executor;
   auto canceled_connects = std::size_t{};
   auto handed_off_connections = std::size_t{};

   for (auto iteration = std::size_t{}; iteration < race_iterations; ++iteration) {
      auto listener = forge::net::tcp::listener{executor, loopback(0)};
      auto connector = forge::net::tcp::connector{executor};
      auto completion = forge::asio::notification{};
      const auto observed_completion = completion.epoch();
      auto client = std::optional<forge::net::tcp::connection>{};
      auto connect_error = std::exception_ptr{};

      boost::asio::co_spawn(
          executor,
          [&]() -> boost::asio::awaitable<void> {
             try {
                client.emplace(
                    co_await connector.async_connect_connection(dns4_loopback(listener.local_endpoint().port)));
             } catch (...) {
                connect_error = std::current_exception();
             }
             completion.notify();
          },
          boost::asio::detached);

      // Two queue turns let the connector register its generation before the foreign-thread cancel races completion.
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
      auto cancel_thread = std::thread{[&connector] { connector.request_cancel(); }};
      cancel_thread.join();
      (void)co_await completion.async_wait(observed_completion);

      BOOST_CHECK(!connector.valid());
      if (connect_error) {
         try {
            std::rethrow_exception(connect_error);
         } catch (const forge::net::tcp::exceptions::canceled&) {
            ++canceled_connects;
         }
         co_await listener.async_close();
         continue;
      }

      BOOST_REQUIRE(client.has_value());
      ++handed_off_connections;
      auto server = co_await listener.async_accept_connection();
      const auto payload = bytes{static_cast<std::uint8_t>(iteration)};
      co_await client->async_write(payload);
      auto received = co_await server.async_read();
      BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());
      co_await client->async_close();
      co_await server.async_close();
      co_await listener.async_close();
   }

   BOOST_CHECK_EQUAL(canceled_connects + handed_off_connections, race_iterations);
}

boost::asio::awaitable<void> late_foreign_thread_connector_request_cancel_preserves_handed_off_connection() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto listener = forge::net::tcp::listener{executor, loopback(0)};
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect_connection(dns4_loopback(listener.local_endpoint().port));
   auto server = co_await listener.async_accept_connection();

   auto cancel_thread = std::thread{[&connector] { connector.request_cancel(); }};
   cancel_thread.join();
   co_await boost::asio::post(executor, boost::asio::use_awaitable);
   co_await boost::asio::post(executor, boost::asio::use_awaitable);

   BOOST_CHECK(!connector.valid());
   const auto payload = text_bytes("connector handoff survives late cancel");
   co_await client.async_write(payload);
   auto received = co_await server.async_read();
   BOOST_CHECK_EQUAL_COLLECTIONS(received.begin(), received.end(), payload.begin(), payload.end());

   co_await client.async_close();
   co_await server.async_close();
   co_await listener.async_close();
}

boost::asio::awaitable<void> tcp_scoped_loopback_preserves_numeric_reverse_zone() {
#if defined(__APPLE__) || defined(__linux__)
   const auto scope = first_native_interface();
   if (!scope) {
      BOOST_TEST_MESSAGE("no native interface available for scoped TCP loopback");
      co_return;
   }

   auto executor = co_await boost::asio::this_coro::executor;
   const auto requested = forge::net::transport::endpoint{
       .host_type = forge::net::transport::endpoint::host_kind::ip6,
       .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
       .host = "::1",
       .port = 0,
       .zone = scope->name};

   auto listener = std::optional<forge::net::tcp::listener>{};
   try {
      listener.emplace(executor, requested);
   } catch (const forge::net::tcp::exceptions::listen_failed&) {
      BOOST_TEST_MESSAGE("native TCP does not support scoped loopback on this host");
      co_return;
   }

   const auto local = listener->local_endpoint();
   BOOST_CHECK_EQUAL(local.host, "::1");
   BOOST_CHECK(local.host.find('%') == std::string::npos);
   if (!local.zone.empty()) {
      BOOST_CHECK_EQUAL(local.zone, std::to_string(scope->index));
   }

   auto remote = requested;
   remote.port = local.port;
   auto accept = boost::asio::co_spawn(executor, listener->async_accept(), boost::asio::use_awaitable);
   auto connector = forge::net::tcp::connector{executor};
   auto client = co_await connector.async_connect(remote);
   auto server = co_await std::move(accept);

   BOOST_CHECK(client.remote_endpoint.host.find('%') == std::string::npos);
   BOOST_CHECK(server.local_endpoint.host.find('%') == std::string::npos);
   if (!client.remote_endpoint.zone.empty()) {
      BOOST_CHECK_EQUAL(client.remote_endpoint.zone, std::to_string(scope->index));
   }
   if (!server.local_endpoint.zone.empty()) {
      BOOST_CHECK_EQUAL(server.local_endpoint.zone, std::to_string(scope->index));
   }

   co_await client.stream.async_close();
   co_await server.stream.async_close();
   co_await listener->async_close();
#else
   BOOST_TEST_MESSAGE("native TCP scope validation is unsupported on this platform");
   co_return;
#endif
}

boost::asio::awaitable<void> tcp_invalid_endpoint_checks() {
   auto executor = co_await boost::asio::this_coro::executor;
   auto connector = forge::net::tcp::connector{executor};
   BOOST_CHECK_THROW((void)co_await connector.async_connect(invalid_quic_endpoint()),
                     forge::net::tcp::exceptions::invalid_endpoint);
   BOOST_CHECK_THROW((void)co_await connector.async_connect(loopback(0)),
                     forge::net::tcp::exceptions::invalid_endpoint);

   auto link_local = forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::ip6,
                                                      .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                                      .host = "fe80::1",
                                                      .port = 1};
   BOOST_CHECK_THROW((void)co_await connector.async_connect(link_local),
                     forge::net::tcp::exceptions::invalid_endpoint);

   auto dns_with_zone = dns4_loopback(1);
   dns_with_zone.zone = "1";
   BOOST_CHECK_THROW((void)co_await connector.async_connect(dns_with_zone),
                     forge::net::tcp::exceptions::invalid_endpoint);

   auto nul_host = loopback(1);
   nul_host.host = std::string{"127.0.0.1\0suffix", 16};
   BOOST_CHECK_THROW((void)co_await connector.async_connect(nul_host),
                     forge::net::tcp::exceptions::invalid_endpoint);

   auto dns_listen = forge::net::transport::endpoint{.host_type = forge::net::transport::endpoint::host_kind::dns,
                                                     .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
                                                     .host = "localhost",
                                                     .port = 0};
   BOOST_CHECK_THROW(((void)forge::net::tcp::listener{executor, dns_listen}),
                     forge::net::tcp::exceptions::invalid_endpoint);

   auto refused_probe = forge::net::tcp::listener{executor, loopback(0)};
   auto refused_endpoint = refused_probe.local_endpoint();
   co_await refused_probe.async_close();
   BOOST_CHECK_THROW((void)co_await connector.async_connect(refused_endpoint),
                     forge::net::tcp::exceptions::connect_failed);
}

} // namespace

BOOST_AUTO_TEST_SUITE(tcp)

BOOST_AUTO_TEST_CASE(tcp_stream_roundtrip_and_framing) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, tcp_roundtrip());
}

BOOST_AUTO_TEST_CASE(tcp_options_affect_stream_reads) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, tcp_read_chunk_limit_is_behavioral());
}

BOOST_AUTO_TEST_CASE(tcp_connection_supports_native_handoff) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, tcp_connection_roundtrip_and_handoff());
}

BOOST_AUTO_TEST_CASE(tcp_integrates_with_transport_registry) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, tcp_registry_roundtrip());
}

BOOST_AUTO_TEST_CASE(tcp_accept_can_be_canceled_or_closed) {
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, cancel_unblocks_accept(), std::chrono::seconds{2}));
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, close_unblocks_accept(), std::chrono::seconds{2}));
   forge::asio::blocking::run(runtime, close_releases_bound_port());
   BOOST_CHECK(
       forge::asio::blocking::run_for(runtime, connection_cancel_unblocks_pending_read(), std::chrono::seconds{2}));
   BOOST_CHECK(
       forge::asio::blocking::run_for(runtime, connector_cancel_rejects_future_connects(), std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_foreign_thread_connection_request_cancel_unblocks_read_with_typed_canceled) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, foreign_thread_connection_request_cancel_unblocks_pending_read(),
                                              std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_connection_request_cancel_before_terminal_worker_arm_is_sticky) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, connection_request_cancel_before_terminal_worker_arms_is_sticky(), std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_stream_request_cancel_before_terminal_worker_arm_is_sticky) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, stream_request_cancel_before_terminal_worker_arms_is_sticky(),
                                              std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_transport_stream_destruction_completes_owned_terminal_worker) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, transport_stream_destruction_completes_owned_terminal_worker(),
                                              std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_foreign_thread_listener_close_unblocks_accept_with_typed_closed) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, foreign_thread_listener_close_unblocks_pending_accept(),
                                              std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_active_io_rejects_native_handoff_without_moving_socket) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(
       forge::asio::blocking::run_for(runtime, active_connection_io_rejects_native_handoff(), std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_late_request_cancel_racing_handoff_preserves_transferred_native_ownership) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, late_request_cancel_does_not_close_handed_off_native_ownership(),
                                              std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(tcp_connector_request_cancel_before_terminal_worker_arm_is_sticky) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, connector_request_cancel_before_terminal_worker_arms_is_sticky(),
                                              std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_foreign_thread_connector_request_cancel_has_single_resolve_connect_handoff_winner) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, foreign_thread_connector_request_cancel_races_resolve_connect_handoff(), std::chrono::seconds{5}));
}

BOOST_AUTO_TEST_CASE(tcp_late_connector_request_cancel_preserves_handed_off_connection) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   BOOST_CHECK(forge::asio::blocking::run_for(
       runtime, late_foreign_thread_connector_request_cancel_preserves_handed_off_connection(),
       std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_scoped_loopback_uses_strict_literal_conversion_when_supported) {
   auto runtime = forge::asio::runtime{};
   BOOST_CHECK(forge::asio::blocking::run_for(runtime, tcp_scoped_loopback_preserves_numeric_reverse_zone(),
                                              std::chrono::seconds{2}));
}

BOOST_AUTO_TEST_CASE(tcp_rejects_invalid_endpoints_and_refused_connects) {
   auto runtime = forge::asio::runtime{};
   forge::asio::blocking::run(runtime, tcp_invalid_endpoint_checks());
}

BOOST_AUTO_TEST_SUITE_END()
