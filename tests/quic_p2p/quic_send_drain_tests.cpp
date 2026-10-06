#include <boost/test/unit_test.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <new>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/use_future.hpp>

#include "../../libraries/net/quic/details/engine_connection_impl.hxx"
#include "../../libraries/net/quic/details/engine_listener_impl.hxx"
#include "libp2p_identity_fixture.hxx"

import forge.asio.runtime;
import forge.net.quic.security;

namespace forge::net::quic::detail {
struct engine_connection_fixture {
   static std::shared_ptr<engine_connection::impl> state(const std::shared_ptr<engine_connection>& connection) {
      return connection->impl_;
   }
};
} // namespace forge::net::quic::detail

namespace {
namespace asio = boost::asio;
namespace detail = forge::net::quic::detail;
using namespace std::chrono_literals;

template <typename T> T completed(std::future<T>& future, std::string_view phase = "native operation") {
   BOOST_REQUIRE(future.wait_for(12s) == std::future_status::ready);
   try {
      return future.get();
   } catch (const detail::engine_failure& error) {
      BOOST_TEST_MESSAGE("QUIC phase=" << phase << " engine_failure.kind=" << static_cast<int>(error.kind())
                                       << " message=" << error.message());
      throw;
   }
}

template <typename Predicate>
asio::awaitable<void> observe(std::shared_ptr<detail::engine_connection::impl> owner, Predicate predicate) {
   const auto deadline = std::chrono::steady_clock::now() + 3s;
   while (true) {
      const auto epoch = owner->udp_send_changed.epoch();
      if (predicate()) {
         co_return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         throw boost::system::system_error{asio::error::timed_out, "QUIC send-drain observation timed out"};
      }
      static_cast<void>(co_await owner->udp_send_changed.async_wait_until(epoch, deadline));
   }
}

struct udp_send_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   asio::ip::udp::socket receiver{runtime.context(), {asio::ip::address_v4::loopback(), 0}};
   std::shared_ptr<asio::ip::udp::socket> socket = std::make_shared<asio::ip::udp::socket>(
       runtime.context(), asio::ip::udp::endpoint{asio::ip::address_v4::loopback(), 0});
   std::shared_ptr<detail::engine_connection::impl> owner;
   forge::asio::gate::ticket held;

   udp_send_fixture() {
      socket->connect(receiver.local_endpoint());
      owner = std::make_shared<detail::engine_connection::impl>(runtime.context(), socket, socket->local_endpoint(),
                                                                receiver.local_endpoint(),
                                                                detail::engine_transport_limits{});
      owner->self = owner;
      auto gate = asio::co_spawn(owner->strand, owner->send_gate.acquire(), asio::use_future);
      held = completed(gate);
   }

   ~udp_send_fixture() {
      held.release();
      auto cleanup = asio::co_spawn(
          owner->strand,
          [owner = owner]() -> asio::awaitable<void> {
             owner->fail_all();
             co_await owner->wait_background_idle();
          },
          asio::use_future);
      BOOST_CHECK(cleanup.wait_for(8s) == std::future_status::ready);
      cleanup.wait();
      BOOST_CHECK_NO_THROW(cleanup.get());
   }

   std::uint64_t enqueue(std::uint8_t byte) {
      auto queued = asio::co_spawn(
          owner->strand,
          [owner = owner, byte]() -> asio::awaitable<std::uint64_t> {
             auto path = detail::make_path(owner->local_endpoint_value, owner->remote_endpoint);
             const auto packet = std::array<std::uint8_t, 1>{byte};
             owner->enqueue_datagram(packet, path.path);
             co_return owner->udp_enqueued_generation;
          },
          asio::use_future);
      return completed(queued);
   }

   std::future<void> drain(std::uint64_t generation, std::chrono::milliseconds budget = 2s) {
      const auto deadline = std::chrono::steady_clock::now() + budget;
      return asio::co_spawn(owner->strand, owner->wait_udp_send_prefix(generation, deadline), asio::use_future);
   }
};

struct quic_send_fixture {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   std::shared_ptr<detail::engine_listener::impl> listener;
   detail::engine_connector connector{runtime.context()};
   std::shared_ptr<detail::engine_connection> client;
   std::shared_ptr<detail::engine_connection> server;
   std::shared_ptr<detail::engine_connection::impl> owner;
   std::shared_ptr<detail::engine_stream> client_stream;
   std::shared_ptr<detail::engine_stream> server_stream;
   std::shared_ptr<detail::engine_stream::impl> stream_owner;
   forge::asio::gate::ticket held;
   std::optional<boost::system::error_code> expected_close_error;
   bool expected_close_allocation_failure = false;

   quic_send_fixture() {
      const auto client_identity = forge::tests::p2p::make_identity_fixture("send-drain-client");
      const auto server_identity = forge::tests::p2p::make_identity_fixture("send-drain-server");
      auto server_options = detail::engine_server_options{};
      server_options.certificate_pem = server_identity.certificate_pem;
      server_options.private_key_pem = server_identity.private_key_pem;
      server_options.security.expected_sha256_fingerprint =
          forge::net::quic::certificate_sha256_fingerprint_from_pem(client_identity.certificate_pem);
      listener = std::make_shared<detail::engine_listener::impl>(
          runtime.context(), detail::engine_endpoint{.host = "127.0.0.1"}, std::move(server_options));
      listener->self = listener;
      auto started = asio::co_spawn(
          listener->strand,
          [listener = listener]() -> asio::awaitable<void> {
             listener->server_socket->open_and_bind(
                 {detail::literal_address(listener->bind_endpoint), listener->bind_endpoint.port});
             listener->bind_endpoint = detail::from_udp_endpoint(listener->server_socket->local_endpoint());
             listener->start();
             co_return;
          },
          asio::use_future);
      completed(started, "listener.open_bind_start");
      BOOST_REQUIRE_GT(listener->bind_endpoint.port, 0U);
      auto options = detail::engine_client_options{};
      options.connect_timeout = 3s;
      options.handshake_timeout = 2s;
      options.certificate_pem = client_identity.certificate_pem;
      options.private_key_pem = client_identity.private_key_pem;
      options.security.expected_sha256_fingerprint =
          forge::net::quic::certificate_sha256_fingerprint_from_pem(server_identity.certificate_pem);
      auto dial =
          asio::co_spawn(runtime.context(),
                         connector.async_connect(detail::from_udp_endpoint(listener->server_socket->local_endpoint()),
                                                 std::move(options)),
                         asio::use_future);
      client = completed(dial, "client.connect_authenticated");
      auto accepted = asio::co_spawn(
          listener->strand,
          [listener = listener]() -> asio::awaitable<std::shared_ptr<detail::engine_connection>> {
             const auto deadline = std::chrono::steady_clock::now() + 3s;
             while (listener->accepted.empty()) {
                auto timer = std::make_shared<asio::steady_timer>(listener->strand);
                timer->expires_at(deadline);
                listener->accept_waiters.emplace_back(timer);
                auto error = boost::system::error_code{};
                co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
                detail::remove_waiter(listener->accept_waiters, timer);
                if (listener->accepted.empty() && std::chrono::steady_clock::now() >= deadline) {
                   throw boost::system::system_error{asio::error::timed_out};
                }
             }
             auto connection = std::move(listener->accepted.front());
             listener->accepted.pop_front();
             co_return connection;
          },
          asio::use_future);
      server = completed(accepted, "server.accept_authenticated");
      BOOST_REQUIRE(client->peer_certificate().has_value());
      BOOST_REQUIRE(server->peer_certificate().has_value());
      const auto connections = listener->connections();
      BOOST_REQUIRE_EQUAL(connections.size(), 1U);
      owner = connections.front();
      auto opened = asio::co_spawn(runtime.context(), client->async_open_stream(), asio::use_future);
      client_stream = completed(opened, "client.open_seed_stream");
      auto incoming = asio::co_spawn(runtime.context(), server->async_accept_stream(), asio::use_future);
      auto seed = asio::co_spawn(
          runtime.context(),
          [stream = client_stream]() -> asio::awaitable<void> {
             const auto bytes = std::array<std::uint8_t, 1>{0x41};
             co_await stream->async_write(bytes);
          },
          asio::use_future);
      completed(seed, "client.write_seed");
      server_stream = completed(incoming, "server.accept_seed_stream");
      auto read = asio::co_spawn(runtime.context(), server_stream->async_read(), asio::use_future);
      BOOST_TEST(completed(read, "server.read_seed") == std::vector<std::uint8_t>{0x41},
                 boost::test_tools::per_element());
      auto baseline = asio::co_spawn(
          owner->strand,
          [owner = owner, id = server_stream->id()]() -> asio::awaitable<std::shared_ptr<detail::engine_stream::impl>> {
             co_await owner->wait_udp_send_prefix(owner->udp_enqueued_generation,
                                                  std::chrono::steady_clock::now() + 2s);
             co_return owner->streams.at(id);
          },
          asio::use_future);
      stream_owner = completed(baseline, "server.capture_native_stream_and_baseline");
      auto gate = asio::co_spawn(owner->strand, owner->send_gate.acquire(), asio::use_future);
      held = completed(gate, "server.hold_send_gate");
   }

   ~quic_send_fixture() {
      held.release();
      for (const auto& connection : {server, client}) {
         if (!connection) {
            continue;
         }
         connection->request_cancel();
         auto close = asio::co_spawn(runtime.context(), connection->async_close(), asio::use_future);
         BOOST_CHECK(close.wait_for(8s) == std::future_status::ready);
         close.wait();
         try {
            close.get();
         } catch (const detail::engine_failure& error) {
            BOOST_CHECK(error.kind() == detail::engine_error_kind::canceled ||
                        error.kind() == detail::engine_error_kind::connection_closed);
         } catch (const boost::system::system_error& error) {
            // Cancellation and a previously tested close timeout remain sticky.
            BOOST_CHECK(error.code() == asio::error::operation_aborted || error.code() == asio::error::timed_out ||
                        (expected_close_error && error.code() == *expected_close_error));
         } catch (const std::bad_alloc&) {
            BOOST_CHECK(expected_close_allocation_failure);
         } catch (...) {
            BOOST_ERROR("unexpected QUIC connection cleanup error");
         }
      }
      auto stopped = asio::co_spawn(
          listener->strand,
          [listener = listener]() -> asio::awaitable<void> {
             listener->stop();
             co_await listener->wait_operations_idle();
          },
          asio::use_future);
      BOOST_CHECK(stopped.wait_for(8s) == std::future_status::ready);
      stopped.wait();
      BOOST_CHECK_NO_THROW(stopped.get());
   }

   void queue(std::size_t size = 64) {
      auto write = asio::co_spawn(
          runtime.context(),
          [stream = server_stream, size]() -> asio::awaitable<void> {
             const auto bytes = std::vector<std::uint8_t>(size, 0x52);
             co_await stream->async_write(bytes);
          },
          asio::use_future);
      completed(write);
   }

   template <typename Predicate> void until(Predicate predicate) {
      auto ready = asio::co_spawn(owner->strand, observe(owner, std::move(predicate)), asio::use_future);
      completed(ready);
   }

   void require_sender_joined() {
      auto joined = asio::co_spawn(
          owner->strand,
          [owner = owner]() -> asio::awaitable<bool> {
             co_return !owner->udp_send_active && owner->udp_inflight_generation == 0 &&
                 owner->background_jobs.load(std::memory_order_acquire) == 0;
          },
          asio::use_future);
      BOOST_TEST(completed(joined));
   }
};

BOOST_AUTO_TEST_CASE(quic_udp_send_prefix_includes_popped_inflight_datagram) {
   auto fixture = udp_send_fixture{};
   const auto prefix = fixture.enqueue(0x51);
   auto draining = fixture.drain(prefix);
   auto ready =
       asio::co_spawn(fixture.owner->strand,
                      observe(fixture.owner,
                              [owner = fixture.owner, prefix] {
                                 return owner->udp_inflight_generation == prefix && owner->outbound_datagrams.empty();
                              }),
                      asio::use_future);
   completed(ready);
   BOOST_CHECK(draining.wait_for(0ms) != std::future_status::ready);
   fixture.held.release();
   BOOST_CHECK_NO_THROW(completed(draining));
   auto read = asio::co_spawn(
       fixture.runtime.context(),
       [&fixture]() -> asio::awaitable<std::uint8_t> {
          using namespace asio::experimental::awaitable_operators;
          auto byte = std::array<std::uint8_t, 1>{};
          auto timer = asio::steady_timer{fixture.runtime.context()};
          timer.expires_after(1s);
          auto result = co_await (fixture.receiver.async_receive(asio::buffer(byte), asio::use_awaitable) ||
                                  timer.async_wait(asio::use_awaitable));
          if (result.index() != 0) {
             throw boost::system::system_error{asio::error::timed_out};
          }
          co_return byte.front();
       },
       asio::use_future);
   BOOST_TEST(completed(read) == 0x51U);
}

BOOST_AUTO_TEST_CASE(quic_udp_send_prefix_does_not_wait_for_later_inflight_packet) {
   auto fixture = udp_send_fixture{};
   const auto prefix = fixture.enqueue(0x61);
   auto started = asio::co_spawn(
       fixture.owner->strand,
       observe(fixture.owner, [owner = fixture.owner, prefix] { return owner->udp_inflight_generation == prefix; }),
       asio::use_future);
   completed(started);
   auto later_gate = asio::co_spawn(fixture.owner->strand, fixture.owner->send_gate.acquire(), asio::use_future);
   const auto later = fixture.enqueue(0x62);
   auto draining = fixture.drain(prefix);
   fixture.held.release();
   fixture.held = completed(later_gate);
   BOOST_CHECK_NO_THROW(completed(draining));
   auto observed = asio::co_spawn(fixture.owner->strand,
                                  observe(fixture.owner,
                                          [owner = fixture.owner, prefix, later] {
                                             return owner->udp_completed_generation == prefix &&
                                                    owner->udp_inflight_generation == later;
                                          }),
                                  asio::use_future);
   completed(observed);
   auto later_drain = fixture.drain(later);
   BOOST_CHECK(later_drain.wait_for(0ms) != std::future_status::ready);
   fixture.held.release();
   BOOST_CHECK_NO_THROW(completed(later_drain));
   auto replay = fixture.drain(prefix);
   BOOST_CHECK_NO_THROW(completed(replay));
   auto gate = asio::co_spawn(fixture.owner->strand, fixture.owner->send_gate.acquire(), asio::use_future);
   fixture.held = completed(gate);
   const auto failed = fixture.enqueue(0x63);
   auto closed = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<void> {
          owner->socket->close();
          co_return;
       },
       asio::use_future);
   completed(closed);
   auto failed_drain = fixture.drain(failed);
   fixture.held.release();
   BOOST_CHECK_EXCEPTION(completed(failed_drain), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::bad_descriptor; });
   auto earlier_after_failure = fixture.drain(prefix);
   BOOST_CHECK_NO_THROW(completed(earlier_after_failure));
}

BOOST_AUTO_TEST_CASE(quic_udp_send_prefix_timeout_never_advances_completed_generation) {
   auto fixture = udp_send_fixture{};
   const auto prefix = fixture.enqueue(0x71);
   auto draining = fixture.drain(prefix, 30ms);
   BOOST_CHECK_EXCEPTION(completed(draining), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::timed_out; });
   auto observed = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<std::uint64_t> { co_return owner->udp_completed_generation; },
       asio::use_future);
   BOOST_TEST(completed(observed) == 0U);
   fixture.held.release();
   auto delivered = fixture.drain(prefix);
   BOOST_CHECK_NO_THROW(completed(delivered));
}

BOOST_AUTO_TEST_CASE(quic_udp_send_failure_is_sticky_and_not_counted_as_sent) {
   auto fixture = udp_send_fixture{};
   const auto prefix = fixture.enqueue(0x81);
   auto started = asio::co_spawn(
       fixture.owner->strand,
       observe(fixture.owner, [owner = fixture.owner, prefix] { return owner->udp_inflight_generation == prefix; }),
       asio::use_future);
   completed(started);
   auto closed = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<void> {
          owner->socket->close();
          co_return;
       },
       asio::use_future);
   completed(closed);
   auto draining = fixture.drain(prefix);
   fixture.held.release();
   BOOST_CHECK_EXCEPTION(completed(draining), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::bad_descriptor; });
   auto replay = fixture.drain(prefix);
   BOOST_CHECK_EXCEPTION(completed(replay), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::bad_descriptor; });
   auto joined = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_await owner->wait_udp_send_idle();
          co_return owner->udp_completed_generation == 0 && owner->udp_inflight_generation == 0 &&
              owner->outbound_datagrams.empty();
       },
       asio::use_future);
   BOOST_TEST(completed(joined));
}

BOOST_AUTO_TEST_CASE(quic_fin_close_waits_for_actual_gated_socket_send) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server_stream->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner, stream = fixture.stream_owner] {
      return stream->fin_send_generation != 0 && owner->udp_inflight_generation != 0 &&
             owner->udp_completed_generation < stream->fin_send_generation;
   });
   BOOST_CHECK(close.wait_for(0ms) != std::future_status::ready);
   auto timer_ran = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          auto timer = asio::steady_timer{owner->strand};
          timer.expires_after(10ms);
          co_await timer.async_wait(asio::use_awaitable);
          co_return owner->udp_inflight_generation != 0;
       },
       asio::use_future);
   BOOST_TEST(completed(timer_ran));
   fixture.held.release();
   BOOST_CHECK_NO_THROW(completed(close));
   auto read = asio::co_spawn(fixture.runtime.context(), fixture.client_stream->async_read(), asio::use_future);
   BOOST_TEST(completed(read) == std::vector<std::uint8_t>(64, 0x52), boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(quic_reset_close_joins_queued_udp_prefix_without_a_fin) {
   auto fixture = quic_send_fixture{};
   fixture.server_stream->request_cancel();
   fixture.until([owner = fixture.owner, stream = fixture.stream_owner] {
      return stream->reset && !stream->fin_queued && stream->terminal_cleanup_owners != 0 &&
             owner->udp_inflight_generation != 0;
   });
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server_stream->async_close(), asio::use_future);
   BOOST_CHECK(close.wait_for(0ms) != std::future_status::ready);
   fixture.held.release();
   BOOST_CHECK_NO_THROW(completed(close));
   auto terminal = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, stream = fixture.stream_owner]() -> asio::awaitable<bool> {
          co_return stream->terminal_cleanup_owners == 0 && stream->terminal_cleanup_complete.load() &&
              owner->udp_completed_generation != 0 && !owner->closing && !owner->canceled;
       },
       asio::use_future);
   BOOST_TEST(completed(terminal));
}

BOOST_AUTO_TEST_CASE(quic_reset_partial_enqueue_failure_captures_new_prefix_and_joins_sender) {
   auto fixture = quic_send_fixture{};
   auto armed = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, stream = fixture.stream_owner]() -> asio::awaitable<std::uint64_t> {
          owner->test_failpoint = [stream](std::string_view name) {
             if (stream->reset && name == "drain_after_udp_enqueue") {
                throw std::bad_alloc{};
             }
             return false;
          };
          co_return owner->udp_enqueued_generation;
       },
       asio::use_future);
   const auto before = completed(armed);
   fixture.server_stream->request_cancel();
   fixture.until([owner = fixture.owner, stream = fixture.stream_owner, before] {
      return stream->terminal_cleanup_error && owner->udp_enqueued_generation > before && !owner->udp_send_active;
   });
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server_stream->async_close(), asio::use_future);
   BOOST_CHECK_THROW(completed(close), std::bad_alloc);
   auto joined = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, stream = fixture.stream_owner]() -> asio::awaitable<bool> {
          owner->test_failpoint = {};
          co_await owner->wait_background_idle();
          co_return owner->udp_completed_generation < owner->udp_enqueued_generation &&
              owner->udp_inflight_generation == 0 && owner->outbound_datagrams.empty() &&
              stream->terminal_cleanup_owners == 0;
       },
       asio::use_future);
   BOOST_TEST(completed(joined));
   fixture.require_sender_joined();
}

BOOST_AUTO_TEST_CASE(quic_fin_submission_timeout_preserves_neighbor_and_original_error_after_reset_cleanup) {
   auto fixture = quic_send_fixture{};
   fixture.held.release();
   auto incoming = asio::co_spawn(fixture.runtime.context(), fixture.client->async_accept_stream(), asio::use_future);
   auto opened = asio::co_spawn(fixture.runtime.context(), fixture.server->async_open_stream(), asio::use_future);
   auto neighbor = completed(opened);
   auto seed = asio::co_spawn(
       fixture.runtime.context(),
       [neighbor]() -> asio::awaitable<void> {
          const auto bytes = std::array<std::uint8_t, 1>{0x53};
          co_await neighbor->async_write(bytes);
       },
       asio::use_future);
   completed(seed);
   auto received_neighbor = completed(incoming);
   auto baseline = asio::co_spawn(fixture.runtime.context(), received_neighbor->async_read(), asio::use_future);
   BOOST_TEST(completed(baseline) == std::vector<std::uint8_t>{0x53}, boost::test_tools::per_element());
   auto paused = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, stream = fixture.stream_owner]() -> asio::awaitable<void> {
          owner->test_failpoint = [stream](std::string_view name) {
             return !stream->reset && (name == "drain_after_owner_claim_before_native_write" ||
                                       name == "drain_after_owner_claim_before_native_write_wait");
          };
          co_return;
       },
       asio::use_future);
   completed(paused);
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server_stream->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(close), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::timed_out; });
   auto healthy = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, stream = fixture.stream_owner]() -> asio::awaitable<bool> {
          owner->test_failpoint = {};
          co_return !owner->close_started && !owner->closing && !owner->canceled && stream->reset &&
              stream->terminal_cleanup_owners == 0 && !stream->terminal_cleanup_error;
       },
       asio::use_future);
   BOOST_TEST(completed(healthy));
   auto write = asio::co_spawn(
       fixture.runtime.context(),
       [neighbor]() -> asio::awaitable<void> {
          const auto bytes = std::array<std::uint8_t, 1>{0x54};
          co_await neighbor->async_write(bytes);
       },
       asio::use_future);
   completed(write);
   auto read = asio::co_spawn(fixture.runtime.context(), received_neighbor->async_read(), asio::use_future);
   BOOST_TEST(completed(read) == std::vector<std::uint8_t>{0x54}, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(quic_connection_close_seals_admission_and_drains_queued_and_inflight_prefix) {
   auto fixture = quic_send_fixture{};
   constexpr auto payload_size = std::size_t{3000};
   fixture.queue(payload_size);
   fixture.until([owner = fixture.owner, stream = fixture.stream_owner] {
      return owner->udp_inflight_generation != 0 && !owner->outbound_datagrams.empty() &&
             stream->send_next_offset == payload_size && stream->outbound.empty();
   });
   auto receipt = std::make_shared<std::optional<std::array<std::uint64_t, 3>>>();
   auto captured_changed = std::make_shared<forge::asio::notification>();
   const auto observed = captured_changed->epoch();
   auto armed = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, stream = fixture.stream_owner, receipt, captured_changed]() -> asio::awaitable<void> {
          owner->test_failpoint = [owner = owner.get(), stream, receipt, captured_changed](std::string_view name) {
             if (name == "async_close_after_udp_prefix_capture") {
                *receipt = std::array<std::uint64_t, 3>{owner->udp_enqueued_generation, stream->send_next_offset,
                                                        stream->outbound.size()};
                captured_changed->notify();
             }
             return false;
          };
          co_return;
       },
       asio::use_future);
   completed(armed);
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && !owner->closing; });
   auto opened = asio::co_spawn(fixture.runtime.context(), fixture.server->async_open_stream(), asio::use_future);
   BOOST_CHECK_EXCEPTION(static_cast<void>(completed(opened)), detail::engine_failure, [](const auto& error) {
      return error.kind() == detail::engine_error_kind::connection_closed;
   });
   auto write = asio::co_spawn(
       fixture.runtime.context(),
       [stream = fixture.server_stream]() -> asio::awaitable<void> {
          const auto bytes = std::array<std::uint8_t, 1>{0x53};
          co_await stream->async_write(bytes);
       },
       asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(write), detail::engine_failure, [](const auto& error) {
      return error.kind() == detail::engine_error_kind::connection_closed;
   });
   auto captured = asio::co_spawn(
       fixture.owner->strand,
       [receipt, captured_changed, observed]() -> asio::awaitable<std::array<std::uint64_t, 3>> {
          auto epoch = observed;
          const auto deadline = std::chrono::steady_clock::now() + 3s;
          while (!receipt->has_value()) {
             if (std::chrono::steady_clock::now() >= deadline) {
                throw boost::system::system_error{asio::error::timed_out, "QUIC close prefix was not captured"};
             }
             epoch = co_await captured_changed->async_wait_until(epoch, deadline);
          }
          co_return **receipt;
       },
       asio::use_future);
   const auto actual = completed(captured);
   const auto prefix = actual[0];
   BOOST_TEST(prefix > 0U);
   BOOST_TEST(actual[1] == payload_size);
   BOOST_TEST(actual[2] == 0U);
   BOOST_CHECK(close.wait_for(0ms) != std::future_status::ready);
   fixture.held.release();
   BOOST_CHECK_NO_THROW(completed(close));
   auto sent = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, prefix]() -> asio::awaitable<bool> {
          owner->test_failpoint = {};
          co_return owner->udp_completed_generation >= prefix && owner->outbound_datagrams.empty() &&
              !owner->native_remote_close_received && !owner->canceled;
       },
       asio::use_future);
   BOOST_TEST(completed(sent));
   fixture.require_sender_joined();
}

BOOST_AUTO_TEST_CASE(quic_connection_close_cancel_joins_sender_without_closing_shared_listener_fd) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   fixture.server->request_cancel();
   BOOST_CHECK_EXCEPTION(completed(close), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
   fixture.require_sender_joined();
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
   auto probe = asio::ip::udp::socket{fixture.runtime.context()};
   probe.open(asio::ip::udp::v4());
   auto error = boost::system::error_code{};
   probe.bind(fixture.listener->server_socket->local_endpoint(), error);
   BOOST_TEST(error == asio::error::address_in_use);
}

BOOST_AUTO_TEST_CASE(quic_connection_close_timeout_joins_sender_and_keeps_error_for_other_callers) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   BOOST_CHECK_EXCEPTION(completed(close), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::timed_out; });
   fixture.require_sender_joined();
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::timed_out; });
}

BOOST_AUTO_TEST_CASE(quic_connection_close_public_cancel_normalizes_only_authored_discard_after_join) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   fixture.server->cancel();
   fixture.server->cancel();
   BOOST_CHECK_EXCEPTION(completed(close), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
   fixture.require_sender_joined();
   auto receipt = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_return owner->udp_send_discarded && owner->udp_send_error == asio::error::operation_aborted &&
              !owner->udp_send_exception && owner->udp_failed_generation != 0 &&
              owner->udp_failed_generation <= owner->udp_enqueued_generation &&
              owner->cancellation_requested.load(std::memory_order_acquire) && !owner->native_remote_close_received &&
              owner->metrics.cancellations.load(std::memory_order_relaxed) == 1U &&
              owner->canceled && owner->terminal_cleanup_complete && owner->close_cleanup_complete &&
              owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(receipt));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
   fixture.require_sender_joined();
}

BOOST_AUTO_TEST_CASE(quic_connection_close_shared_socket_abort_before_owner_terminal_stays_a_real_failure) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   auto stop_socket = asio::co_spawn(
       fixture.listener->strand,
       [listener = fixture.listener]() -> asio::awaitable<void> {
          // Exercise the real shared-socket stop before connection teardown,
          // not a manufactured error or a connection cancel flag.
          listener->server_socket->stop();
          co_return;
       },
       asio::use_future);
   completed(stop_socket);
   fixture.held.release();
   BOOST_CHECK_EXCEPTION(completed(close), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::operation_aborted; });
   fixture.require_sender_joined();
   auto receipt = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_return !owner->udp_send_discarded && owner->udp_send_error == asio::error::operation_aborted &&
              !owner->udp_send_exception &&
              owner->canceled && owner->terminal_cleanup_complete && owner->close_cleanup_complete &&
              !owner->cancellation_requested.load(std::memory_order_acquire) && !owner->native_remote_close_received &&
              owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(receipt));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::operation_aborted; });
   fixture.require_sender_joined();
}

BOOST_AUTO_TEST_CASE(quic_connection_close_atomic_cancel_before_udp_failure_report_is_owned_discard) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   auto observed = std::make_shared<bool>(false);
   auto armed = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, connection = fixture.server, observed]() -> asio::awaitable<void> {
          owner->test_failpoint = [owner = owner.get(), connection = std::weak_ptr{connection},
                                   observed](std::string_view name) {
             if (name == "udp_send_failure_before_report") {
                BOOST_TEST(!owner->terminal_signaled.load(std::memory_order_acquire));
                BOOST_TEST(!owner->canceled);
                BOOST_TEST(!owner->terminal_cleanup_complete);
                // Publish a real cancellation at the failure-report barrier.
                // The posted terminal worker cannot run before this producer
                // resumes, so only the atomic request can classify the abort.
                const auto transport = connection.lock();
                BOOST_REQUIRE(transport);
                transport->request_cancel();
                *observed = owner->cancellation_requested.load(std::memory_order_acquire) &&
                            !owner->terminal_signaled.load(std::memory_order_acquire);
             }
             return false;
          };
          co_return;
       },
       asio::use_future);
   completed(armed);
   auto stop_socket = asio::co_spawn(
       fixture.listener->strand,
       [listener = fixture.listener]() -> asio::awaitable<void> {
          listener->server_socket->stop();
          co_return;
       },
       asio::use_future);
   completed(stop_socket);
   fixture.held.release();
   BOOST_CHECK_EXCEPTION(completed(close), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
   fixture.require_sender_joined();
   auto receipt = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, observed]() -> asio::awaitable<bool> {
          owner->test_failpoint = {};
          co_return *observed && owner->udp_send_discarded && owner->udp_send_error == asio::error::operation_aborted &&
              !owner->udp_send_exception && owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(receipt));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
}

BOOST_AUTO_TEST_CASE(quic_connection_close_listener_stop_publishes_cancel_before_shared_socket_abort) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   auto stop_listener = asio::co_spawn(
       fixture.listener->strand,
       [listener = fixture.listener]() -> asio::awaitable<void> {
          listener->stop();
          co_return;
       },
       asio::use_future);
   completed(stop_listener);
   BOOST_CHECK_EXCEPTION(completed(close), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
   fixture.require_sender_joined();
   auto receipt = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_return owner->cancellation_requested.load(std::memory_order_acquire) && owner->udp_send_discarded &&
              owner->udp_send_error == asio::error::operation_aborted && !owner->udp_send_exception &&
              owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(receipt));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), detail::engine_failure,
                         [](const auto& error) { return error.kind() == detail::engine_error_kind::canceled; });
}

BOOST_AUTO_TEST_CASE(quic_connection_close_preserves_real_peer_port_loss_over_cleanup_discard) {
   auto fixture = quic_send_fixture{};
   fixture.held.release();
   const auto owner = detail::engine_connection_fixture::state(fixture.client);
   auto gate = asio::co_spawn(owner->strand, owner->send_gate.acquire(), asio::use_future);
   auto held = completed(gate);
   auto write = asio::co_spawn(
       fixture.runtime.context(),
       [stream = fixture.client_stream]() -> asio::awaitable<void> {
          const auto bytes = std::array<std::uint8_t, 32>{};
          co_await stream->async_write(bytes);
       },
       asio::use_future);
   completed(write);
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.client->async_close(), asio::use_future);
   auto active = asio::co_spawn(
       owner->strand,
       observe(owner, [owner] { return owner->close_started && owner->udp_inflight_generation != 0; }),
       asio::use_future);
   completed(active);
   auto stop_peer = asio::co_spawn(
       fixture.listener->strand,
       [listener = fixture.listener]() -> asio::awaitable<void> {
          listener->stop();
          co_return;
       },
       asio::use_future);
   completed(stop_peer);
   auto probe = asio::co_spawn(
       owner->strand,
       [owner]() -> asio::awaitable<void> {
          // Fault injection on the real authenticated connection's UDP socket:
          // obtain an OS receive error while the captured engine prefix stays
          // blocked. This probe is not an acknowledged engine packet.
          const auto byte = std::array<std::uint8_t, 1>{0x01};
          auto error = boost::system::error_code{};
          static_cast<void>(co_await owner->socket->async_send(
              asio::buffer(byte), asio::redirect_error(asio::use_awaitable, error)));
       },
       asio::use_future);
   completed(probe);
   auto actual_error = boost::system::error_code{};
   BOOST_CHECK_EXCEPTION(completed(close), boost::system::system_error, [&actual_error](const auto& error) {
      actual_error = error.code();
      return actual_error == asio::error::connection_refused || actual_error == asio::error::connection_reset;
   });
   auto receipt = asio::co_spawn(
       owner->strand,
       [owner, actual_error]() -> asio::awaitable<bool> {
          co_return owner->handshake_done && owner->udp_transport_error == actual_error && actual_error &&
              owner->udp_send_discarded && owner->udp_send_error == asio::error::operation_aborted &&
              !owner->udp_send_exception && !owner->native_remote_close_received &&
              !owner->cancellation_requested.load(std::memory_order_acquire) && owner->background_jobs == 0 &&
              owner->terminal_cleanup_complete && owner->close_cleanup_complete &&
              owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(receipt));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.client->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), boost::system::system_error,
                         [actual_error](const auto& error) { return error.code() == actual_error; });
   fixture.expected_close_error = actual_error;
}

BOOST_AUTO_TEST_CASE(quic_connection_close_failed_background_join_is_never_normalized_to_cancel) {
   auto fixture = quic_send_fixture{};
   fixture.expected_close_allocation_failure = true;
   fixture.queue();
   auto active_jobs = std::make_shared<std::size_t>(0);
   auto armed_waits = std::make_shared<std::size_t>(0);
   auto worker_entered = std::make_shared<forge::asio::notification>();
   auto worker_released = std::make_shared<forge::asio::notification>();
   auto join_failed = std::make_shared<forge::asio::notification>();
   auto release_worker = [worker_released](forge::asio::notification*) noexcept { worker_released->notify(); };
   auto release_on_exit =
       std::unique_ptr<forge::asio::notification, decltype(release_worker)>{worker_released.get(), release_worker};
   auto armed = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, active_jobs, armed_waits, worker_entered, worker_released,
        join_failed]() -> asio::awaitable<void> {
          owner->spawn_background([worker_entered, worker_released](const auto&) -> asio::awaitable<void> {
             worker_entered->notify();
             static_cast<void>(co_await worker_released->async_wait_until(0, std::chrono::steady_clock::now() + 8s));
          });
          owner->test_failpoint = [owner = owner.get(), active_jobs, armed_waits, join_failed](std::string_view name) {
             if (name == "async_close_terminal_wait_armed") {
                ++*armed_waits;
                owner->udp_send_changed.notify();
             }
             if (name == "background_join_before_waiter_allocation" && *active_jobs == 0) {
                *active_jobs = owner->background_jobs.load(std::memory_order_acquire);
                join_failed->notify();
                throw std::bad_alloc{};
             }
             return false;
          };
          co_return;
       },
       asio::use_future);
   completed(armed);
   auto entered =
       asio::co_spawn(fixture.runtime.context(),
                      worker_entered->async_wait_until(0, std::chrono::steady_clock::now() + 3s), asio::use_future);
   BOOST_TEST(completed(entered) > 0U);
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   auto concurrent = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([armed_waits] { return *armed_waits == 2; });
   fixture.server->request_cancel();
   auto failed =
       asio::co_spawn(fixture.runtime.context(),
                      join_failed->async_wait_until(0, std::chrono::steady_clock::now() + 3s), asio::use_future);
   BOOST_TEST(completed(failed) > 0U);
   auto pending = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, active_jobs]() -> asio::awaitable<bool> {
          co_return *active_jobs > 0 && owner->background_jobs.load(std::memory_order_acquire) > 0 &&
              !owner->close_cleanup_complete;
       },
       asio::use_future);
   BOOST_TEST(completed(pending));
   BOOST_CHECK(close.wait_for(0ms) != std::future_status::ready);
   BOOST_CHECK(concurrent.wait_for(0ms) != std::future_status::ready);
   release_on_exit.reset();
   BOOST_CHECK_THROW(completed(close), std::bad_alloc);
   fixture.require_sender_joined();
   BOOST_CHECK_THROW(completed(concurrent), std::bad_alloc);
   fixture.require_sender_joined();
   auto state = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, active_jobs]() -> asio::awaitable<bool> {
          co_return *active_jobs > 0 && owner->close_cleanup_complete && !owner->close_completion_pending &&
              !owner->close_work_active;
       },
       asio::use_future);
   BOOST_TEST(completed(state));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_THROW(completed(replay), std::bad_alloc);
   fixture.require_sender_joined();
}

BOOST_AUTO_TEST_CASE(quic_close_error_publication_waits_for_actual_background_owner) {
   auto fixture = udp_send_fixture{};
   const auto prefix = fixture.enqueue(0x83);
   auto started = asio::co_spawn(fixture.owner->strand,
                                 observe(fixture.owner,
                                         [owner = fixture.owner, prefix] {
                                            return owner->udp_inflight_generation == prefix &&
                                                   owner->background_jobs.load(std::memory_order_acquire) > 0;
                                         }),
                                 asio::use_future);
   completed(started);
   auto pending = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          owner->complete_close(std::make_exception_ptr(std::bad_alloc{}));
          co_return owner->close_completion_pending && !owner->close_cleanup_complete;
       },
       asio::use_future);
   BOOST_TEST(completed(pending));
   auto close = asio::co_spawn(fixture.owner->strand, fixture.owner->wait_close_cleanup(), asio::use_future);
   BOOST_CHECK(close.wait_for(0ms) != std::future_status::ready);
   fixture.held.release();
   BOOST_CHECK_THROW(completed(close), std::bad_alloc);
   auto completed_state = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, prefix]() -> asio::awaitable<bool> {
          co_return owner->close_cleanup_complete && !owner->close_completion_pending &&
              owner->background_jobs.load(std::memory_order_acquire) == 0 && !owner->udp_send_active &&
              owner->udp_inflight_generation == 0 && owner->udp_completed_generation == prefix;
       },
       asio::use_future);
   BOOST_TEST(completed(completed_state));
   auto replay = asio::co_spawn(fixture.owner->strand, fixture.owner->wait_close_cleanup(), asio::use_future);
   BOOST_CHECK_THROW(completed(replay), std::bad_alloc);
}

BOOST_AUTO_TEST_CASE(quic_connection_close_after_prior_cancel_is_idempotent_unsent_cleanup) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   fixture.until([owner = fixture.owner] { return owner->udp_inflight_generation != 0; });
   fixture.server->request_cancel();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(close));
   fixture.require_sender_joined();
   auto state = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_return owner->canceled && owner->terminal_cleanup_complete && owner->udp_send_discarded &&
              owner->udp_completed_generation < owner->udp_enqueued_generation && owner->outbound_datagrams.empty();
       },
       asio::use_future);
   BOOST_TEST(completed(state));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(replay));
}

BOOST_AUTO_TEST_CASE(quic_connection_close_after_native_peer_close_joins_without_delivery_claim) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   fixture.until([owner = fixture.owner] { return owner->udp_inflight_generation != 0; });
   auto remote = asio::co_spawn(fixture.runtime.context(), fixture.client->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(remote));
   fixture.until([owner = fixture.owner] { return owner->native_remote_close_received; });
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(close));
   fixture.require_sender_joined();
   auto state = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_return owner->terminal_cleanup_complete && !owner->canceled && owner->udp_send_discarded &&
              owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(state));
   auto stream = asio::co_spawn(fixture.runtime.context(), fixture.server_stream->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(stream));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(replay));
}

BOOST_AUTO_TEST_CASE(quic_connection_close_native_peer_close_during_prefix_wait_is_joined_cleanup) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && owner->udp_inflight_generation != 0; });
   BOOST_CHECK(close.wait_for(0ms) != std::future_status::ready);
   auto remote = asio::co_spawn(fixture.runtime.context(), fixture.client->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(remote));
   BOOST_CHECK_NO_THROW(completed(close));
   fixture.require_sender_joined();
   auto state = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_return owner->native_remote_close_received && !owner->canceled && owner->udp_send_discarded &&
              owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(state));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(replay));
}

BOOST_AUTO_TEST_CASE(quic_fin_close_native_peer_close_does_not_count_unsent_fin_as_success) {
   auto fixture = quic_send_fixture{};
   fixture.queue();
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server_stream->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner, stream = fixture.stream_owner] {
      return stream->fin_send_generation != 0 && owner->udp_completed_generation < stream->fin_send_generation;
   });
   auto remote = asio::co_spawn(fixture.runtime.context(), fixture.client->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(remote));
   BOOST_CHECK_EXCEPTION(completed(close), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::operation_aborted; });
   auto joined = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_NO_THROW(completed(joined));
   fixture.require_sender_joined();
   auto unsent = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner, stream = fixture.stream_owner]() -> asio::awaitable<bool> {
          co_return owner->native_remote_close_received &&
              owner->udp_completed_generation < stream->fin_send_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(unsent));
}

BOOST_AUTO_TEST_CASE(quic_connection_close_active_socket_error_is_sticky_not_remote_cleanup) {
   auto fixture = quic_send_fixture{};
   fixture.expected_close_error = asio::error::invalid_argument;
   fixture.queue(6000);
   fixture.until(
       [owner = fixture.owner] { return owner->udp_inflight_generation != 0 && !owner->outbound_datagrams.empty(); });
   auto close = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   fixture.until([owner = fixture.owner] { return owner->close_started && !owner->closing; });
   auto fault = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<void> {
          // Port zero makes a real socket send fail; no connection-state or error fabrication.
          owner->outbound_datagrams.front().route.remote.port(0);
          co_return;
       },
       asio::use_future);
   completed(fault);
   fixture.held.release();
   BOOST_CHECK_EXCEPTION(completed(close), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::invalid_argument; });
   fixture.require_sender_joined();
   auto state = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<bool> {
          co_return !owner->native_remote_close_received && !owner->udp_send_discarded &&
              owner->udp_completed_generation < owner->udp_enqueued_generation;
       },
       asio::use_future);
   BOOST_TEST(completed(state));
   auto replay = asio::co_spawn(fixture.runtime.context(), fixture.server->async_close(), asio::use_future);
   BOOST_CHECK_EXCEPTION(completed(replay), boost::system::system_error,
                         [](const auto& error) { return error.code() == asio::error::invalid_argument; });
}

BOOST_AUTO_TEST_CASE(quic_udp_send_discard_marker_never_hides_later_real_error_or_exception) {
   auto fixture = udp_send_fixture{};
   const auto prefix = fixture.enqueue(0x82);
   auto marked = asio::co_spawn(
       fixture.owner->strand,
       [owner = fixture.owner]() -> asio::awaitable<void> {
          owner->clear_queued_work();
          BOOST_TEST(owner->udp_send_discarded);
          owner->fail_udp_send(asio::error::fault, std::make_exception_ptr(std::bad_alloc{}));
          owner->clear_queued_work();
          BOOST_TEST(!owner->udp_send_discarded);
          co_return;
       },
       asio::use_future);
   completed(marked);
   auto drain = fixture.drain(prefix);
   BOOST_CHECK_THROW(completed(drain), std::bad_alloc);
   auto replay = fixture.drain(prefix);
   BOOST_CHECK_THROW(completed(replay), std::bad_alloc);
}

} // namespace
