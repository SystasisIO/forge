module;

#include <boost/test/unit_test.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <vector>

module forge.net.p2p.node;

import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;
import forge.net.p2p.exceptions;
import forge.net.p2p.reachability;

#include "../../libraries/net/p2p/details/host_event_source.hxx"

namespace forge::net::p2p {
namespace {

template <typename T>
T event_result(boost::asio::io_context& io, std::future<T>& result) {
   io.restart();
   io.run_for(std::chrono::milliseconds{250});
   BOOST_REQUIRE(result.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready);
   return result.get();
}

} // namespace

BOOST_AUTO_TEST_SUITE(p2p_host_events)

BOOST_AUTO_TEST_CASE(initial_snapshot_and_monotonic_generation) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{2};
   source.publish(host_event{.effective = reachability::state::publicly_reachable});
   auto subscription = source.subscribe();
   auto first = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   const auto initial = event_result(io, first);
   BOOST_REQUIRE(initial);
   BOOST_TEST(initial->generation == 1U);
   BOOST_TEST(!initial->resync_required);
   BOOST_CHECK(initial->effective == reachability::state::publicly_reachable);
   source.publish(host_event{.effective = reachability::state::private_network});
   auto next = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   const auto update = event_result(io, next);
   BOOST_REQUIRE(update);
   BOOST_TEST(update->generation == 2U);
   BOOST_CHECK(update->effective == reachability::state::private_network);
   BOOST_TEST(!update->resync_required);
}

BOOST_AUTO_TEST_CASE(slow_readers_coalesce_and_report_resync) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{2};
   auto subscription = source.subscribe();
   for (auto n = 0; n < 10000; ++n) {
      source.publish(host_event{});
   }
   auto next = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   const auto update = event_result(io, next);
   BOOST_REQUIRE(update);
   BOOST_TEST(update->generation == 10000U);
   BOOST_TEST(update->resync_required);
}

BOOST_AUTO_TEST_CASE(subscriber_limit_reuses_closed_slots) {
   auto source = detail::host_event_source{1};
   auto first = source.subscribe();
   BOOST_CHECK_THROW(source.subscribe(), exceptions::backpressure_rejected);
   first.close();
   auto replacement = source.subscribe();
   BOOST_TEST(replacement.active());
   BOOST_TEST(!first.active());
}

BOOST_AUTO_TEST_CASE(shutdown_wakes_blocked_and_late_readers) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{1};
   auto subscription = source.subscribe();
   auto initial = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(event_result(io, initial));
   auto pending = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   BOOST_CHECK(pending.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready);
   source.close();
   BOOST_TEST(!event_result(io, pending).has_value());
   auto late = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_TEST(!event_result(io, late).has_value());
   BOOST_CHECK_THROW(source.subscribe(), exceptions::closed);
}

BOOST_AUTO_TEST_CASE(move_assignment_closes_only_replaced_subscription) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{2};
   auto first = source.subscribe();
   auto second = source.subscribe();
   auto initial = boost::asio::co_spawn(io, first.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(event_result(io, initial));
   auto pending = boost::asio::co_spawn(io, first.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   first = std::move(second);
   BOOST_TEST(!event_result(io, pending).has_value());
   BOOST_TEST(first.active());
   BOOST_TEST(!second.active());
}

BOOST_AUTO_TEST_CASE(operation_owns_state_before_first_suspension) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{1};
   auto operation = [&] {
      auto subscription = source.subscribe();
      return subscription.async_read();
   }();
   auto pending = boost::asio::co_spawn(io, std::move(operation), boost::asio::use_future);
   BOOST_TEST(!event_result(io, pending).has_value());
}

BOOST_AUTO_TEST_CASE(cancelled_reader_does_not_poison_next_read) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{1};
   auto subscription = source.subscribe();
   auto initial = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(event_result(io, initial));
   auto cancellation = boost::asio::cancellation_signal{};
   auto pending = boost::asio::co_spawn(io, subscription.async_read(),
       boost::asio::bind_cancellation_slot(cancellation.slot(), boost::asio::use_future));
   io.restart();
   io.poll();
   cancellation.emit(boost::asio::cancellation_type::all);
   BOOST_CHECK_THROW(event_result(io, pending), std::exception);
   source.publish(host_event{});
   auto next = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(event_result(io, next));
}

BOOST_AUTO_TEST_CASE(subscribe_and_publish_are_atomic) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{64};
   auto writer = std::jthread{[&] {
      for (auto n = 0; n < 1000; ++n) {
         source.publish(host_event{});
      }
   }};
   auto subscriptions = std::vector<host_event_subscription>{};
   for (auto n = 0; n < 64; ++n) {
      subscriptions.push_back(source.subscribe());
   }
   writer.join();
   for (auto& subscription : subscriptions) {
      auto next = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
      const auto value = event_result(io, next);
      BOOST_REQUIRE(value);
      BOOST_TEST(value->generation == 1000U);
   }
}

BOOST_AUTO_TEST_CASE(concurrent_read_is_rejected_without_losing_original_waiter) {
   auto io = boost::asio::io_context{};
   auto source = detail::host_event_source{1};
   auto subscription = source.subscribe();
   auto initial = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(event_result(io, initial));
   auto first = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   auto second = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_CHECK_THROW(event_result(io, second), exceptions::invalid_options);
   BOOST_CHECK(first.wait_for(std::chrono::milliseconds{0}) != std::future_status::ready);
   source.publish(host_event{});
   BOOST_REQUIRE(event_result(io, first));
}

BOOST_AUTO_TEST_CASE(source_destruction_wakes_surviving_subscription) {
   auto io = boost::asio::io_context{};
   auto source = std::make_unique<detail::host_event_source>(1);
   auto subscription = source->subscribe();
   auto initial = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   BOOST_REQUIRE(event_result(io, initial));
   auto pending = boost::asio::co_spawn(io, subscription.async_read(), boost::asio::use_future);
   io.restart();
   io.poll();
   source.reset();
   BOOST_TEST(!event_result(io, pending).has_value());
   BOOST_TEST(!subscription.active());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::p2p
