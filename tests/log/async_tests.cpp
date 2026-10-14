#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/log/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <source_location>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.compute;
import forge.asio.runtime;
import forge.log.async_sink;
import forge.log.exceptions;
import forge.log.logger;
import forge.log.logger_config;
import forge.log.record;

namespace {

using namespace std::chrono_literals;

class test_signal {
 public:
   void advance() {
      const auto lock = std::scoped_lock{_mutex};
      ++_count;
      _changed.notify_all();
   }

   bool wait_for(std::size_t count, std::chrono::milliseconds timeout = 2s) {
      auto lock = std::unique_lock{_mutex};
      return _changed.wait_for(lock, timeout, [&] { return _count >= count; });
   }

   std::size_t count() {
      const auto lock = std::scoped_lock{_mutex};
      return _count;
   }

 private:
   std::mutex _mutex;
   std::condition_variable _changed;
   std::size_t _count = 0;
};

class controlled_output {
 public:
   void enter() {
      auto lock = std::unique_lock{_mutex};
      const auto ordinal = ++_entered;
      _maximum_active = std::max(_maximum_active, ++_active);
      _changed.notify_all();
      // A broken inline delivery or a failed assertion must not strand a worker.
      if (!_changed.wait_for(lock, 4s, [&] { return _released || _permits >= ordinal; })) {
         ++_timed_out;
      }
   }

   void capture(const forge::log_record& record) {
      const auto lock = std::scoped_lock{_mutex};
      _records.push_back(record);
   }

   void leave() {
      const auto lock = std::scoped_lock{_mutex};
      --_active;
      ++_finished;
      _changed.notify_all();
   }

   void release_through(std::size_t count) {
      const auto lock = std::scoped_lock{_mutex};
      _permits = std::max(_permits, count);
      _changed.notify_all();
   }

   void release_all() {
      const auto lock = std::scoped_lock{_mutex};
      _released = true;
      _changed.notify_all();
   }

   bool wait_entered(std::size_t count) {
      auto lock = std::unique_lock{_mutex};
      return _changed.wait_for(lock, 2s, [&] { return _entered >= count; });
   }

   std::size_t finished() const {
      const auto lock = std::scoped_lock{_mutex};
      return _finished;
   }

   std::size_t timed_out() const {
      const auto lock = std::scoped_lock{_mutex};
      return _timed_out;
   }

   std::size_t maximum_active() const {
      const auto lock = std::scoped_lock{_mutex};
      return _maximum_active;
   }

   std::vector<forge::log_record> records() const {
      const auto lock = std::scoped_lock{_mutex};
      return _records;
   }

 private:
   mutable std::mutex _mutex;
   std::condition_variable _changed;
   std::size_t _entered = 0;
   std::size_t _finished = 0;
   std::size_t _active = 0;
   std::size_t _maximum_active = 0;
   std::size_t _permits = 0;
   std::size_t _timed_out = 0;
   bool _released = false;
   std::vector<forge::log_record> _records;
};

class controlled_sink final : public forge::sink {
 public:
   explicit controlled_sink(std::shared_ptr<controlled_output> output,
                            std::function<void(const forge::log_record&)> callback = {})
       : _output{std::move(output)}, _callback{std::move(callback)} {}

   void log(const forge::log_record& record) override {
      _output->enter();
      const auto on_exit = exit_guard{_output};
      // Inspect the borrowed record AFTER release, not before the lifetime test.
      _output->capture(record);
      if (_callback) {
         _callback(record);
      }
   }

 private:
   struct exit_guard {
      std::shared_ptr<controlled_output> output;
      ~exit_guard() {
         output->leave();
      }
   };

   std::shared_ptr<controlled_output> _output;
   std::function<void(const forge::log_record&)> _callback;
};

template <typename T> T require_result(std::future<T>& result) {
   BOOST_REQUIRE(result.wait_for(3s) == std::future_status::ready);
   return result.get();
}

struct async_fixture {
   explicit async_fixture(forge::async_sink_options options = {},
                          std::function<void(const forge::log_record&)> callback = {},
                          forge::asio::compute::pool::options compute_options = {
                              .worker_threads = 4, .thread_name = "log-test-output"})
       : pool{std::move(compute_options)},
         downstream{std::make_shared<controlled_sink>(output, std::move(callback))},
         adapter{std::make_shared<forge::async_sink>(downstream, pool.get_executor(), options)} {}

   ~async_fixture() {
      // This runs before any member destructor, especially the pool's join.
      output->release_all();
      if (adapter) {
         adapter->close();
      }
   }

   void require_blocked() {
      BOOST_REQUIRE(output->wait_entered(1));
      BOOST_REQUIRE_EQUAL(output->finished(), 0U);
      BOOST_REQUIRE_EQUAL(output->timed_out(), 0U);
   }

   bool wait(boost::asio::awaitable<bool> operation) {
      auto result = boost::asio::co_spawn(runtime.context(), std::move(operation), boost::asio::use_future);
      return require_result(result);
   }

   void join_pool() {
      output->release_all();
      BOOST_REQUIRE(forge::asio::blocking::run_for(runtime, pool.shutdown(), 3s));
      BOOST_TEST(output->timed_out() == 0U);
   }

   void finish() {
      output->release_all();
      BOOST_REQUIRE(wait(adapter->async_shutdown(2s)));
      const auto snapshot = adapter->snapshot();
      BOOST_TEST(snapshot.queued_records == 0U);
      BOOST_TEST(snapshot.queued_bytes == 0U);
      BOOST_TEST(snapshot.accepted == snapshot.delivered + snapshot.failed);
      join_pool();
   }

   forge::asio::runtime runtime{{.worker_threads = 1}};
   forge::asio::compute::pool pool;
   std::shared_ptr<controlled_output> output = std::make_shared<controlled_output>();
   std::shared_ptr<forge::sink> downstream;
   std::shared_ptr<forge::async_sink> adapter;
};

class producer_group {
 public:
   explicit producer_group(std::shared_ptr<controlled_output> output) : _output{std::move(output)} {}

   ~producer_group() {
      // Release callbacks AND partially constructed start groups before joining.
      _output->release_all();
      start();
      for (auto& thread : _threads) {
         thread.join();
      }
   }

   template <typename Work> void add(Work work) {
      auto task = std::make_shared<std::packaged_task<void()>>(
          [start = _start, work = std::move(work)]() mutable {
             if (!start->wait_for(1, 4s)) {
                throw std::runtime_error{"test producer start timed out"};
             }
             work();
          });
      _results.push_back(task->get_future());
      _threads.emplace_back([task = std::move(task), done = _done] {
         (*task)();
         done->advance();
      });
   }

   void start() {
      _start->advance();
   }

   bool wait() {
      return _done->wait_for(_threads.size());
   }

   void join() {
      for (auto& thread : _threads) {
         thread.join();
      }
      _threads.clear();
      // Unexpected producer exceptions are rethrown on the test thread.
      for (auto& result : _results) {
         result.get();
      }
      _results.clear();
   }

 private:
   std::shared_ptr<controlled_output> _output;
   std::shared_ptr<test_signal> _start = std::make_shared<test_signal>();
   std::shared_ptr<test_signal> _done = std::make_shared<test_signal>();
   std::vector<std::jthread> _threads;
   std::vector<std::future<void>> _results;
};

forge::log_record test_record(std::string message) {
   return {.level = forge::log_level::info, .logger = "async.test", .message = std::move(message)};
}

boost::asio::awaitable<void> runtime_probe() {
   co_await boost::asio::post(co_await boost::asio::this_coro::executor, boost::asio::use_awaitable);
}

boost::asio::awaitable<bool> started_flush(boost::asio::awaitable<bool> operation,
                                         std::shared_ptr<test_signal> started) {
   started->advance();
   co_return co_await std::move(operation);
}

} // namespace

BOOST_AUTO_TEST_SUITE(log_async_test_suite)

BOOST_AUTO_TEST_CASE(blocked_output_does_not_block_32_producers_or_runtime, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{{.max_records = 64}};
   auto route = forge::logger{"async.producers"};
   route.set_log_level(forge::log_level::info);
   route.add_sink(fixture.adapter);
   ilog(route, "blocked");
   fixture.require_blocked();

   auto producers = producer_group{fixture.output};
   for (int producer = 0; producer < 32; ++producer) {
      producers.add([route, producer]() mutable {
         for (int item = 0; item < 16; ++item) {
            ilog(route, "event ${producer}/${item}", ("producer", producer)("item", item));
         }
      });
   }
   producers.start();
   BOOST_REQUIRE(producers.wait());
   producers.join();
   BOOST_TEST(fixture.output->finished() == 0U);
   BOOST_REQUIRE(forge::asio::blocking::run_for(fixture.runtime, runtime_probe(), 1s));
   const auto snapshot = fixture.adapter->snapshot();
   BOOST_TEST(snapshot.accepted == 64U);
   BOOST_TEST(snapshot.dropped == 449U);
   BOOST_TEST(snapshot.queued_records == 64U);
   BOOST_TEST(snapshot.queued_bytes <= 8U * 1024U * 1024U);
   BOOST_TEST(snapshot.delivered == 0U);
   // One bounded pump, not a new compute task per admitted or dropped record.
   BOOST_TEST(fixture.pool.snapshot().submitted == 1U);
   fixture.finish();
   BOOST_TEST(fixture.adapter->snapshot().delivered == 64U);
}

BOOST_AUTO_TEST_CASE(record_limit_includes_the_active_callback, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{{.max_records = 2}};
   fixture.adapter->log(test_record("first"));
   fixture.require_blocked();
   fixture.adapter->log(test_record("second"));
   fixture.adapter->log(test_record("overflow"));
   const auto snapshot = fixture.adapter->snapshot();
   BOOST_TEST(snapshot.accepted == 2U);
   BOOST_TEST(snapshot.queued_records == 2U);
   BOOST_TEST(snapshot.dropped == 1U);
   fixture.finish();
   const auto records = fixture.output->records();
   BOOST_REQUIRE_EQUAL(records.size(), 2U);
   BOOST_TEST(records[0].message == "first");
   BOOST_TEST(records[1].message == "second");
}

BOOST_AUTO_TEST_CASE(byte_limit_includes_active_record_and_drops_oversized_input, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{{.max_records = 64, .max_bytes = 4096}};
   fixture.adapter->log(test_record(std::string(2500, 'a')));
   fixture.require_blocked();
   const auto active = fixture.adapter->snapshot();
   BOOST_REQUIRE(active.queued_bytes > 2048U);
   BOOST_REQUIRE(active.queued_bytes <= 4096U);
   fixture.adapter->log(test_record(std::string(2500, 'b')));
   fixture.adapter->log(test_record(std::string(8192, 'c')));
   const auto full = fixture.adapter->snapshot();
   BOOST_TEST(full.accepted == 1U);
   BOOST_TEST(full.dropped == 2U);
   BOOST_TEST(full.queued_bytes == active.queued_bytes);
   fixture.output->release_all();
   BOOST_REQUIRE(fixture.wait(fixture.adapter->async_flush(2s)));
   // An oversized input must also be rejected when the queue is empty.
   fixture.adapter->log(test_record(std::string(8192, 'd')));
   BOOST_TEST(fixture.adapter->snapshot().dropped == 3U);
   fixture.adapter->log(test_record("after oversized drop"));
   fixture.finish();
   BOOST_TEST(fixture.adapter->snapshot().delivered == 2U);
}

BOOST_AUTO_TEST_CASE(fifo_delivery_is_serial_even_with_four_compute_workers, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{{.max_records = 256}};
   fixture.adapter->log(test_record("0"));
   fixture.require_blocked();
   for (int item = 1; item < 256; ++item) {
      fixture.adapter->log(test_record(std::to_string(item)));
   }
   BOOST_TEST(fixture.adapter->snapshot().queued_records == 256U);
   BOOST_TEST(fixture.pool.snapshot().submitted == 1U);
   fixture.finish();
   const auto records = fixture.output->records();
   BOOST_REQUIRE_EQUAL(records.size(), 256U);
   for (int item = 0; item < 256; ++item) {
      BOOST_TEST(records[item].message == std::to_string(item));
   }
   BOOST_TEST(fixture.output->maximum_active() == 1U);
}

BOOST_AUTO_TEST_CASE(downstream_exceptions_are_counted_and_do_not_stop_the_pump, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{{}, [](const forge::log_record& record) {
      if (record.message == "failure") {
         throw std::runtime_error{"private-synthetic-sink-error"};
      }
   }};
   fixture.output->release_all();
   for (int item = 0; item < 32; ++item) {
      fixture.adapter->log(test_record("failure"));
      fixture.adapter->log(test_record("success"));
   }
   BOOST_REQUIRE(fixture.wait(fixture.adapter->async_flush(2s)));
   const auto snapshot = fixture.adapter->snapshot();
   BOOST_TEST(snapshot.accepted == 64U);
   BOOST_TEST(snapshot.failed == 32U);
   BOOST_TEST(snapshot.delivered == 32U);
   BOOST_TEST(snapshot.dropped == 0U);
   BOOST_TEST(fixture.output->records().size() == 64U);
   fixture.finish();
}

BOOST_AUTO_TEST_CASE(flush_timeout_does_not_complete_a_blocked_record_and_can_be_retried, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   fixture.adapter->log(test_record("blocked"));
   fixture.require_blocked();
   BOOST_TEST(!fixture.wait(fixture.adapter->async_flush(30ms)));
   BOOST_TEST(!fixture.wait(fixture.adapter->async_flush(0ms)));
   const auto pending = fixture.adapter->snapshot();
   BOOST_TEST(pending.accepted == 1U);
   BOOST_TEST(pending.delivered == 0U);
   BOOST_TEST(pending.failed == 0U);
   BOOST_TEST(pending.queued_records == 1U);
   BOOST_TEST(pending.queued_bytes > 0U);
   BOOST_TEST(!pending.closed);
   BOOST_TEST(fixture.output->finished() == 0U);
   fixture.output->release_all();
   BOOST_REQUIRE(fixture.wait(fixture.adapter->async_flush(2s)));
   fixture.adapter->log(test_record("accepted after flush timeout"));
   fixture.finish();
   BOOST_TEST(fixture.adapter->snapshot().delivered == 2U);
}

BOOST_AUTO_TEST_CASE(shutdown_timeout_closes_admission_but_preserves_running_work, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   fixture.adapter->log(test_record("active"));
   fixture.require_blocked();
   fixture.adapter->log(test_record("queued"));
   BOOST_TEST(!fixture.wait(fixture.adapter->async_shutdown(30ms)));
   fixture.adapter->close();
   fixture.adapter->close();
   fixture.adapter->log(test_record("closed drop"));
   const auto pending = fixture.adapter->snapshot();
   BOOST_TEST(pending.closed);
   BOOST_TEST(pending.accepted == 2U);
   BOOST_TEST(pending.delivered == 0U);
   BOOST_TEST(pending.failed == 0U);
   BOOST_TEST(pending.queued_records == 2U);
   BOOST_TEST(pending.dropped == 1U);
   auto retry = boost::asio::co_spawn(fixture.runtime.context(), fixture.adapter->async_shutdown(2s),
                                     boost::asio::use_future);
   BOOST_CHECK(retry.wait_for(20ms) == std::future_status::timeout);
   BOOST_TEST(fixture.output->finished() == 0U);
   fixture.output->release_all();
   BOOST_REQUIRE(require_result(retry));
   fixture.finish();
   BOOST_TEST(fixture.adapter->snapshot().delivered == 2U);
}

BOOST_AUTO_TEST_CASE(flush_fence_excludes_continuously_admitted_newer_records, * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{{.max_records = 128}};
   fixture.adapter->log(test_record("older first"));
   fixture.require_blocked();
   fixture.adapter->log(test_record("older second"));
   auto operation = fixture.adapter->async_flush(2s);
   // The facade captures the fence now, before this awaitable is even started.
   fixture.adapter->log(test_record("newer blocked"));
   auto flush = boost::asio::co_spawn(fixture.runtime.context(), std::move(operation), boost::asio::use_future);

   auto stop = std::atomic_bool{false};
   auto emitted = test_signal{};
   auto stop_mutex = std::mutex{};
   auto stop_changed = std::condition_variable{};
   auto producers = producer_group{fixture.output};
   producers.add([&, adapter = fixture.adapter] {
      const auto deadline = std::chrono::steady_clock::now() + 3s;
      while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
         adapter->log(test_record("newer continuous"));
         emitted.advance();
         auto lock = std::unique_lock{stop_mutex};
         stop_changed.wait_for(lock, 1ms, [&] { return stop.load(); });
      }
   });
   producers.start();
   BOOST_REQUIRE(emitted.wait_for(8));
   fixture.output->release_through(2);
   BOOST_REQUIRE(fixture.output->wait_entered(3));
   BOOST_REQUIRE(require_result(flush));
   const auto at_fence = fixture.adapter->snapshot();
   BOOST_TEST(at_fence.delivered == 2U);
   BOOST_TEST(at_fence.failed == 0U);
   BOOST_TEST(at_fence.queued_records > 0U);
   BOOST_TEST(fixture.output->finished() == 2U);
   const auto after_flush = emitted.count();
   BOOST_REQUIRE(emitted.wait_for(after_flush + 8));
   BOOST_TEST(fixture.adapter->snapshot().delivered == 2U);
   stop.store(true);
   stop_changed.notify_all();
   BOOST_REQUIRE(producers.wait());
   producers.join();
   const auto before_release = fixture.adapter->snapshot();
   BOOST_TEST(before_release.accepted + before_release.dropped == emitted.count() + 3U);
   fixture.finish();
   const auto records = fixture.output->records();
   BOOST_REQUIRE(records.size() >= 3U);
   BOOST_TEST(records[0].message == "older first");
   BOOST_TEST(records[1].message == "older second");
   BOOST_TEST(records[2].message == "newer blocked");
}

BOOST_AUTO_TEST_CASE(recursion_is_suppressed_across_async_sinks_and_guard_survives_exceptions,
                     * boost::unit_test::timeout(20)) {
   auto route = forge::logger{"async.recursive.origin"};
   auto nested = forge::logger{"async.recursive.nested"};
   route.set_log_level(forge::log_level::info);
   nested.set_log_level(forge::log_level::info);
   auto self = std::weak_ptr<forge::async_sink>{};
   auto fixture = async_fixture{{}, [&](const forge::log_record& record) {
      if (record.message != "outer") {
         return;
      }
      route.info("same route recursion");
      auto adapter = self.lock();
      if (!adapter) {
         throw std::logic_error{"recursive test lost its live adapter"};
      }
      adapter->log(test_record("direct recursion"));
      nested.info("different async route recursion");
      throw std::runtime_error{"synthetic exception after recursive logging"};
   }};
   self = fixture.adapter;
   fixture.output->release_all();
   auto other_output = std::make_shared<controlled_output>();
   other_output->release_all();
   auto other_sink = std::make_shared<controlled_sink>(other_output);
   auto other = std::make_shared<forge::async_sink>(other_sink, fixture.pool.get_executor());
   auto sibling_output = std::make_shared<controlled_output>();
   sibling_output->release_all();
   auto sibling = std::make_shared<controlled_sink>(sibling_output);
   route.add_sink(fixture.adapter);
   nested.add_sink(other);
   nested.add_sink(sibling);

   for (int attempt = 0; attempt < 2; ++attempt) {
      route.info("outer");
      BOOST_REQUIRE(fixture.wait(fixture.adapter->async_flush(2s)));
   }
   route.info("ordinary after exceptions");
   nested.info("ordinary secondary emission");
   BOOST_REQUIRE(fixture.wait(other->async_shutdown(2s)));
   fixture.finish();
   const auto primary = fixture.adapter->snapshot();
   BOOST_TEST(primary.accepted == 3U);
   BOOST_TEST(primary.delivered == 1U);
   BOOST_TEST(primary.failed == 2U);
   BOOST_TEST(primary.dropped == 4U);
   BOOST_TEST(primary.suppressed_reentrant == 4U);
   const auto secondary = other->snapshot();
   BOOST_TEST(secondary.accepted == 1U);
   BOOST_TEST(secondary.delivered == 1U);
   BOOST_TEST(secondary.dropped == 2U);
   BOOST_TEST(secondary.suppressed_reentrant == 2U);
   BOOST_TEST(sibling_output->records().size() == 3U);
   BOOST_TEST(other_output->timed_out() == 0U);
   BOOST_TEST(sibling_output->timed_out() == 0U);
}

BOOST_AUTO_TEST_CASE(removal_and_facade_destruction_do_not_join_or_release_active_downstream,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   auto route = forge::logger{"async.removal"};
   route.set_log_level(forge::log_level::info);
   route.add_sink(fixture.adapter);
   route.info("active before removal");
   fixture.require_blocked();
   route.info("queued before removal");
   const auto facade_lifetime = std::weak_ptr<forge::async_sink>{fixture.adapter};
   const auto downstream_lifetime = std::weak_ptr<forge::sink>{fixture.downstream};
   fixture.downstream.reset();
   auto producers = producer_group{fixture.output};
   producers.add([route, adapter = std::move(fixture.adapter)]() mutable {
      route.remove_sink(adapter);
      adapter.reset();
      route.info("not routed after removal");
   });
   producers.start();
   BOOST_REQUIRE(producers.wait());
   producers.join();
   BOOST_TEST(facade_lifetime.expired());
   BOOST_TEST(!downstream_lifetime.expired());
   BOOST_TEST(fixture.output->finished() == 0U);
   BOOST_REQUIRE(forge::asio::blocking::run_for(fixture.runtime, runtime_probe(), 1s));
   fixture.join_pool();
   BOOST_TEST(downstream_lifetime.expired());
   const auto records = fixture.output->records();
   BOOST_REQUIRE_EQUAL(records.size(), 2U);
   BOOST_TEST(records[0].message == "active before removal");
   BOOST_TEST(records[1].message == "queued before removal");
}

BOOST_AUTO_TEST_CASE(returned_waiters_retain_state_before_await_and_after_facade_destruction,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   fixture.adapter->log(test_record("active"));
   fixture.require_blocked();
   fixture.adapter->log(test_record("queued"));
   auto flush_operation = fixture.adapter->async_flush(2s);
   auto shutdown_operation = fixture.adapter->async_shutdown(2s);
   BOOST_TEST(fixture.adapter->snapshot().closed);
   const auto lifetime = std::weak_ptr<forge::sink>{fixture.downstream};
   fixture.downstream.reset();
   auto producers = producer_group{fixture.output};
   producers.add([adapter = std::move(fixture.adapter)]() mutable { adapter.reset(); });
   producers.start();
   BOOST_REQUIRE(producers.wait());
   producers.join();
   BOOST_TEST(!lifetime.expired());
   BOOST_TEST(fixture.output->finished() == 0U);

   auto flush = boost::asio::co_spawn(fixture.runtime.context(), std::move(flush_operation), boost::asio::use_future);
   auto shutdown = boost::asio::co_spawn(fixture.runtime.context(), std::move(shutdown_operation), boost::asio::use_future);
   BOOST_CHECK(flush.wait_for(20ms) == std::future_status::timeout);
   BOOST_CHECK(shutdown.wait_for(0ms) == std::future_status::timeout);
   fixture.output->release_all();
   BOOST_REQUIRE(require_result(flush));
   BOOST_REQUIRE(require_result(shutdown));
   fixture.join_pool();
   BOOST_TEST(lifetime.expired());
   BOOST_TEST(fixture.output->records().size() == 2U);
}

BOOST_AUTO_TEST_CASE(queued_records_own_payload_and_retain_redaction_and_producer_metadata,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   auto route = forge::logger{"async.metadata"};
   route.set_log_level(forge::log_level::info);
   route.add_sink(fixture.adapter);
   ilog(route, "blocking predecessor");
   fixture.require_blocked();
   const auto location = std::source_location::current();
   const auto timestamp = std::chrono::sys_time<std::chrono::microseconds>{123456us};
   {
      auto record = forge::log_record{
          .level = forge::log_level::off,
          .logger = "replaced by route",
          .component = "retained component",
          .message = "failure ${token}",
          .fields = {{"token", "private-async-fixture-value", true}, forge::log_ctx("public", 42)},
          .timestamp = timestamp,
          .thread_id = "producer-thread-id",
          .thread_name = "producer-thread-name",
          .location = location,
          .stacktrace = forge::stacktrace_snapshot{
              .backend = "fixture",
              .unavailable_reason = "retained reason",
              .frames = {{.index = 7, .address = 42, .name = "producer frame",
                          .source_file = "producer source", .source_line = 123}}},
          .exception_chain = "outer -> inner",
      };
      elog(route, record);
      record.message.assign(4096, 'x');
      record.component.clear();
      record.fields.clear();
      record.timestamp = {};
      record.thread_id.clear();
      record.thread_name.clear();
      record.location = {};
      record.stacktrace.reset();
      record.exception_chain.clear();
   }
   const auto producer_name = std::string{forge::get_thread_name()};
   auto producer_id = std::ostringstream{};
   producer_id << std::this_thread::get_id();
   const auto line = __LINE__ + 1;
   ilog(route, "macro source ${value}", ("value", 7));
   fixture.finish();
   const auto records = fixture.output->records();
   BOOST_REQUIRE_EQUAL(records.size(), 3U);
   const auto& retained = records[1];
   BOOST_TEST(retained.level.value == forge::log_level::error);
   BOOST_TEST(retained.logger == "async.metadata");
   BOOST_TEST(retained.component == "retained component");
   BOOST_TEST(retained.message == "failure <redacted>");
   BOOST_TEST(retained.timestamp.time_since_epoch().count() == timestamp.time_since_epoch().count());
   BOOST_TEST(retained.thread_id == "producer-thread-id");
   BOOST_TEST(retained.thread_name == "producer-thread-name");
   BOOST_TEST(retained.location.line() == location.line());
   BOOST_TEST(std::string{retained.location.file_name()} == location.file_name());
   BOOST_TEST(std::string{retained.location.function_name()} == location.function_name());
   BOOST_TEST(retained.exception_chain == "outer -> inner");
   BOOST_REQUIRE_EQUAL(retained.fields.size(), 2U);
   BOOST_TEST(retained.fields[0].key == "token");
   BOOST_TEST(retained.fields[0].value == "<redacted>");
   BOOST_TEST(retained.fields[0].redacted);
   BOOST_TEST(retained.fields[1].key == "public");
   BOOST_TEST(retained.fields[1].value == "42");
   BOOST_TEST(!retained.fields[1].redacted);
   BOOST_REQUIRE(retained.stacktrace.has_value());
   BOOST_TEST(retained.stacktrace->backend == "fixture");
   BOOST_TEST(retained.stacktrace->unavailable_reason == "retained reason");
   BOOST_REQUIRE_EQUAL(retained.stacktrace->frames.size(), 1U);
   const auto& frame = retained.stacktrace->frames[0];
   BOOST_TEST(frame.index == 7U);
   BOOST_TEST(frame.address == 42U);
   BOOST_TEST(frame.name == "producer frame");
   BOOST_TEST(frame.source_file == "producer source");
   BOOST_TEST(frame.source_line == 123U);
   BOOST_TEST(forge::format_json_log_record(retained).find("private-async-fixture-value") == std::string::npos);
   BOOST_TEST(records[2].location.line() == line);
   BOOST_TEST(std::string{records[2].location.file_name()} == __FILE__);
   BOOST_TEST(records[2].thread_id == producer_id.str());
   BOOST_TEST(records[2].thread_name == producer_name);
   BOOST_TEST(records[2].timestamp.time_since_epoch().count() > 0);
}

BOOST_AUTO_TEST_CASE(cancelling_one_flush_waiter_does_not_cancel_pump_or_other_waiter,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   fixture.adapter->log(test_record("survives waiter cancellation"));
   fixture.require_blocked();
   auto caller = boost::asio::make_strand(fixture.runtime.context());
   auto started = std::make_shared<test_signal>();
   auto cancellation = std::make_shared<boost::asio::cancellation_signal>();
   auto cancelled = boost::asio::co_spawn(
       caller, started_flush(fixture.adapter->async_flush(5s), started),
       boost::asio::bind_cancellation_slot(cancellation->slot(), boost::asio::use_future));
   auto surviving = boost::asio::co_spawn(
       caller, started_flush(fixture.adapter->async_flush(5s), started), boost::asio::use_future);
   BOOST_REQUIRE(started->wait_for(2));
   // The strand cannot run this handler until both flushes have yielded at
   // their real async waits. Cancellation is not timed against a sleep.
   boost::asio::post(caller, [cancellation] { cancellation->emit(boost::asio::cancellation_type::all); });
   // Shorter than both the flush deadline and the sink's emergency release:
   // readiness here must come from cancellation, not either timeout.
   BOOST_REQUIRE(cancelled.wait_for(1s) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(cancelled.get(), boost::system::system_error,
                         [](const boost::system::system_error& error) {
                            return error.code() == boost::asio::error::operation_aborted;
                         });
   BOOST_CHECK(surviving.wait_for(0ms) == std::future_status::timeout);
   const auto pending = fixture.adapter->snapshot();
   BOOST_TEST(pending.accepted == 1U);
   BOOST_TEST(pending.delivered == 0U);
   BOOST_TEST(pending.failed == 0U);
   BOOST_TEST(pending.dropped == 0U);
   BOOST_TEST(pending.queued_records == 1U);
   BOOST_TEST(!pending.closed);
   BOOST_TEST(fixture.output->finished() == 0U);
   BOOST_TEST(fixture.pool.snapshot().canceled == 0U);
   BOOST_TEST(fixture.pool.snapshot().running == 1U);
   fixture.adapter->log(test_record("accepted after cancellation"));
   BOOST_TEST(fixture.adapter->snapshot().accepted == 2U);
   fixture.output->release_all();
   BOOST_REQUIRE(require_result(surviving));
   BOOST_REQUIRE(fixture.wait(fixture.adapter->async_flush(2s)));
   fixture.finish();
   BOOST_TEST(fixture.adapter->snapshot().delivered == 2U);
   const auto records = fixture.output->records();
   BOOST_REQUIRE_EQUAL(records.size(), 2U);
   BOOST_TEST(records[0].message == "survives waiter cancellation");
   BOOST_TEST(records[1].message == "accepted after cancellation");
}

BOOST_AUTO_TEST_CASE(concurrent_32_flush_waiters_share_one_blocked_pump,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   fixture.adapter->log(test_record("one shared fence"));
   fixture.require_blocked();
   auto caller = boost::asio::make_strand(fixture.runtime.context());
   auto started = std::make_shared<test_signal>();
   auto waiters = std::vector<std::future<bool>>{};
   waiters.reserve(32);
   for (int waiter = 0; waiter < 32; ++waiter) {
      waiters.push_back(boost::asio::co_spawn(
          caller, started_flush(fixture.adapter->async_flush(5s), started), boost::asio::use_future));
   }
   BOOST_REQUIRE(started->wait_for(32));
   // This strand marker runs only after the last started flush has suspended.
   auto marker = boost::asio::co_spawn(caller, runtime_probe(), boost::asio::use_future);
   require_result(marker);
   for (auto& waiter : waiters) {
      BOOST_CHECK(waiter.wait_for(0ms) == std::future_status::timeout);
   }
   BOOST_TEST(fixture.output->finished() == 0U);
   BOOST_TEST(fixture.adapter->snapshot().queued_records == 1U);
   BOOST_TEST(fixture.pool.snapshot().submitted == 1U);
   BOOST_TEST(fixture.pool.snapshot().running == 1U);
   fixture.output->release_all();
   for (auto& waiter : waiters) {
      BOOST_REQUIRE(require_result(waiter));
   }
   fixture.finish();
   BOOST_TEST(fixture.adapter->snapshot().accepted == 1U);
   BOOST_TEST(fixture.adapter->snapshot().delivered == 1U);
   BOOST_TEST(fixture.output->records().size() == 1U);
   BOOST_TEST(fixture.output->maximum_active() == 1U);
}

BOOST_AUTO_TEST_CASE(stopped_compute_executor_drops_admission_without_stranded_records,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   fixture.pool.request_stop();
   for (int attempt = 1; attempt <= 2; ++attempt) {
      fixture.adapter->log(test_record("rejected by stopped executor"));
      const auto snapshot = fixture.adapter->snapshot();
      BOOST_TEST(snapshot.accepted == 0U);
      BOOST_TEST(snapshot.delivered == 0U);
      BOOST_TEST(snapshot.failed == 0U);
      BOOST_TEST(snapshot.dropped == static_cast<unsigned>(attempt));
      BOOST_TEST(snapshot.queued_records == 0U);
      BOOST_TEST(snapshot.queued_bytes == 0U);
      BOOST_TEST(!snapshot.closed);
      BOOST_REQUIRE(fixture.wait(fixture.adapter->async_flush(0ms)));
   }
   BOOST_TEST(fixture.pool.snapshot().submitted == 0U);
   BOOST_TEST(fixture.pool.snapshot().rejected == 2U);
   BOOST_TEST(fixture.output->records().empty());
   BOOST_REQUIRE(fixture.wait(fixture.adapter->async_shutdown(0ms)));
   fixture.finish();
}

BOOST_AUTO_TEST_CASE(full_compute_executor_drops_admission_without_acceptance_fence_entries,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{{}, {}, {.worker_threads = 1, .max_pending_tasks = 0,
                                        .thread_name = "log-test-refusal"}};
   // Occupy actual compute capacity with the same bounded test callback. The
   // fixture releases this callback before its pool destructor on every path.
   auto occupying = fixture.pool.get_executor().try_submit(
       {.name = "capacity blocker"}, [downstream = fixture.downstream] {
          downstream->log(test_record("occupied compute worker"));
       });
   BOOST_REQUIRE(occupying.has_value());
   fixture.require_blocked();
   for (int attempt = 1; attempt <= 2; ++attempt) {
      fixture.adapter->log(test_record("rejected by full executor"));
      const auto snapshot = fixture.adapter->snapshot();
      BOOST_TEST(snapshot.accepted == 0U);
      BOOST_TEST(snapshot.delivered == 0U);
      BOOST_TEST(snapshot.failed == 0U);
      BOOST_TEST(snapshot.dropped == static_cast<unsigned>(attempt));
      BOOST_TEST(snapshot.queued_records == 0U);
      BOOST_TEST(snapshot.queued_bytes == 0U);
      BOOST_REQUIRE(fixture.wait(fixture.adapter->async_flush(0ms)));
   }
   BOOST_TEST(fixture.pool.snapshot().submitted == 1U);
   BOOST_TEST(fixture.pool.snapshot().rejected == 2U);
   BOOST_TEST(fixture.pool.snapshot().running == 1U);
   // The external job is not an accepted logging record. Closing an empty
   // adapter must succeed without stopping or waiting for its external owner.
   BOOST_REQUIRE(fixture.wait(fixture.adapter->async_shutdown(0ms)));
   BOOST_TEST(fixture.output->finished() == 0U);
   fixture.output->release_all();
   auto occupied_done = boost::asio::co_spawn(
       fixture.runtime.context(), std::move(*occupying).wait(), boost::asio::use_future);
   require_result(occupied_done);
   fixture.finish();
   BOOST_TEST(fixture.adapter->snapshot().delivered == 0U);
   const auto records = fixture.output->records();
   BOOST_REQUIRE_EQUAL(records.size(), 1U);
   BOOST_TEST(records[0].message == "occupied compute worker");
}

BOOST_AUTO_TEST_CASE(invalid_options_and_dependencies_throw_typed_invalid_config,
                     * boost::unit_test::timeout(20)) {
   auto fixture = async_fixture{};
   const auto executor = fixture.pool.get_executor();
   BOOST_CHECK_THROW(forge::async_sink(nullptr, executor), forge::log::exceptions::invalid_config);
   BOOST_CHECK_THROW(forge::async_sink(fixture.downstream, forge::asio::compute::executor{}),
                     forge::log::exceptions::invalid_config);
   BOOST_CHECK_THROW(forge::async_sink(fixture.downstream, executor, forge::async_sink_options{.max_records = 0}),
                     forge::log::exceptions::invalid_config);
   BOOST_CHECK_THROW(forge::async_sink(fixture.downstream, executor, forge::async_sink_options{.max_bytes = 0}),
                     forge::log::exceptions::invalid_config);
   const auto defaults = forge::async_sink_options{};
   BOOST_TEST(defaults.max_records == 8192U);
   BOOST_TEST(defaults.max_bytes == 8U * 1024U * 1024U);
   BOOST_TEST(fixture.pool.snapshot().submitted == 0U);
   BOOST_TEST(fixture.output->records().empty());
   fixture.finish();
}

BOOST_AUTO_TEST_SUITE_END()
