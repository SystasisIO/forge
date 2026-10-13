#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

import forge.app.diagnostics;
import forge.app.events;
import forge.app.plugin_context;
import forge.app.plugin_lifecycle;
import forge.asio.notification;
import forge.asio.runtime;

namespace {

using forge::app::lifecycle_operation;
using forge::app::lifecycle_phase;
using forge::app::plugin_lifecycle;

constexpr auto operation_timeout = std::chrono::seconds{2};
constexpr auto test_timeout = std::chrono::seconds{5};

template <typename T>
concept can_stop = requires(T& value) { value.request_stop(); };

template <typename T>
concept can_begin = requires(T& value) { value.begin(lifecycle_operation::configure); };

template <typename T>
concept can_clear_diagnostics = requires(T& value) { value.diagnostics()->clear_plugins(); };

template <typename T>
concept can_bind_diagnostics = requires(T& value, forge::app::lifecycle_observer observer) {
   value.diagnostics()->bind_application(observer);
   value.diagnostics()->bind_plugin("probe", "1", observer);
};

static_assert(!can_stop<forge::app::lifecycle_observer>);
static_assert(!can_begin<forge::app::lifecycle_observer>);
static_assert(!can_clear_diagnostics<forge::app::plugin_context>);
static_assert(!can_bind_diagnostics<forge::app::plugin_context>);
static_assert(std::is_same_v<decltype(std::declval<const forge::app::plugin_context&>().diagnostics()),
                             const forge::app::diagnostics_store*>);

template <typename T>
T ready(std::future<T>& result) {
   BOOST_REQUIRE(result.wait_until(std::chrono::steady_clock::now() + test_timeout) == std::future_status::ready);
   return result.get();
}

void ready(std::future<void>& result) {
   BOOST_REQUIRE(result.wait_until(std::chrono::steady_clock::now() + test_timeout) == std::future_status::ready);
   result.get();
}

plugin_lifecycle::operation begin(forge::asio::runtime& runtime, const plugin_lifecycle& lifecycle,
                                 lifecycle_operation value) {
   auto result = boost::asio::co_spawn(runtime.context(), lifecycle.begin(value), boost::asio::use_future);
   return ready(result);
}

void finish(forge::asio::runtime& runtime, const plugin_lifecycle& lifecycle, lifecycle_operation value) {
   auto operation = begin(runtime, lifecycle, value);
   BOOST_REQUIRE(static_cast<bool>(operation));
   BOOST_REQUIRE(!operation.complete());
}

boost::asio::awaitable<void> executor_barrier() {
   co_return;
}

void check_projection(const forge::app::lifecycle_snapshot& actual,
                      const forge::app::lifecycle_snapshot& canonical) {
   BOOST_CHECK(actual.phase == canonical.phase);
   BOOST_CHECK_EQUAL(actual.epoch, canonical.epoch);
   BOOST_CHECK_EQUAL(actual.generation, canonical.generation);
   BOOST_CHECK_EQUAL(actual.stop_requested, canonical.stop_requested);
   BOOST_CHECK_EQUAL(actual.admission_open, canonical.admission_open);
   BOOST_CHECK_EQUAL(actual.owner_alive, canonical.owner_alive);
   BOOST_CHECK_EQUAL(actual.completed_operations, canonical.completed_operations);
   BOOST_CHECK(actual.in_progress == canonical.in_progress);
   BOOST_REQUIRE(actual.failure.has_value() == canonical.failure.has_value());
   if (canonical.failure) {
      BOOST_CHECK(actual.failure->operation == canonical.failure->operation);
      BOOST_CHECK_EQUAL(actual.failure->attempt, canonical.failure->attempt);
      BOOST_CHECK(actual.failure->exception == canonical.failure->exception);
   }
}

bool bounded_records(const std::vector<forge::app::event_record>& records, std::uint64_t count_limit,
                     std::uint64_t byte_limit, bool critical_only) {
   if (records.size() > count_limit) {
      return false;
   }
   auto previous = std::uint64_t{};
   for (const auto& record : records) {
      if (record.id <= previous || record.topic.size() + record.message.size() > byte_limit ||
          (critical_only && record.severity != forge::app::event_severity::critical)) {
         return false;
      }
      previous = record.id;
   }
   return true;
}

struct journal_state {
   forge::app::event_journal_options limits{.max_critical_events = 3, .max_recent_events = 7, .max_record_bytes = 32};
   forge::app::event_journal journal{limits};
   forge::app::diagnostics_store diagnostics;
   forge::asio::notification start;
   forge::asio::notification progress;
   forge::asio::notification inspected;
   std::atomic_uint ready_workers = 0;
   std::atomic_uint first_batches = 0;
   std::atomic_uint finished_writers = 0;
};

constexpr auto batches_per_writer = 64U;

boost::asio::awaitable<void> wait_count(std::shared_ptr<journal_state> state, bool first_batches,
                                      unsigned expected, std::chrono::steady_clock::time_point deadline) {
   for (;;) {
      const auto epoch = state->progress.epoch();
      const auto count = first_batches ? state->first_batches.load() : state->ready_workers.load();
      if (count >= expected) {
         co_return;
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         throw std::runtime_error{"diagnostics test rendezvous deadline expired"};
      }
      co_await state->progress.async_wait_until(epoch, deadline);
   }
}

boost::asio::awaitable<void> wait_start(std::shared_ptr<journal_state> state,
                                      std::chrono::steady_clock::time_point deadline) {
   const auto epoch = state->start.epoch();
   ++state->ready_workers;
   state->progress.notify();
   co_await state->start.async_wait_until(epoch, deadline);
}

void publish_batch(journal_state& state, unsigned writer, unsigned batch) {
   const auto severity = batch % 2 == 0 ? forge::app::event_severity::critical : forge::app::event_severity::info;
   state.journal.publish(severity, "writer-" + std::to_string(writer), "item-" + std::to_string(batch));
   state.journal.publish(severity, std::string(state.limits.max_record_bytes + 1, 'x'), "");
   state.journal.publish(severity, "oversize", std::string(state.limits.max_record_bytes, 'x'));
}

boost::asio::awaitable<void> publish_records(std::shared_ptr<journal_state> state, unsigned writer,
                                           std::chrono::steady_clock::time_point deadline) {
   co_await wait_start(state, deadline);
   const auto inspected_epoch = state->inspected.epoch();
   publish_batch(*state, writer, 0);
   ++state->first_batches;
   state->progress.notify();
   co_await state->inspected.async_wait_until(inspected_epoch, deadline);
   for (auto batch = 1U; batch < batches_per_writer; ++batch) {
      co_await boost::asio::post(boost::asio::use_awaitable);
      publish_batch(*state, writer, batch);
   }
   ++state->finished_writers;
}

void require_bounded_snapshot(const journal_state& state, const forge::app::application_diagnostics_snapshot& snapshot) {
   if (!bounded_records(snapshot.recent_events, state.limits.max_recent_events, state.limits.max_record_bytes, false) ||
       !bounded_records(snapshot.critical_events, state.limits.max_critical_events, state.limits.max_record_bytes, true)) {
      throw std::runtime_error{"diagnostics journal snapshot exceeded its record bounds"};
   }
}

boost::asio::awaitable<unsigned> inspect_records(std::shared_ptr<journal_state> state,
                                               std::chrono::steady_clock::time_point deadline) {
   co_await wait_start(state, deadline);
   co_await wait_count(state, true, 2, deadline);
   const auto first = state->diagnostics.snapshot(state->journal);
   require_bounded_snapshot(*state, first);
   if (first.events.published != 6 || first.events.dropped != 4 || state->finished_writers.load() != 0) {
      throw std::runtime_error{"diagnostics snapshot did not overlap both active publishers"};
   }
   state->inspected.notify();
   for (auto index = 0U; index < 128; ++index) {
      co_await boost::asio::post(boost::asio::use_awaitable);
      require_bounded_snapshot(*state, state->diagnostics.snapshot(state->journal));
   }
   co_return 129;
}

struct retirement_state {
   forge::asio::runtime* runtime = nullptr;
   std::shared_ptr<forge::app::diagnostics_store> diagnostics = std::make_shared<forge::app::diagnostics_store>();
   std::shared_ptr<forge::app::event_journal> journal = std::make_shared<forge::app::event_journal>();
   std::atomic_uint destructions = 0;
   std::atomic_bool reentered = false;
   std::atomic_bool expired = false;
   std::atomic_bool failed = false;
   std::atomic_size_t plugin_count = 0;
   std::atomic<lifecycle_phase> application_phase = lifecycle_phase::stopped;
};

boost::asio::awaitable<std::pair<std::size_t, lifecycle_phase>> reenter_snapshot(
    std::shared_ptr<retirement_state> state) {
   const auto snapshot = state->diagnostics->snapshot(*state->journal);
   co_return std::pair{snapshot.plugins.size(), snapshot.lifecycle.phase};
}

struct retired_exception_probe {
   explicit retired_exception_probe(std::shared_ptr<retirement_state> value) : state{std::move(value)} {}

   ~retired_exception_probe() noexcept {
      try {
         // Delegated re-entry keeps a lock-retirement regression finite: timeout
         // releases the retiring caller's lock so the snapshot worker can finish.
         auto result = boost::asio::co_spawn(state->runtime->context(), reenter_snapshot(state), boost::asio::use_future);
         if (result.wait_until(std::chrono::steady_clock::now() + operation_timeout) == std::future_status::ready) {
            const auto snapshot = result.get();
            state->plugin_count.store(snapshot.first);
            state->application_phase.store(snapshot.second);
            state->reentered.store(true);
         } else {
            state->expired.store(true);
         }
      } catch (...) {
         state->failed.store(true);
      }
      ++state->destructions;
   }

   std::shared_ptr<retirement_state> state;
};

class reentrant_failure final : public std::exception {
 public:
   explicit reentrant_failure(std::shared_ptr<retired_exception_probe> value) : probe_{std::move(value)} {}

   const char* what() const noexcept override {
      return "diagnostics retirement probe";
   }

 private:
   std::shared_ptr<retired_exception_probe> probe_;
};

enum class retirement_kind { clear_plugins, rebind_plugin, rebind_application };

void check_retirement(retirement_kind kind) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   const auto state = std::make_shared<retirement_state>();
   state->runtime = &runtime;
   {
      auto lifecycle = plugin_lifecycle{};
      auto operation = begin(runtime, lifecycle, lifecycle_operation::shutdown);
      const auto probe = std::make_shared<retired_exception_probe>(state);
      const auto error = std::make_exception_ptr(reentrant_failure{probe});
      BOOST_REQUIRE(operation.complete(error) == error);
      if (kind == retirement_kind::rebind_application) {
         state->diagnostics->bind_application(lifecycle.observer());
      } else {
         state->diagnostics->bind_plugin("probe", "old", lifecycle.observer());
      }
      const auto bound = state->diagnostics->snapshot(*state->journal);
      if (kind == retirement_kind::rebind_application) {
         BOOST_REQUIRE(bound.lifecycle.failure);
         BOOST_CHECK(bound.lifecycle.failure->exception == error);
      } else {
         BOOST_REQUIRE_EQUAL(bound.plugins.size(), 1U);
         BOOST_REQUIRE(bound.plugins.front().lifecycle.failure);
         BOOST_CHECK(bound.plugins.front().lifecycle.failure->exception == error);
      }
   }
   // A single-worker barrier releases the completed begin coroutine's owners
   // before testing that the diagnostics binding is the last failed owner.
   auto barrier = boost::asio::co_spawn(runtime.context(), executor_barrier(), boost::asio::use_future);
   ready(barrier);
   BOOST_REQUIRE_EQUAL(state->destructions.load(), 0U);
   // Only the diagnostics binding now retains the failed lifecycle and error.
   const auto replacement = plugin_lifecycle{};
   switch (kind) {
   case retirement_kind::clear_plugins:
      state->diagnostics->clear_plugins();
      break;
   case retirement_kind::rebind_plugin:
      state->diagnostics->bind_plugin("probe", "new", replacement.observer());
      break;
   case retirement_kind::rebind_application:
      state->diagnostics->bind_application(replacement.observer());
      break;
   }
   BOOST_REQUIRE_EQUAL(state->destructions.load(), 1U);
   BOOST_CHECK(!state->expired.load());
   BOOST_CHECK(!state->failed.load());
   BOOST_CHECK(state->reentered.load());
   BOOST_CHECK(state->application_phase.load() == lifecycle_phase::created);
   BOOST_CHECK_EQUAL(state->plugin_count.load(), kind == retirement_kind::rebind_plugin ? 1U : 0U);
   const auto current = state->diagnostics->snapshot(*state->journal);
   if (kind == retirement_kind::rebind_plugin) {
      BOOST_REQUIRE_EQUAL(current.plugins.size(), 1U);
      BOOST_CHECK_EQUAL(current.plugins.front().id, "probe");
      BOOST_CHECK_EQUAL(current.plugins.front().version, "new");
      check_projection(current.plugins.front().lifecycle, replacement.snapshot());
   } else if (kind == retirement_kind::rebind_application) {
      check_projection(current.lifecycle, replacement.snapshot());
   } else {
      BOOST_CHECK(current.plugins.empty());
   }
   runtime.stop();
}

} // namespace

BOOST_AUTO_TEST_SUITE(diagnostics_tests)

BOOST_AUTO_TEST_CASE(diagnostics_projects_canonical_lifecycle_without_consumer_mutation_authority) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 2}};
   auto application = plugin_lifecycle{};
   auto plugin = plugin_lifecycle{};
   auto diagnostics = forge::app::diagnostics_store{};
   const auto& reader = diagnostics;
   const auto journal = forge::app::event_journal{};
   diagnostics.bind_application(application.observer());
   diagnostics.bind_plugin("projection", "1", plugin.observer());
   auto before = reader.snapshot(journal);
   BOOST_REQUIRE_EQUAL(before.plugins.size(), 1U);
   check_projection(before.lifecycle, application.snapshot());
   check_projection(before.plugins.front().lifecycle, plugin.snapshot());
   before.lifecycle.phase = lifecycle_phase::stopped;
   before.plugins.front().lifecycle.stop_requested = true;
   BOOST_CHECK(application.snapshot().phase == lifecycle_phase::created);
   BOOST_CHECK(!plugin.snapshot().stop_requested);

   const auto epoch = application.snapshot().epoch;
   auto changed = boost::asio::co_spawn(
       runtime.context(), application.observer().async_wait_until(
           epoch, std::chrono::steady_clock::now() + operation_timeout), boost::asio::use_future);
   finish(runtime, application, lifecycle_operation::configure);
   BOOST_CHECK(ready(changed).epoch != epoch);
   finish(runtime, plugin, lifecycle_operation::configure);
   auto initializing = begin(runtime, plugin, lifecycle_operation::initialize);
   const auto in_progress = reader.snapshot(journal);
   BOOST_REQUIRE_EQUAL(in_progress.plugins.size(), 1U);
   check_projection(in_progress.lifecycle, application.snapshot());
   check_projection(in_progress.plugins.front().lifecycle, plugin.snapshot());
   BOOST_CHECK(in_progress.plugins.front().lifecycle.in_progress == lifecycle_operation::initialize);
   const auto error = std::make_exception_ptr(std::runtime_error{"canonical initialization failure"});
   BOOST_REQUIRE(initializing.complete(error) == error);
   const auto failed = reader.snapshot(journal);
   BOOST_REQUIRE_EQUAL(failed.plugins.size(), 1U);
   BOOST_REQUIRE(failed.plugins.front().lifecycle.failure);
   check_projection(failed.lifecycle, application.snapshot());
   check_projection(failed.plugins.front().lifecycle, plugin.snapshot());
   BOOST_CHECK(failed.plugins.front().lifecycle.failure->exception == error);
   const auto observed_epoch = plugin.snapshot().epoch;
   static_cast<void>(reader.snapshot(journal));
   BOOST_CHECK_EQUAL(plugin.snapshot().epoch, observed_epoch);
}

BOOST_AUTO_TEST_CASE(diagnostics_concurrent_publish_and_snapshot_preserve_journal_bounds) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   const auto state = std::make_shared<journal_state>();
   const auto deadline = std::chrono::steady_clock::now() + operation_timeout;
   auto first = boost::asio::co_spawn(runtime.context(), publish_records(state, 1, deadline), boost::asio::use_future);
   auto second = boost::asio::co_spawn(runtime.context(), publish_records(state, 2, deadline), boost::asio::use_future);
   auto reader = boost::asio::co_spawn(runtime.context(), inspect_records(state, deadline), boost::asio::use_future);
   auto waiting = boost::asio::co_spawn(runtime.context(), wait_count(state, false, 3, deadline), boost::asio::use_future);
   ready(waiting);
   state->start.notify();
   ready(first);
   ready(second);
   BOOST_CHECK_EQUAL(ready(reader), 129U);
   BOOST_CHECK_EQUAL(state->finished_writers.load(), 2U);
   const auto final = state->diagnostics.snapshot(state->journal);
   require_bounded_snapshot(*state, final);
   BOOST_CHECK_EQUAL(final.events.published, 2U * batches_per_writer * 3U);
   BOOST_CHECK_EQUAL(final.events.dropped, 2U * batches_per_writer * 2U);
   BOOST_CHECK_EQUAL(final.recent_events.size(), state->limits.max_recent_events);
   BOOST_CHECK_EQUAL(final.critical_events.size(), state->limits.max_critical_events);
}

BOOST_AUTO_TEST_CASE(diagnostics_clear_plugins_retires_failure_destructor_outside_lock) {
   check_retirement(retirement_kind::clear_plugins);
}

BOOST_AUTO_TEST_CASE(diagnostics_rebind_plugin_retires_failure_destructor_outside_lock) {
   check_retirement(retirement_kind::rebind_plugin);
}

BOOST_AUTO_TEST_CASE(diagnostics_rebind_application_retires_failure_destructor_outside_lock) {
   check_retirement(retirement_kind::rebind_application);
}

BOOST_AUTO_TEST_SUITE_END()
