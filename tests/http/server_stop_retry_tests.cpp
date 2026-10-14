#include <boost/test/unit_test.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <utility>

import forge.asio.notification;
import forge.asio.runtime;
import forge.net.http.exceptions;
import forge.net.http.route_context;
import forge.net.http.server;
import forge.net.http.types;

namespace {

namespace asio = boost::asio;

constexpr auto operation_timeout = std::chrono::seconds{5};
constexpr auto result_timeout = std::chrono::seconds{6};

struct held_handler {
   forge::asio::notification entered;
   forge::asio::notification release;
   std::atomic_bool completed = false;
   std::atomic_uint calls = 0;
};

boost::asio::awaitable<forge::net::http::response> hold_request(
    std::shared_ptr<held_handler> state, forge::net::http::route_context& context) {
   // A real handler may need to finish owned work even after socket cancellation.
   // Keep it alive until the test releases it, bounded independently of stop.
   co_await asio::this_coro::throw_if_cancelled(false);
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation());
   auto completed = boost::scope::scope_exit{[state] { state->completed.store(true); }};
   const auto epoch = state->release.epoch();
   ++state->calls;
   state->entered.notify();
   static_cast<void>(co_await state->release.async_wait_until(
      epoch, std::chrono::steady_clock::now() + operation_timeout));
   co_return forge::net::http::make_text_response(context.request, forge::net::http::status::ok, "done");
}

struct server_fixture {
   server_fixture()
       : server{runtime, {.read_timeout = operation_timeout, .idle_timeout = operation_timeout},
                [state = handler](forge::net::http::route_context& context) { return hold_request(state, context); }} {
      server.start();
   }

   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 4}};
   std::shared_ptr<held_handler> handler = std::make_shared<held_handler>();
   forge::net::http::server server;
};

asio::awaitable<boost::system::error_code> request_until_peer_closes(std::uint16_t port) {
   auto executor = co_await asio::this_coro::executor;
   auto stream = boost::beast::tcp_stream{executor};
   stream.expires_after(operation_timeout);
   co_await stream.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
   constexpr char request[] = "GET /held HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n\r\n";
   co_await asio::async_write(stream, asio::buffer(request, sizeof(request) - 1), asio::use_awaitable);
   auto input = std::array<char, 1>{};
   auto [error, size] = co_await stream.async_read_some(asio::buffer(input), asio::as_tuple(asio::use_awaitable));
   static_cast<void>(size);
   co_return error;
}

asio::awaitable<std::exception_ptr> capture_error(asio::awaitable<void> operation) {
   try {
      co_await std::move(operation);
   } catch (...) {
      co_return std::current_exception();
   }
   co_return std::exception_ptr{};
}

asio::awaitable<bool> executor_barrier() {
   co_return true;
}

template <typename T> T ready(server_fixture& fixture, std::future<T>& future) {
   const auto status = future.wait_for(result_timeout);
   if (status != std::future_status::ready) {
      // Bound teardown too: a lost wake must fail, not hang server's destructor.
      fixture.runtime.stop();
   }
   BOOST_REQUIRE(status == std::future_status::ready);
   return future.get();
}

bool was_cancelled(const std::exception_ptr& failure) {
   if (!failure) {
      return false;
   }
   try {
      std::rethrow_exception(failure);
   } catch (const boost::system::system_error& error) {
      return error.code() == asio::error::operation_aborted;
   } catch (...) {
      return false;
   }
}

bool restart_was_rejected(const std::exception_ptr& failure) {
   if (!failure) {
      return false;
   }
   try {
      std::rethrow_exception(failure);
   } catch (const forge::net::http::exceptions::internal&) {
      return true;
   } catch (...) {
      return false;
   }
}

void wait_for_handler(server_fixture& fixture, std::uint64_t epoch) {
   auto entered = asio::co_spawn(fixture.runtime.context(), fixture.handler->entered.async_wait_until(
      epoch, std::chrono::steady_clock::now() + operation_timeout), asio::use_future);
   BOOST_REQUIRE(ready(fixture, entered) != epoch);
}

void require_peer_closed(server_fixture& fixture, std::future<boost::system::error_code>& client) {
   const auto error = ready(fixture, client);
   // This public socket observation proves stop reached session cancellation,
   // rather than canceling the stop coroutine before it owned any cleanup.
   BOOST_REQUIRE(error == asio::error::eof || error == asio::error::connection_reset);
}

} // namespace

BOOST_AUTO_TEST_CASE(http_stop_cancellation_preserves_live_handler_until_real_retry_drains) {
   auto fixture = server_fixture{};
   auto release = boost::scope::scope_exit{[&] { fixture.handler->release.notify(); }};
   auto serial = asio::make_strand(fixture.runtime.context());
   const auto epoch = fixture.handler->entered.epoch();
   auto client = asio::co_spawn(fixture.runtime.context(), request_until_peer_closes(fixture.server.port()),
                                asio::use_future);
   wait_for_handler(fixture, epoch);

   auto signal = std::make_shared<asio::cancellation_signal>();
   auto first = asio::co_spawn(serial, capture_error(fixture.server.async_stop()),
      asio::bind_cancellation_slot(signal->slot(), asio::use_future));
   require_peer_closed(fixture, client);
   BOOST_REQUIRE(!fixture.handler->completed.load());
   asio::post(serial, [signal] { signal->emit(asio::cancellation_type::terminal); });
   BOOST_CHECK(was_cancelled(ready(fixture, first)));

   // A failed stop is still closing, not a restartable, fully drained server.
   auto restart = asio::co_spawn(serial, capture_error(fixture.server.async_start()), asio::use_future);
   BOOST_CHECK(restart_was_rejected(ready(fixture, restart)));
   auto retry = asio::co_spawn(serial, capture_error(fixture.server.async_stop()), asio::use_future);
   auto joined = asio::co_spawn(serial, capture_error(fixture.server.async_stop()), asio::use_future);
   BOOST_CHECK(retry.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   BOOST_CHECK(joined.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   BOOST_CHECK(!fixture.handler->completed.load());
   fixture.handler->release.notify();
   BOOST_CHECK(!ready(fixture, retry));
   BOOST_CHECK(!ready(fixture, joined));
   BOOST_CHECK(fixture.handler->completed.load());
   BOOST_CHECK_EQUAL(fixture.handler->calls.load(), 1U);

   auto repeated = asio::co_spawn(serial, capture_error(fixture.server.async_stop()), asio::use_future);
   BOOST_CHECK(!ready(fixture, repeated));
   auto started = asio::co_spawn(serial, capture_error(fixture.server.async_start()), asio::use_future);
   BOOST_REQUIRE(!ready(fixture, started));
   auto stopped = asio::co_spawn(serial, capture_error(fixture.server.async_stop()), asio::use_future);
   BOOST_CHECK(!ready(fixture, stopped));
}

BOOST_AUTO_TEST_CASE(http_stop_joined_callers_keep_failed_result_after_retry_submission) {
   auto fixture = server_fixture{};
   auto release = boost::scope::scope_exit{[&] { fixture.handler->release.notify(); }};
   auto serial = asio::make_strand(fixture.runtime.context());
   const auto epoch = fixture.handler->entered.epoch();
   auto client = asio::co_spawn(fixture.runtime.context(), request_until_peer_closes(fixture.server.port()),
                                asio::use_future);
   wait_for_handler(fixture, epoch);

   auto signal = std::make_shared<asio::cancellation_signal>();
   auto first = asio::co_spawn(serial, capture_error(fixture.server.async_stop()),
      asio::bind_cancellation_slot(signal->slot(), asio::use_future));
   require_peer_closed(fixture, client);
   auto joined = asio::co_spawn(serial, capture_error(fixture.server.async_stop()), asio::use_future);
   // Queue the join request before cancellation. This caller-side barrier is
   // not an admission witness; ordering relies on Boost 1.90 queued delivery.
   auto barrier = asio::co_spawn(serial, executor_barrier(), asio::use_future);
   BOOST_REQUIRE(ready(fixture, barrier));
   asio::post(serial, [signal] { signal->emit(asio::cancellation_type::terminal); });
   const auto failure = ready(fixture, first);
   BOOST_CHECK(was_cancelled(failure));
   // Submit a replacement before consuming the old result. The public API
   // does not expose the exact instant the replacement attempt is reserved.
   auto retry = asio::co_spawn(serial, capture_error(fixture.server.async_stop()), asio::use_future);
   const auto joined_failure = ready(fixture, joined);
   BOOST_CHECK(was_cancelled(joined_failure));
   BOOST_CHECK(joined_failure == failure);
   BOOST_CHECK(!fixture.handler->completed.load());

   BOOST_CHECK(retry.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   fixture.handler->release.notify();
   BOOST_CHECK(!ready(fixture, retry));
   BOOST_CHECK(fixture.handler->completed.load());
}

BOOST_AUTO_TEST_CASE(http_stop_cancelled_joiner_does_not_abandon_owned_drain) {
   auto fixture = server_fixture{};
   auto release = boost::scope::scope_exit{[&] { fixture.handler->release.notify(); }};
   auto serial = asio::make_strand(fixture.runtime.context());
   const auto epoch = fixture.handler->entered.epoch();
   auto client = asio::co_spawn(fixture.runtime.context(), request_until_peer_closes(fixture.server.port()),
                                asio::use_future);
   wait_for_handler(fixture, epoch);

   auto owner = asio::co_spawn(serial, capture_error(fixture.server.async_stop()), asio::use_future);
   require_peer_closed(fixture, client);
   auto signal = std::make_shared<asio::cancellation_signal>();
   auto joined = asio::co_spawn(serial, capture_error(fixture.server.async_stop()),
      asio::bind_cancellation_slot(signal->slot(), asio::use_future));
   // Queue the join request before cancellation. This caller-side barrier is
   // not an admission witness; ordering relies on Boost 1.90 queued delivery.
   auto barrier = asio::co_spawn(serial, executor_barrier(), asio::use_future);
   BOOST_REQUIRE(ready(fixture, barrier));
   asio::post(serial, [signal] { signal->emit(asio::cancellation_type::terminal); });
   BOOST_CHECK(was_cancelled(ready(fixture, joined)));
   BOOST_CHECK(owner.wait_for(std::chrono::milliseconds{25}) == std::future_status::timeout);
   BOOST_CHECK(!fixture.handler->completed.load());
   fixture.handler->release.notify();
   BOOST_CHECK(!ready(fixture, owner));
   BOOST_CHECK(fixture.handler->completed.load());
}

BOOST_AUTO_TEST_CASE(http_stop_session_completion_and_drain_registration_share_a_strand) {
   auto fixture = server_fixture{};
   auto release = boost::scope::scope_exit{[&] { fixture.handler->release.notify(); }};
   for (auto attempt = 0U; attempt < 32U; ++attempt) {
      if (attempt != 0U) {
         fixture.server.start();
      }
      fixture.handler->completed.store(false);
      const auto epoch = fixture.handler->entered.epoch();
      auto client = asio::co_spawn(fixture.runtime.context(), request_until_peer_closes(fixture.server.port()),
                                   asio::use_future);
      wait_for_handler(fixture, epoch);
      auto stopping = asio::co_spawn(fixture.runtime.context(), capture_error(fixture.server.async_stop()),
                                     asio::use_future);
      require_peer_closed(fixture, client);
      // Completion now races the acceptor's session-drain continuation across
      // distinct strands. No production hook forces a particular instruction gap.
      fixture.handler->release.notify();
      BOOST_CHECK(!ready(fixture, stopping));
      BOOST_CHECK(fixture.handler->completed.load());
   }
   BOOST_CHECK_EQUAL(fixture.handler->calls.load(), 32U);
}
