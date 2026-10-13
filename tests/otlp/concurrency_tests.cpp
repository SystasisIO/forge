#include "controlled_collector.hxx"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

#include <chrono>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.log.record;
import forge.otlp.exceptions;
import forge.otlp.log_exporter;
import forge.otlp.options;

namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;
using otlp_concurrency_fixtures::controlled_collector;

forge::log_record concurrent_record(std::string message) {
   return {.level = forge::log_level::info, .logger = "otlp.concurrent", .message = std::move(message)};
}

forge::otlp::log_exporter_options concurrent_options(const controlled_collector& collector) {
   auto result = forge::otlp::log_exporter_options{};
   result.endpoint = collector.endpoint();
   result.batch.flush_interval = 1h;
   result.retry.max_attempts = 0;
   result.request_timeout = 10s;
   result.shutdown_timeout = 5s;
   return result;
}

asio::awaitable<bool> observe_closed(forge::otlp::log_exporter& exporter) {
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   while (std::chrono::steady_clock::now() < deadline) {
      try {
         static_cast<void>(co_await exporter.async_export({}));
      } catch (const forge::otlp::exceptions::queue_closed&) {
         co_return true;
      }
   }
   co_return false;
}

asio::awaitable<void> sample_metrics(forge::otlp::log_exporter& exporter) {
   const auto executor = co_await asio::this_coro::executor;
   for (auto count = 0; count != 128; ++count) {
      static_cast<void>(exporter.metrics());
      co_await asio::post(executor, asio::use_awaitable);
   }
}

asio::awaitable<bool> observe_started_retry(forge::otlp::log_exporter& exporter) {
   const auto deadline = std::chrono::steady_clock::now() + 2s;
   while (std::chrono::steady_clock::now() < deadline) {
      if (exporter.metrics().retry_attempts == 1U) {
         // This empty operation executes on the owning strand. It cannot run
         // until the observed retry coroutine has yielded at its timer wait.
         static_cast<void>(co_await exporter.async_export({}));
         const auto snapshot = exporter.metrics();
         co_return snapshot.retry_attempts == 1U && snapshot.export_attempts == 1U &&
                   snapshot.exported_records == 0U && snapshot.failed_records == 0U;
      }
      static_cast<void>(co_await exporter.async_export({}));
   }
   co_return false;
}

template <typename T> void require_ready(std::future<T>& future) {
   BOOST_REQUIRE(future.wait_for(3s) == std::future_status::ready);
}

} // namespace

BOOST_AUTO_TEST_SUITE(otlp_concurrency_test_suite)

BOOST_AUTO_TEST_CASE(returned_awaitables_pin_impl_before_coroutine_execution) {
   auto owner = forge::asio::runtime{{.worker_threads = 3}};
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   auto exporter = std::make_unique<forge::otlp::log_exporter>(owner);
   auto flush = exporter->async_flush();
   auto shutdown = exporter->async_shutdown();
   auto direct = exporter->async_export({concurrent_record("delayed")});
   exporter.reset();

   auto flush_done = asio::co_spawn(caller.context(), std::move(flush), asio::use_future);
   auto shutdown_done = asio::co_spawn(caller.context(), std::move(shutdown), asio::use_future);
   auto direct_done = asio::co_spawn(caller.context(), std::move(direct), asio::use_future);
   require_ready(flush_done);
   require_ready(shutdown_done);
   require_ready(direct_done);
   flush_done.get();
   shutdown_done.get();
   BOOST_CHECK_THROW(static_cast<void>(direct_done.get()), forge::otlp::exceptions::queue_closed);
}

BOOST_AUTO_TEST_CASE(multiple_executors_share_inflight_flush_and_shutdown_completion) {
   for (const auto status : {forge::net::http::status::ok, forge::net::http::status::bad_request}) {
      auto owner = forge::asio::runtime{{.worker_threads = 4}};
      auto first_caller = forge::asio::runtime{{.worker_threads = 2}};
      auto second_caller = forge::asio::runtime{{.worker_threads = 2}};
      auto collector = controlled_collector{owner, status};
      auto exporter = forge::otlp::log_exporter{owner, concurrent_options(collector)};
      BOOST_REQUIRE(exporter.enqueue(concurrent_record("queued")));
      auto first_flush = asio::co_spawn(first_caller.context(), exporter.async_flush(), asio::use_future);
      BOOST_REQUIRE(collector.wait_for_requests(1));
      auto second_flush = asio::co_spawn(second_caller.context(), exporter.async_flush(), asio::use_future);
      auto first_metrics = asio::co_spawn(first_caller.context(), sample_metrics(exporter), asio::use_future);
      auto second_metrics = asio::co_spawn(second_caller.context(), sample_metrics(exporter), asio::use_future);
      auto first_shutdown = asio::co_spawn(first_caller.context(), exporter.async_shutdown(), asio::use_future);
      auto closed = asio::co_spawn(second_caller.context(), observe_closed(exporter), asio::use_future);
      require_ready(closed);
      BOOST_REQUIRE(closed.get());
      auto second_shutdown = asio::co_spawn(second_caller.context(), exporter.async_shutdown(), asio::use_future);
      BOOST_CHECK(first_shutdown.wait_for(0ms) == std::future_status::timeout);
      BOOST_CHECK(second_shutdown.wait_for(0ms) == std::future_status::timeout);
      BOOST_CHECK(first_flush.wait_for(0ms) == std::future_status::timeout);
      BOOST_CHECK(second_flush.wait_for(0ms) == std::future_status::timeout);
      collector.release();

      require_ready(first_flush);
      require_ready(second_flush);
      require_ready(first_shutdown);
      require_ready(second_shutdown);
      require_ready(first_metrics);
      require_ready(second_metrics);
      first_flush.get();
      second_flush.get();
      first_shutdown.get();
      second_shutdown.get();
      first_metrics.get();
      second_metrics.get();
      const auto metrics = exporter.metrics();
      BOOST_TEST(metrics.export_attempts == 1U);
      BOOST_TEST(metrics.queue_depth == 0U);
      BOOST_TEST(metrics.exported_records == (status == forge::net::http::status::ok ? 1U : 0U));
      BOOST_TEST(metrics.failed_records == (status == forge::net::http::status::ok ? 0U : 1U));
      BOOST_TEST(metrics.dropped_records == metrics.failed_records);
      BOOST_CHECK(!exporter.enqueue(concurrent_record("late")));
      forge::asio::blocking::run(second_caller, exporter.async_shutdown());
   }
}

BOOST_AUTO_TEST_CASE(cancelling_flush_waiter_does_not_cancel_shared_export) {
   auto owner = forge::asio::runtime{{.worker_threads = 3}};
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   auto caller_strand = asio::make_strand(caller.context());
   auto collector = controlled_collector{owner};
   auto exporter = forge::otlp::log_exporter{owner, concurrent_options(collector)};
   BOOST_REQUIRE(exporter.enqueue(concurrent_record("survives-caller-cancellation")));
   auto cancellation = asio::cancellation_signal{};
   auto cancelled = asio::co_spawn(caller_strand, exporter.async_flush(),
                                  asio::bind_cancellation_slot(cancellation.slot(), asio::use_future));
   BOOST_REQUIRE(collector.wait_for_requests(1));
   auto surviving = asio::co_spawn(owner.context(), exporter.async_flush(), asio::use_future);
   asio::post(caller_strand, [&] { cancellation.emit(asio::cancellation_type::all); });
   require_ready(cancelled);
   BOOST_CHECK_THROW(cancelled.get(), boost::system::system_error);
   BOOST_CHECK(surviving.wait_for(0ms) == std::future_status::timeout);
   collector.release();
   require_ready(surviving);
   surviving.get();
   forge::asio::blocking::run(caller, exporter.async_shutdown());
   BOOST_TEST(exporter.metrics().exported_records == 1U);
   BOOST_TEST(exporter.metrics().failed_records == 0U);
}

BOOST_AUTO_TEST_CASE(shutdown_deadline_cancels_preexisting_direct_request_and_joins_it) {
   auto owner = forge::asio::runtime{{.worker_threads = 3}};
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   auto collector = controlled_collector{owner};
   auto options = concurrent_options(collector);
   options.shutdown_timeout = 100ms;
   options.retry.max_attempts = 100;
   auto exporter = forge::otlp::log_exporter{owner, options};
   auto direct = asio::co_spawn(caller.context(), exporter.async_export({concurrent_record("in-flight")}),
                               asio::use_future);
   BOOST_REQUIRE(collector.wait_for_requests(1));
   const auto started = std::chrono::steady_clock::now();
   auto shutdown = asio::co_spawn(caller.context(), exporter.async_shutdown(), asio::use_future);
   require_ready(shutdown);
   shutdown.get();
   const auto elapsed = std::chrono::steady_clock::now() - started;
   BOOST_CHECK(elapsed < 2s);
   require_ready(direct);
   try {
      BOOST_TEST(direct.get().failed_records == 1U);
   } catch (const boost::system::system_error&) {
      // Cancellation is also a valid direct-operation terminal outcome.
   }
   const auto metrics = exporter.metrics();
   BOOST_TEST(metrics.failed_records == 1U);
   BOOST_TEST(metrics.retry_attempts == 0U);
   BOOST_TEST(metrics.export_attempts == 1U);
   BOOST_TEST(collector.bodies().size() == 1U);
}

BOOST_AUTO_TEST_CASE(facade_destruction_cancels_owned_inflight_work_on_exporter_strand) {
   auto owner = forge::asio::runtime{{.worker_threads = 4}};
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   auto collector = controlled_collector{owner};
   auto exporter = std::make_unique<forge::otlp::log_exporter>(owner, concurrent_options(collector));
   BOOST_REQUIRE(exporter->enqueue(concurrent_record("abandoned")));
   auto flush = asio::co_spawn(caller.context(), exporter->async_flush(), asio::use_future);
   BOOST_REQUIRE(collector.wait_for_requests(1));
   exporter.reset();
   require_ready(flush);
   try {
      flush.get();
   } catch (const boost::system::system_error&) {
      // The worker may complete with cancellation rather than a dropped batch.
   }
   BOOST_TEST(collector.bodies().size() == 1U);
}

BOOST_AUTO_TEST_CASE(shutdown_deadline_cancels_started_queued_retry_after_http_503) {
   auto owner = forge::asio::runtime{{.worker_threads = 3}};
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   auto collector = controlled_collector{owner, forge::net::http::status::service_unavailable};
   auto options = concurrent_options(collector);
   options.shutdown_timeout = 100ms;
   options.retry = {.max_attempts = 100, .base_delay = 5s, .max_delay = 5s};
   auto exporter = forge::otlp::log_exporter{owner, options};
   BOOST_REQUIRE(exporter.enqueue(concurrent_record("queued-retry")));
   auto flush = asio::co_spawn(caller.context(), exporter.async_flush(), asio::use_future);
   BOOST_REQUIRE(collector.wait_for_requests(1));
   collector.release();
   auto retry_started = asio::co_spawn(caller.context(), observe_started_retry(exporter), asio::use_future);
   require_ready(retry_started);
   BOOST_REQUIRE(retry_started.get());
   BOOST_CHECK(flush.wait_for(0ms) == std::future_status::timeout);

   const auto started = std::chrono::steady_clock::now();
   auto shutdown = asio::co_spawn(caller.context(), exporter.async_shutdown(), asio::use_future);
   require_ready(shutdown);
   shutdown.get();
   const auto elapsed = std::chrono::steady_clock::now() - started;
   BOOST_CHECK(elapsed < 2s);
   require_ready(flush);
   try {
      flush.get();
   } catch (const boost::system::system_error& error) {
      BOOST_CHECK(error.code() == asio::error::operation_aborted);
   }
   const auto snapshot = exporter.metrics();
   BOOST_TEST(snapshot.enqueued_records == 1U);
   BOOST_TEST(snapshot.exported_records == 0U);
   BOOST_TEST(snapshot.failed_records == 1U);
   BOOST_TEST(snapshot.dropped_records == 1U);
   BOOST_TEST(snapshot.queue_depth == 0U);
   BOOST_TEST(snapshot.queue_bytes == 0U);
   BOOST_TEST(snapshot.export_attempts == 1U);
   BOOST_TEST(snapshot.retry_attempts == 1U);
   BOOST_TEST(collector.bodies().size() == 1U);
}

BOOST_AUTO_TEST_CASE(shutdown_deadline_cancels_started_direct_retry_after_http_503) {
   auto owner = forge::asio::runtime{{.worker_threads = 3}};
   auto caller = forge::asio::runtime{{.worker_threads = 2}};
   auto collector = controlled_collector{owner, forge::net::http::status::service_unavailable};
   auto options = concurrent_options(collector);
   options.shutdown_timeout = 100ms;
   options.retry = {.max_attempts = 100, .base_delay = 5s, .max_delay = 5s};
   auto exporter = forge::otlp::log_exporter{owner, options};
   auto direct = asio::co_spawn(caller.context(), exporter.async_export({concurrent_record("direct-retry")}),
                               asio::use_future);
   BOOST_REQUIRE(collector.wait_for_requests(1));
   collector.release();
   auto retry_started = asio::co_spawn(caller.context(), observe_started_retry(exporter), asio::use_future);
   require_ready(retry_started);
   BOOST_REQUIRE(retry_started.get());
   BOOST_CHECK(direct.wait_for(0ms) == std::future_status::timeout);

   const auto started = std::chrono::steady_clock::now();
   auto shutdown = asio::co_spawn(caller.context(), exporter.async_shutdown(), asio::use_future);
   require_ready(shutdown);
   shutdown.get();
   const auto elapsed = std::chrono::steady_clock::now() - started;
   BOOST_CHECK(elapsed < 2s);
   require_ready(direct);
   try {
      const auto result = direct.get();
      BOOST_TEST(result.submitted_records == 1U);
      BOOST_TEST(result.exported_records == 0U);
      BOOST_TEST(result.failed_records == 1U);
   } catch (const boost::system::system_error& error) {
      BOOST_CHECK(error.code() == asio::error::operation_aborted);
   }
   const auto snapshot = exporter.metrics();
   BOOST_TEST(snapshot.enqueued_records == 0U);
   BOOST_TEST(snapshot.exported_records == 0U);
   BOOST_TEST(snapshot.failed_records == 1U);
   BOOST_TEST(snapshot.dropped_records == 0U);
   BOOST_TEST(snapshot.queue_depth == 0U);
   BOOST_TEST(snapshot.queue_bytes == 0U);
   BOOST_TEST(snapshot.export_attempts == 1U);
   BOOST_TEST(snapshot.retry_attempts == 1U);
   BOOST_TEST(collector.bodies().size() == 1U);
}

BOOST_AUTO_TEST_SUITE_END()
