#include <boost/asio/awaitable.hpp>
#include <boost/test/unit_test.hpp>
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <vector>

import forge.api.p2p.publication;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.p2p.dht;
import forge.net.p2p.protocol;
import forge.net.p2p.provider_registration;

BOOST_AUTO_TEST_CASE(publication_stop_is_sticky_for_native_retirement_and_reentrant_facade_destruction) {
   auto runtime = forge::asio::runtime{};
   auto native = std::stop_source{};
   auto closed = std::atomic_uint{0};
   auto owner = std::make_unique<forge::api::p2p::publication>(forge::api::p2p::detail::publication_access::make(
      runtime.context().get_executor(), [&] { ++closed; },
      []() -> boost::asio::awaitable<void> { co_return; }, [] { return true; }, native.get_token()));
   const auto token = owner->stop_token();
   auto notifications = std::atomic_uint{0};
   auto callback = std::stop_callback{token, [&]() noexcept {
      ++notifications;
      owner->close();
      owner.reset();
   }};
   BOOST_CHECK(native.request_stop());
   BOOST_CHECK(!owner);
   BOOST_CHECK(token.stop_requested());
   BOOST_CHECK_EQUAL(closed.load(), 1U);
   BOOST_CHECK_EQUAL(notifications.load(), 1U);
   auto late = std::atomic_uint{0};
   auto late_callback = std::stop_callback{token, [&]() noexcept { ++late; }};
   BOOST_CHECK_EQUAL(late.load(), 1U);
}

BOOST_AUTO_TEST_CASE(publication_pre_retired_native_token_and_move_replacement_are_generation_scoped) {
   auto runtime = forge::asio::runtime{};
   const auto make = [&](std::stop_token native = {}) {
      return forge::api::p2p::detail::publication_access::make(runtime.context().get_executor(), [] {},
         []() -> boost::asio::awaitable<void> { co_return; }, [] { return true; }, native);
   };
   auto native = std::stop_source{};
   native.request_stop();
   auto retired = make(native.get_token());
   BOOST_CHECK(!retired.active());
   BOOST_CHECK(retired.stop_token().stop_requested());
   auto old = make();
   const auto old_token = old.stop_token();
   auto next = make();
   const auto next_token = next.stop_token();
   old = std::move(next);
   BOOST_CHECK(old_token.stop_requested());
   BOOST_CHECK(!next_token.stop_requested());
   BOOST_CHECK(!next.stop_token().stop_possible());
   old.close();
   BOOST_CHECK(next_token.stop_requested());
}

BOOST_AUTO_TEST_CASE(publication_concurrent_close_signals_once_and_drain_remains_explicit) {
   auto runtime = forge::asio::runtime{};
   auto closed = std::atomic_uint{0};
   auto drained = std::atomic_uint{0};
   auto notifications = std::atomic_uint{0};
   auto value = forge::api::p2p::detail::publication_access::make(runtime.context().get_executor(), [&] { ++closed; },
      [&]() -> boost::asio::awaitable<void> { ++drained; co_return; }, [] { return true; });
   auto callback = std::stop_callback{value.stop_token(), [&]() noexcept { ++notifications; }};
   auto threads = std::vector<std::thread>{};
   for (auto index = 0; index != 64; ++index) { threads.emplace_back([&] { value.close(); }); }
   for (auto& thread : threads) { thread.join(); }
   BOOST_CHECK_EQUAL(closed.load(), 1U);
   BOOST_CHECK_EQUAL(notifications.load(), 1U);
   BOOST_CHECK_EQUAL(drained.load(), 0U);
   forge::asio::blocking::run(runtime, value.async_close());
   forge::asio::blocking::run(runtime, value.async_close());
   BOOST_CHECK_EQUAL(drained.load(), 1U);
}

BOOST_AUTO_TEST_CASE(provider_request_stop_retains_retryable_withdraw_fence) {
   auto runtime = forge::asio::runtime{};
   auto stopped = std::atomic_uint{0};
   auto withdrawn = std::atomic_uint{0};
   auto registration = forge::net::p2p::detail::provider_registration_access::make(
      forge::net::p2p::protocol_id{"/test/provider-stop/1"}, forge::net::p2p::dht::key{},
      [] { return true; }, [&] { ++stopped; }, [&]() -> boost::asio::awaitable<void> {
         if (++withdrawn == 1U) { throw std::runtime_error{"withdraw failed"}; }
         co_return;
      });
   auto threads = std::vector<std::thread>{};
   for (auto index = 0; index != 32; ++index) { threads.emplace_back([&] { registration.request_stop(); }); }
   for (auto& thread : threads) { thread.join(); }
   BOOST_CHECK(!registration.active());
   BOOST_CHECK_EQUAL(stopped.load(), 1U);
   BOOST_CHECK_EQUAL(withdrawn.load(), 0U);
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, registration.async_withdraw()), std::runtime_error);
   forge::asio::blocking::run(runtime, registration.async_withdraw());
   forge::asio::blocking::run(runtime, registration.async_withdraw());
   BOOST_CHECK_EQUAL(withdrawn.load(), 2U);
}
