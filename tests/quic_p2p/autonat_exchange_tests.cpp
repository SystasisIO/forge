module;

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <future>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>
#include <forge/exceptions/macros.hpp>

#include "libp2p_identity_fixture.hxx"

module forge.net.p2p.node;

import forge.asio.blocking;
import forge.asio.notification;
import forge.asio.runtime;
import forge.exceptions;
import forge.multiformats.multihash;
import forge.multiformats.types;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.reachability;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.quic.connector;
import forge.net.quic.exceptions;
import forge.net.quic.listener;
import forge.net.quic.options;
import forge.net.quic.security;
import forge.net.quic.transport;
import forge.net.transport.exceptions;
import forge.net.transport.stream;
import forge.net.yamux.exceptions;

#include "../../libraries/net/p2p/details/autonat_exchange.hxx"
#include "../../libraries/net/p2p/details/autonat_v2_dialback.hxx"
#include "../../libraries/net/p2p/details/cancellation_latch.hxx"
#include "../../libraries/net/p2p/details/length_delimited.hxx"
#include "../../libraries/net/p2p/details/resource_stream.hxx"

namespace forge::net::p2p {
namespace {

namespace quic = forge::net::quic;

constexpr auto exchange_timeout = std::chrono::milliseconds{500};
constexpr auto exchange_nonce = std::uint64_t{0x01'0203'0405'0607'08};

template <typename T>
T await_terminal(std::future<T>& future) {
   BOOST_REQUIRE(future.wait_for(std::chrono::seconds{5}) == std::future_status::ready);
   return future.get();
}

[[nodiscard]] peer_id local_peer() {
   const auto payload = forge::multiformats::bytes{0xa1U};
   return peer_id::from_bytes(forge::multiformats::multihash::identity(payload).encode());
}

[[nodiscard]] std::vector<endpoint> candidates() {
   return {
       parse_endpoint("/ip4/8.8.8.8/tcp/4001"),
       parse_endpoint("/ip4/8.8.4.4/tcp/4002"),
   };
}

[[nodiscard]] quic::server_options loopback_server_options() {
   static const auto identity = forge::tests::p2p::make_identity_fixture("autonat-exchange-tests");
   return quic::server_options{
       .security = quic::security_options{.verify_peer = false},
       .certificate_pem = identity.certificate_pem,
       .private_key_pem = identity.private_key_pem,
   };
}

[[nodiscard]] quic::client_options loopback_client_options() {
   return quic::client_options{
       .handshake_timeout = std::chrono::milliseconds{5'000},
       .security = quic::security_options{.verify_peer = false},
   };
}

// This uses the real QUIC test transport rather than a scripted stream backend.
struct exchange_stream_pair {
   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 2}};
   quic::listener listener{runtime, quic::endpoint{.host = "127.0.0.1", .port = 0}, loopback_server_options()};
   quic::connector connector{runtime};
   quic::connection client_connection;
   quic::connection server_connection;
   stream client;
   stream server;

   exchange_stream_pair() {
      auto accepted = boost::asio::co_spawn(runtime.context(), listener.async_accept(), boost::asio::use_future);
      client_connection = forge::asio::blocking::run(
          runtime, connector.async_connect(listener.local_endpoint(), loopback_client_options()));
      server_connection = await_terminal(accepted);

      auto accepted_stream =
          boost::asio::co_spawn(runtime.context(), server_connection.async_accept_stream(), boost::asio::use_future);
      client = stream{quic::as_transport_stream(
          forge::asio::blocking::run(runtime, client_connection.async_open_stream()))};
      // QUIC announces a stream on its first write, not when the local handle is created.
      const auto preface = std::vector<std::uint8_t>{0xa5};
      forge::asio::blocking::run(runtime, client.async_write(preface));
      server = stream{quic::as_transport_stream(await_terminal(accepted_stream))};
      const auto received = forge::asio::blocking::run(runtime, server.async_read());
      BOOST_REQUIRE(received == preface);
   }

   ~exchange_stream_pair() {
      client.request_cancel();
      server.request_cancel();
      client_connection.request_cancel();
      server_connection.request_cancel();
      listener.stop();
   }
};

[[nodiscard]] reachability::options v1_options() {
   return reachability::options{.max_endpoints = 16, .max_message_size = 4096};
}

[[nodiscard]] reachability::options v2_options() {
   return reachability::options{.max_endpoints = 16, .max_message_size = 8192};
}

boost::asio::awaitable<reachability::message> read_v1_request(stream& channel) {
   auto buffer = std::vector<std::uint8_t>{};
   co_return reachability::codec::decode_v1(
       co_await async_read_length_delimited(channel, buffer, v1_options().max_message_size), v1_options());
}

boost::asio::awaitable<reachability::v2::message> read_v2_request(stream& channel) {
   auto buffer = std::vector<std::uint8_t>{};
   co_return reachability::codec::decode_v2(
       co_await async_read_length_delimited(channel, buffer, v2_options().max_message_size), v2_options());
}

boost::asio::awaitable<void> write_v1(stream& channel, const reachability::message& message) {
   const auto frame = reachability::codec::encode_v1(message, v1_options());
   co_await channel.async_write(std::span<const std::uint8_t>{frame});
}

boost::asio::awaitable<void> write_v2(stream& channel, const reachability::v2::message& message) {
   const auto frame = reachability::codec::encode_v2(message, v2_options());
   co_await channel.async_write(std::span<const std::uint8_t>{frame});
}

[[nodiscard]] reachability::result run_v1(exchange_stream_pair& pair, std::vector<endpoint> offered = candidates()) {
   return forge::asio::blocking::run(
       pair.runtime,
       detail::async_exchange_autonat(std::move(pair.client), local_peer(), std::move(offered), false, 0, {},
                                      pair.runtime.context(), exchange_timeout, {}));
}

[[nodiscard]] reachability::result
run_v2(exchange_stream_pair& pair, detail::autonat_dialback_wait dialback,
       std::vector<endpoint> offered = candidates(), std::uint64_t nonce = exchange_nonce,
       std::shared_ptr<cancellation_latch> cancellation = {}) {
   return forge::asio::blocking::run(
       pair.runtime,
       detail::async_exchange_autonat(std::move(pair.client), local_peer(), std::move(offered), true, nonce,
                                      std::move(dialback), pair.runtime.context(), exchange_timeout,
                                      std::move(cancellation)));
}

template <typename Operation> void expect_p2p_failure(Operation&& operation, exceptions::code expected) {
   try {
      static_cast<void>(std::forward<Operation>(operation)());
      BOOST_FAIL("expected AutoNAT exchange failure");
   } catch (const forge::exceptions::base& error) {
      const auto code = exceptions::code_of(error);
      BOOST_REQUIRE(code.has_value());
      BOOST_CHECK(*code == expected);
   }
}

[[nodiscard]] detail::autonat_dialback_wait matching_dialback(endpoint expected) {
   return [expected = std::move(expected)](std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<std::optional<endpoint>> { co_return expected; };
}

BOOST_AUTO_TEST_SUITE(p2p_autonat_exchange)

BOOST_AUTO_TEST_CASE(v1_accepts_chunked_length_delimited_response) {
   auto pair = exchange_stream_pair{};
   const auto offered = candidates();
   auto server = boost::asio::co_spawn(
       pair.runtime.context(),
       [&channel = pair.server, expected = offered.front()]() mutable -> boost::asio::awaitable<void> {
          const auto request = co_await read_v1_request(channel);
          if (request.kind != reachability::message::message_kind::dial || !request.peer) {
             throw std::runtime_error{"expected AutoNAT v1 dial request"};
          }
          const auto frame = reachability::codec::encode_v1(reachability::message{
              .kind = reachability::message::message_kind::dial_response,
              .response = reachability::dial_response{.status = reachability::dial_status::ok, .endpoint = expected},
          }, v1_options());
          if (frame.size() < 2U) {
             throw std::runtime_error{"AutoNAT v1 response did not produce a frame"};
          }
          co_await channel.async_write(std::span<const std::uint8_t>{frame}.first(1));
          co_await channel.async_write(std::span<const std::uint8_t>{frame}.subspan(1));
       },
       boost::asio::use_future);

   const auto result = run_v1(pair, offered);
   await_terminal(server);
   BOOST_CHECK(result.value == reachability::state::publicly_reachable);
   BOOST_REQUIRE(result.observed.has_value());
   BOOST_TEST(result.observed->to_string() == offered.front().to_string());
}

BOOST_AUTO_TEST_CASE(v1_accepts_donor_substitution_without_advertisement_authority) {
   auto pair = exchange_stream_pair{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v1_request(channel));
          co_await write_v1(channel, reachability::message{
              .kind = reachability::message::message_kind::dial_response,
              .response = reachability::dial_response{
                  .status = reachability::dial_status::ok,
                  .endpoint = parse_endpoint("/ip4/1.1.1.1/tcp/443"),
              },
          });
       }, boost::asio::use_future);

   const auto result = run_v1(pair);
   BOOST_CHECK(result.value == reachability::state::publicly_reachable);
   BOOST_REQUIRE(result.observed);
   BOOST_TEST(result.observed->to_string() == "/ip4/1.1.1.1/tcp/443");
   await_terminal(server);
}

BOOST_AUTO_TEST_CASE(v1_refused_dial_does_not_claim_private_reachability) {
   auto pair = exchange_stream_pair{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v1_request(channel));
          co_await write_v1(channel, reachability::message{
              .kind = reachability::message::message_kind::dial_response,
              .response = reachability::dial_response{.status = reachability::dial_status::dial_refused},
          });
       }, boost::asio::use_future);

   const auto result = run_v1(pair);
   await_terminal(server);
   BOOST_CHECK(result.value == reachability::state::unknown);
   BOOST_TEST(!result.observed.has_value());
}

BOOST_AUTO_TEST_CASE(v2_ok_without_a_validated_dialback_is_rejected) {
   auto pair = exchange_stream_pair{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_response,
              .dial_response = reachability::v2::dial_response{
                  .status = reachability::v2::response_status::ok,
                  .index = 0,
                  .dial_status = reachability::v2::dial_status::ok,
              },
          });
       }, boost::asio::use_future);

   const auto no_dialback = [](std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<std::optional<endpoint>> { co_return std::nullopt; };
   expect_p2p_failure([&] { return run_v2(pair, no_dialback); }, exceptions::code::protocol_error);
   await_terminal(server);
}

BOOST_AUTO_TEST_CASE(v2_rejects_response_with_out_of_range_address_index) {
   auto pair = exchange_stream_pair{};
   auto dialback_calls = std::size_t{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_response,
              .dial_response = reachability::v2::dial_response{
                  .status = reachability::v2::response_status::ok,
                  .index = 2,
                  .dial_status = reachability::v2::dial_status::ok,
              },
          });
       }, boost::asio::use_future);
   const auto dialback = [&dialback_calls](std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<std::optional<endpoint>> {
      ++dialback_calls;
      co_return std::nullopt;
   };

   expect_p2p_failure([&] { return run_v2(pair, dialback); }, exceptions::code::protocol_error);
   await_terminal(server);
   BOOST_TEST(dialback_calls == 0U);
}

BOOST_AUTO_TEST_CASE(v2_rejects_data_request_larger_than_protocol_limit) {
   auto pair = exchange_stream_pair{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_data_request,
              .dial_data_request = reachability::v2::dial_data_request{.index = 0, .bytes = 100'001},
          });
       }, boost::asio::use_future);

   expect_p2p_failure([&] { return run_v2(pair, matching_dialback(candidates().front())); },
                       exceptions::code::protocol_error);
   await_terminal(server);
}

BOOST_AUTO_TEST_CASE(v2_rejects_repeated_data_request) {
   auto pair = exchange_stream_pair{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          const auto request = reachability::v2::message{
              .type = reachability::v2::message::kind::dial_data_request,
              .dial_data_request = reachability::v2::dial_data_request{.index = 0, .bytes = 1},
          };
          co_await write_v2(channel, request);
          co_await write_v2(channel, request);
       }, boost::asio::use_future);

   expect_p2p_failure([&] { return run_v2(pair, matching_dialback(candidates().front())); },
                       exceptions::code::protocol_error);
   await_terminal(server);
}

BOOST_AUTO_TEST_CASE(v2_rejects_changed_address_after_data_request) {
   auto pair = exchange_stream_pair{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_data_request,
              .dial_data_request = reachability::v2::dial_data_request{.index = 0, .bytes = 1},
          });
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_response,
              .dial_response = reachability::v2::dial_response{
                  .status = reachability::v2::response_status::ok,
                  .index = 1,
                  .dial_status = reachability::v2::dial_status::ok,
              },
          });
       }, boost::asio::use_future);

   expect_p2p_failure([&] { return run_v2(pair, matching_dialback(candidates().front())); },
                       exceptions::code::protocol_error);
   await_terminal(server);
}

BOOST_AUTO_TEST_CASE(v2_matching_validated_dialback_makes_selected_address_public) {
   auto pair = exchange_stream_pair{};
   const auto offered = candidates();
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_response,
              .dial_response = reachability::v2::dial_response{
                  .status = reachability::v2::response_status::ok,
                  .index = 0,
                  .dial_status = reachability::v2::dial_status::ok,
              },
          });
       }, boost::asio::use_future);

   const auto result = run_v2(pair, matching_dialback(offered.front()), offered);
   await_terminal(server);
   BOOST_CHECK(result.value == reachability::state::publicly_reachable);
   BOOST_REQUIRE(result.observed.has_value());
   BOOST_TEST(result.observed->to_string() == offered.front().to_string());
}

BOOST_AUTO_TEST_CASE(v2_stop_after_validated_dialback_cannot_return_success) {
   auto pair = exchange_stream_pair{};
   auto cancellation = std::make_shared<cancellation_latch>();
   auto server = boost::asio::co_spawn(pair.runtime.context(),
       [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_response,
              .dial_response = reachability::v2::dial_response{
                  .status = reachability::v2::response_status::ok, .index = 0,
                  .dial_status = reachability::v2::dial_status::ok}});
       }, boost::asio::use_future);
   const auto dialback = [cancellation](std::shared_ptr<cancellation_latch>)
       -> boost::asio::awaitable<std::optional<endpoint>> {
      cancellation->request_stop();
      co_return candidates().front();
   };
   expect_p2p_failure([&] { return run_v2(pair, dialback, candidates(), exchange_nonce, cancellation); },
                      exceptions::code::canceled);
   await_terminal(server);
}

BOOST_AUTO_TEST_CASE(v2_dialback_error_with_matching_nonce_and_validated_callback_is_public) {
   auto pair = exchange_stream_pair{};
   const auto offered = candidates();
   auto server = boost::asio::co_spawn(
       pair.runtime.context(),
       [&channel = pair.server]() -> boost::asio::awaitable<void> {
          const auto request = co_await read_v2_request(channel);
          if (!request.dial_request || request.dial_request->nonce != exchange_nonce) {
             throw std::runtime_error{"AutoNAT v2 request nonce did not bind dialback"};
          }
          co_await write_v2(channel, reachability::v2::message{
              .type = reachability::v2::message::kind::dial_response,
              .dial_response = reachability::v2::dial_response{
                  .status = reachability::v2::response_status::ok,
                  .index = 0,
                  .dial_status = reachability::v2::dial_status::dial_back_error,
              },
          });
       },
       boost::asio::use_future);

   const auto result = run_v2(pair, matching_dialback(offered.front()), offered, exchange_nonce);
   await_terminal(server);
   BOOST_CHECK(result.value == reachability::state::publicly_reachable);
   BOOST_REQUIRE(result.observed.has_value());
   BOOST_TEST(result.observed->to_string() == offered.front().to_string());
}

BOOST_AUTO_TEST_CASE(v2_caller_cancellation_interrupts_pending_exchange) {
   auto pair = exchange_stream_pair{};
   auto cancellation = std::make_shared<cancellation_latch>();
   auto request_received = std::promise<void>{};
   auto received = request_received.get_future();
   auto server = boost::asio::co_spawn(
       pair.runtime.context(),
       [&channel = pair.server, &request_received]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
          request_received.set_value();
       },
       boost::asio::use_future);
   auto exchange = boost::asio::co_spawn(
       pair.runtime.context(),
       detail::async_exchange_autonat(std::move(pair.client), local_peer(), candidates(), true, exchange_nonce,
                                      matching_dialback(candidates().front()), pair.runtime.context(), exchange_timeout,
                                      cancellation),
       boost::asio::use_future);

   BOOST_REQUIRE(received.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   cancellation->request_stop();
   expect_p2p_failure([&] { return exchange.get(); }, exceptions::code::canceled);
   await_terminal(server);
}

BOOST_AUTO_TEST_CASE(v2_deadline_interrupts_pending_exchange) {
   auto pair = exchange_stream_pair{};
   auto server = boost::asio::co_spawn(
       pair.runtime.context(), [&channel = pair.server]() -> boost::asio::awaitable<void> {
          static_cast<void>(co_await read_v2_request(channel));
       }, boost::asio::use_future);

   expect_p2p_failure(
       [&] {
          return forge::asio::blocking::run(
              pair.runtime,
              detail::async_exchange_autonat(std::move(pair.client), local_peer(), candidates(), true, exchange_nonce,
                                             matching_dialback(candidates().front()), pair.runtime.context(),
                                             std::chrono::milliseconds{20}, {}));
       },
       exceptions::code::timeout);
   await_terminal(server);
}

// Only the negative/lifetime cases use a bounded transport model. The foreign
// reset regression below reads the nonce over the actual QUIC transport.
class dialback_test_transport final : public forge::net::transport::detail::stream_concept {
 public:
   enum class fault { none, reset, yamux_reset, transport_closed, resource, internal, local_closed,
                      local_cancel, transport_cancel };
   fault write_fault = fault::none;
   fault read_fault = fault::none;
   fault close_fault = fault::none;
   bool stall_read = false;
   bool delay_close = false;
   std::atomic_bool canceled = false;
   std::atomic_bool written = false;
   std::atomic_bool closed = false;
   std::atomic_bool released = false;
   std::promise<void> reading;
   std::promise<void> closing;
   forge::asio::notification changed;

   bool valid() const noexcept override { return true; }
   std::int64_t id() const noexcept override { return 17; }
   static void fail(fault value) {
      switch (value) {
      case fault::none: return;
      case fault::reset: FORGE_THROW_EXCEPTION(quic::exceptions::stream_reset, "peer reset");
      case fault::yamux_reset:
         FORGE_THROW_EXCEPTION(forge::net::yamux::exceptions::stream_reset, "peer reset");
      case fault::transport_closed:
         FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::closed, "canonical stream EOF");
      case fault::resource: FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "local resource refusal");
      case fault::internal: FORGE_THROW_EXCEPTION(exceptions::internal, "local runtime failure");
      case fault::local_closed: FORGE_THROW_EXCEPTION(exceptions::closed, "invalid local stream");
      case fault::local_cancel: FORGE_THROW_EXCEPTION(quic::exceptions::canceled, "unrelated local cancellation");
      case fault::transport_cancel:
         FORGE_THROW_EXCEPTION(forge::net::transport::exceptions::canceled, "unrelated canonical cancellation");
      }
   }
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override {
      fail(write_fault);
      const auto message = reachability::codec::decode_v2_dial_back(bytes);
      if (message.nonce != exchange_nonce) { throw std::runtime_error{"wrong nonce written"}; }
      written.store(true);
      co_return;
   }
   boost::asio::awaitable<void> wait_for(const std::atomic_bool& ready) {
      const auto end = std::chrono::steady_clock::now() + std::chrono::seconds{7};
      while (!ready.load()) {
         const auto epoch = changed.epoch();
         if (!ready.load()) { static_cast<void>(co_await changed.async_wait_until(epoch, end)); }
         if (!ready.load() && std::chrono::steady_clock::now() >= end) {
            throw std::runtime_error{"bounded dial-back test transport wait expired"};
         }
      }
   }
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      reading.set_value();
      fail(read_fault);
      if (stall_read) {
         co_await wait_for(canceled);
         FORGE_THROW_EXCEPTION(quic::exceptions::canceled, "stream cancellation delivered");
      }
      co_return std::vector<std::uint8_t>{0};
   }
   boost::asio::awaitable<void> async_close() override {
      closing.set_value();
      if (delay_close) { co_await wait_for(released); }
      closed.store(true);
      fail(close_fault);
   }
   void cancel() override { canceled.store(true); changed.notify(); }
   void release_close() { released.store(true); changed.notify(); }
};

stream test_dialback_channel(const std::shared_ptr<dialback_test_transport>& model) {
   return stream{forge::net::transport::detail::stream_access::make_cancelable(
       model, [model] noexcept { model->cancel(); })};
}

template <typename T> T await_dialback(std::future<T>& future) {
   BOOST_REQUIRE(future.wait_for(std::chrono::seconds{8}) == std::future_status::ready);
   return future.get();
}

auto start_dialback(forge::asio::runtime& runtime, stream channel,
                    std::shared_ptr<cancellation_latch> cancellation = {},
                    std::chrono::milliseconds parent_timeout = std::chrono::seconds{10}) {
   return boost::asio::co_spawn(runtime.context(),
       detail::async_autonat_v2_dialback(std::move(channel), exchange_nonce, runtime.context(),
           std::chrono::steady_clock::now() + parent_timeout, std::move(cancellation)),
       boost::asio::use_future);
}

boost::asio::awaitable<void> read_nonce_and_reset(stream& channel) {
   auto buffer = std::vector<std::uint8_t>{};
   const auto message = reachability::codec::decode_v2_dial_back(
       co_await async_read_length_delimited(channel, buffer, 1024));
   if (message.nonce != exchange_nonce) { throw std::runtime_error{"QUIC peer received the wrong nonce"}; }
   channel.request_cancel();
   try { co_await channel.async_close(); }
   catch (const forge::exceptions::base& error) {
      if (!detail::autonat_v2_remote_close(error) &&
          !quic::exceptions::is(error, quic::exceptions::code::canceled)) { throw; }
   }
}

boost::asio::awaitable<void> join_dialback_pair(exchange_stream_pair& pair) {
   pair.server.request_cancel();
   pair.client_connection.request_cancel();
   pair.server_connection.request_cancel();
   auto failure = std::exception_ptr{};
   const auto capture_failure = [&failure] {
      try { throw; }
      catch (const forge::exceptions::base& error) {
         if (!detail::autonat_v2_remote_close(error) &&
             !quic::exceptions::is(error, quic::exceptions::code::canceled) && !failure) {
            failure = std::current_exception();
         }
      }
      catch (...) { if (!failure) { failure = std::current_exception(); } }
   };
   try { co_await pair.server.async_close(); } catch (...) { capture_failure(); }
   try { co_await pair.client_connection.async_close(); } catch (...) { capture_failure(); }
   try { co_await pair.server_connection.async_close(); } catch (...) { capture_failure(); }
   co_await pair.listener.async_stop();
   if (failure) { std::rethrow_exception(failure); }
}

BOOST_AUTO_TEST_CASE(v2_dialback_quic_reset_after_nonce_write_is_ok) {
   auto pair = exchange_stream_pair{};
   auto peer = boost::asio::co_spawn(pair.runtime.context(), read_nonce_and_reset(pair.server), boost::asio::use_future);
   auto result = start_dialback(pair.runtime, std::move(pair.client));
   auto failure = std::exception_ptr{};
   auto status = reachability::v2::dial_status::unused;
   try { status = await_dialback(result); } catch (...) { failure = std::current_exception(); }
   try { await_dialback(peer); } catch (...) { if (!failure) { failure = std::current_exception(); } }
   forge::asio::blocking::run(pair.runtime, join_dialback_pair(pair));
   if (failure) { std::rethrow_exception(failure); }
   BOOST_CHECK(status == reachability::v2::dial_status::ok);
}

BOOST_AUTO_TEST_CASE(v2_dialback_failed_nonce_write_is_error) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->write_fault = dialback_test_transport::fault::reset;
   auto result = start_dialback(runtime, test_dialback_channel(model));
   BOOST_CHECK(await_dialback(result) == reachability::v2::dial_status::dial_back_error);
   BOOST_CHECK(!model->written.load());
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_yamux_reset_after_nonce_write_is_ok) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->read_fault = dialback_test_transport::fault::yamux_reset;
   auto result = start_dialback(runtime, test_dialback_channel(model));
   BOOST_CHECK(await_dialback(result) == reachability::v2::dial_status::ok);
   BOOST_CHECK(model->written.load());
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_transport_closed_is_phase_local) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (const auto phase : {0, 1, 2}) {
      auto model = std::make_shared<dialback_test_transport>();
      if (phase == 0) { model->write_fault = dialback_test_transport::fault::transport_closed; }
      if (phase == 1) { model->read_fault = dialback_test_transport::fault::transport_closed; }
      model->close_fault = dialback_test_transport::fault::transport_closed;
      auto result = start_dialback(runtime, test_dialback_channel(model));
      BOOST_CHECK(await_dialback(result) == (phase == 0 ? reachability::v2::dial_status::dial_back_error
                                                      : reachability::v2::dial_status::ok));
      BOOST_CHECK_EQUAL(model->written.load(), phase != 0);
      BOOST_CHECK(model->closed.load());
   }
}

BOOST_AUTO_TEST_CASE(v2_dialback_parent_cancel_after_transport_close_is_canceled) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->read_fault = model->close_fault = dialback_test_transport::fault::transport_closed;
   model->delay_close = true;
   auto closing = model->closing.get_future();
   auto cancellation = std::make_shared<cancellation_latch>();
   auto result = start_dialback(runtime, test_dialback_channel(model), cancellation);
   await_dialback(closing);
   cancellation->request_stop();
   model->release_close();
   expect_p2p_failure([&] { return await_dialback(result); }, exceptions::code::canceled);
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_parent_cancel_during_ack_joins_close) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->stall_read = model->delay_close = true;
   auto reading = model->reading.get_future();
   auto closing = model->closing.get_future();
   auto cancellation = std::make_shared<cancellation_latch>();
   auto result = start_dialback(runtime, test_dialback_channel(model), cancellation);
   await_dialback(reading);
   cancellation->request_stop();
   await_dialback(closing);
   const auto pending = result.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout;
   model->release_close();
   expect_p2p_failure([&] { return await_dialback(result); }, exceptions::code::canceled);
   BOOST_CHECK(pending);
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_parent_cancel_before_write_is_canceled) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   auto cancellation = std::make_shared<cancellation_latch>();
   cancellation->request_stop();
   auto result = start_dialback(runtime, test_dialback_channel(model), cancellation);
   expect_p2p_failure([&] { return await_dialback(result); }, exceptions::code::canceled);
   BOOST_CHECK(!model->written.load());
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_parent_cancel_after_ack_before_close_cannot_win) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->delay_close = true;
   auto closing = model->closing.get_future();
   auto cancellation = std::make_shared<cancellation_latch>();
   auto result = start_dialback(runtime, test_dialback_channel(model), cancellation);
   await_dialback(closing);
   cancellation->request_stop();
   model->release_close();
   expect_p2p_failure([&] { return await_dialback(result); }, exceptions::code::canceled);
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_inherited_cancel_during_ack_joins_close) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->stall_read = model->delay_close = true;
   auto reading = model->reading.get_future();
   auto closing = model->closing.get_future();
   auto executor = boost::asio::make_strand(runtime.context());
   auto signal = std::make_shared<boost::asio::cancellation_signal>();
   auto result = boost::asio::co_spawn(executor,
       detail::async_autonat_v2_dialback(test_dialback_channel(model), exchange_nonce, runtime.context(),
           std::chrono::steady_clock::now() + std::chrono::seconds{10}, {}),
       boost::asio::bind_cancellation_slot(signal->slot(), boost::asio::use_future));
   await_dialback(reading);
   boost::asio::post(executor, [signal] { signal->emit(boost::asio::cancellation_type::all); });
   await_dialback(closing);
   const auto pending = result.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout;
   model->release_close();
   expect_p2p_failure([&] { return await_dialback(result); }, exceptions::code::canceled);
   BOOST_CHECK(pending);
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_parent_deadline_is_not_optional_ack_timeout) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->stall_read = true;
   auto result = start_dialback(runtime, test_dialback_channel(model), {}, std::chrono::milliseconds{100});
   expect_p2p_failure([&] { return await_dialback(result); }, exceptions::code::timeout);
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_optional_ack_wait_is_bounded) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto model = std::make_shared<dialback_test_transport>();
   model->stall_read = true;
   const auto start = std::chrono::steady_clock::now();
   auto result = start_dialback(runtime, test_dialback_channel(model));
   BOOST_CHECK(await_dialback(result) == reachability::v2::dial_status::ok);
   const auto elapsed = std::chrono::steady_clock::now() - start;
   BOOST_CHECK(elapsed >= std::chrono::seconds{4});
   BOOST_CHECK(elapsed < std::chrono::seconds{8});
   BOOST_CHECK(model->canceled.load());
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_success_waits_for_resource_cleanup) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto resources = resource_manager{};
   auto model = std::make_shared<dialback_test_transport>();
   model->delay_close = true;
   auto closing = model->closing.get_future();
   auto reservation = resources.reserve_stream(local_peer(), resource_manager::session_direction::outbound);
   BOOST_REQUIRE(reservation);
   auto [channel, resource] = detail::prepare_resource_stream(std::move(*reservation));
   resource->attach(std::move(test_dialback_channel(model)).into_transport_stream());
   auto result = start_dialback(runtime, stream{std::move(channel)});
   await_dialback(closing);
   const auto pending = result.wait_for(std::chrono::milliseconds{20}) == std::future_status::timeout;
   const auto retained = resources.current().system.outbound_streams;
   model->release_close();
   BOOST_CHECK(await_dialback(result) == reachability::v2::dial_status::ok);
   BOOST_CHECK(pending);
   BOOST_CHECK_EQUAL(retained, 1U);
   BOOST_CHECK_EQUAL(resources.current().system.outbound_streams, 0U);
   BOOST_CHECK(model->closed.load());
}

BOOST_AUTO_TEST_CASE(v2_dialback_local_failures_are_not_success) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (const auto phase : {0, 1, 2, 3}) {
      auto model = std::make_shared<dialback_test_transport>();
      if (phase == 0) { model->write_fault = dialback_test_transport::fault::resource; }
      if (phase == 1) { model->read_fault = dialback_test_transport::fault::internal; }
      if (phase == 2) {
         model->read_fault = dialback_test_transport::fault::reset;
         model->close_fault = dialback_test_transport::fault::internal;
      }
      if (phase == 3) { model->read_fault = dialback_test_transport::fault::local_closed; }
      auto result = start_dialback(runtime, test_dialback_channel(model));
      expect_p2p_failure([&] { return await_dialback(result); },
          phase == 0 ? exceptions::code::backpressure_rejected :
          phase == 3 ? exceptions::code::closed : exceptions::code::internal);
      BOOST_CHECK(model->closed.load());
   }
}

BOOST_AUTO_TEST_CASE(v2_dialback_unrelated_native_cancel_is_not_optional_ack) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   for (const auto fault : {dialback_test_transport::fault::local_cancel,
                            dialback_test_transport::fault::transport_cancel}) {
      auto model = std::make_shared<dialback_test_transport>();
      model->read_fault = fault;
      auto result = start_dialback(runtime, test_dialback_channel(model));
      BOOST_CHECK_EXCEPTION(await_dialback(result), forge::exceptions::base,
          [fault](const forge::exceptions::base& error) {
             return fault == dialback_test_transport::fault::local_cancel
                 ? quic::exceptions::is(error, quic::exceptions::code::canceled)
                 : forge::net::transport::exceptions::is(error, forge::net::transport::exceptions::code::canceled);
          });
      BOOST_CHECK(model->closed.load());
   }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace
} // namespace forge::net::p2p
