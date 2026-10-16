// Replacement allocation symbols are isolated to this executable. The injector
// follows quic_p2p/control/pubsub_control_allocation and is submit-thread only.
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <vector>

#define BOOST_TEST_MODULE asio_task_admission
#include <boost/test/included/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/use_future.hpp>

import forge.asio.runtime;
import forge.asio.task;
import forge.asio.compute;
import forge.asio.blocking;

namespace {

thread_local bool allocation_armed = false;
thread_local std::size_t allocation_ordinal = 0;
thread_local std::size_t allocation_calls = 0;
thread_local unsigned allocation_kind = 0; // 1=new, 2=the actual Boost aligned allocation.
thread_local const std::atomic_bool* allocation_work_started = nullptr;

class allocation_failure {
 public:
   explicit allocation_failure(std::size_t ordinal, const std::atomic_bool* work_started = nullptr) noexcept {
      if (allocation_armed) {
         std::terminate();
      }
      allocation_calls = 0;
      allocation_kind = 0;
      allocation_ordinal = ordinal;
      allocation_work_started = work_started;
      allocation_armed = true;
   }
   ~allocation_failure() {
      allocation_armed = false;
      allocation_work_started = nullptr;
   }
   allocation_failure(const allocation_failure&) = delete;
   allocation_failure& operator=(const allocation_failure&) = delete;

   std::size_t calls() const noexcept {
      return allocation_calls;
   }
   unsigned kind() const noexcept {
      return allocation_kind;
   }
};

bool should_fail(unsigned kind) noexcept {
   if (allocation_armed &&
       (allocation_work_started == nullptr || !allocation_work_started->load(std::memory_order_acquire))) {
      ++allocation_calls;
      allocation_kind = kind;
      if (allocation_calls == allocation_ordinal) {
         allocation_armed = false; // Never fail rollback, assertions, or IO threads.
         return true;
      }
   }
   return false;
}

void* allocate(std::size_t size, std::size_t alignment = 0) {
   if (should_fail(1)) {
      throw std::bad_alloc{};
   }
   void* result = nullptr;
   if (alignment == 0) {
      result = std::malloc(size == 0 ? 1 : size);
   } else if (posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0) {
      result = nullptr;
   }
   if (!result) {
      throw std::bad_alloc{};
   }
   return result;
}

using scheduler_type = forge::asio::task::scheduler;
using handle = forge::asio::task::handle;
using task = forge::asio::task::task;
using awaitable_task = forge::asio::task::awaitable;

struct io_gate {
   std::mutex mutex;
   std::condition_variable changed;
   bool entered = false;
   bool released = false;
   bool timed_out = false;

   void hold() {
      auto lock = std::unique_lock{mutex};
      entered = true;
      changed.notify_all();
      timed_out = !changed.wait_for(lock, std::chrono::seconds{3}, [this] { return released; });
   }

   bool wait_for_entry() {
      auto lock = std::unique_lock{mutex};
      return changed.wait_for(lock, std::chrono::seconds{2}, [this] { return entered; });
   }

   void release() {
      const auto lock = std::scoped_lock{mutex};
      released = true;
      changed.notify_all();
   }

   ~io_gate() {
      release();
   }
};

struct gate_release {
   io_gate& gate;
   ~gate_release() {
      gate.release();
   }
};

void wait_task(forge::asio::runtime& runtime, const handle& value) {
   auto completion = boost::asio::co_spawn(runtime.context(), value.wait(), boost::asio::use_future);
   BOOST_REQUIRE(completion.wait_for(std::chrono::seconds{2}) == std::future_status::ready);
   completion.get();
}

struct sentinel {
   scheduler_type* owner;
   std::atomic_size_t* destroyed;

   ~sentinel() {
      static_cast<void>(owner->snapshot()); // Deadlocks if final release holds the owner mutex.
      destroyed->fetch_add(1, std::memory_order_release);
   }
};

struct outcome {
   std::size_t calls = 0;
   bool threw = false;
   unsigned failed_kind = 0;
};

outcome exercise(std::size_t ordinal, bool delayed, bool awaitable, bool warm) {
   // Captured state outlives scheduler teardown and the worker join on every
   // assertion/exception path. Release precedes either owner destructor.
   auto gate = io_gate{};
   auto runs = std::atomic_size_t{0};
   auto destroyed = std::atomic_size_t{0};
   auto healthy_runs = std::atomic_size_t{0};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{
       runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_awaitable_tasks = 1, .max_pending_tasks = 16}};
   const auto release = gate_release{gate};
   boost::asio::post(runtime.context(), [&] { gate.hold(); });
   BOOST_REQUIRE(gate.wait_for_entry());

   auto seed = handle{};
   if (warm) {
      // An existing posted drain also distinguishes a cold first-strand post.
      seed = scheduler.submit(task{.work = [] {}});
      BOOST_REQUIRE(seed.accepted());
   }
   const auto before = scheduler.snapshot();
   auto captured = std::make_shared<sentinel>(&scheduler, &destroyed);
   auto blocking = task{.work = [captured, &runs] { runs.fetch_add(1, std::memory_order_release); }};
   auto asynchronous =
       awaitable_task{.work = [captured, &runs](forge::asio::task::context&) -> boost::asio::awaitable<void> {
          runs.fetch_add(1, std::memory_order_release);
          co_return;
       }};
   captured.reset();
   // Only the candidate owns the sentinel when submit begins.
   if (awaitable) {
      blocking = {};
   } else {
      asynchronous = {};
   }

   auto candidate = handle{};
   auto result = outcome{};
   {
      const auto failure = allocation_failure{ordinal};
      try {
         if (awaitable) {
            candidate = delayed ? scheduler.submit_after(std::move(asynchronous), std::chrono::milliseconds{10})
                                : scheduler.submit(std::move(asynchronous));
         } else {
            candidate = delayed ? scheduler.submit_after(std::move(blocking), std::chrono::milliseconds{10})
                                : scheduler.submit(std::move(blocking));
         }
      } catch (const std::bad_alloc&) {
         result.threw = true;
      }
      result.calls = failure.calls();
      if (result.threw) {
         result.failed_kind = failure.kind();
      }
   }
   // SBO moved-from targets may retain copies, but their destruction is outside
   // the scheduler lock and outside the injection window.
   blocking = {};
   asynchronous = {};
   const auto after = scheduler.snapshot();
   if (result.threw) {
      BOOST_REQUIRE(!candidate.valid());
      BOOST_REQUIRE_EQUAL(after.submitted, before.submitted);
      BOOST_REQUIRE_EQUAL(after.pending, before.pending);
      BOOST_REQUIRE_EQUAL(after.started, before.started);
      BOOST_REQUIRE_EQUAL(destroyed.load(std::memory_order_acquire), 1U);
   } else {
      BOOST_REQUIRE(candidate.accepted());
      BOOST_REQUIRE_EQUAL(after.submitted, before.submitted + 1);
      BOOST_REQUIRE_EQUAL(after.pending, before.pending + 1);
   }

   auto healthy = scheduler.submit(task{.work = [&] { healthy_runs.fetch_add(1, std::memory_order_release); }});
   BOOST_REQUIRE(healthy.accepted());
   gate.release();
   wait_task(runtime, healthy);
   if (seed.valid()) {
      wait_task(runtime, seed);
   }
   if (candidate.valid()) {
      wait_task(runtime, candidate);
   }
   BOOST_REQUIRE_EQUAL(healthy_runs.load(std::memory_order_acquire), 1U);
   BOOST_REQUIRE_EQUAL(runs.load(std::memory_order_acquire), result.threw ? 0U : 1U);
   scheduler.stop();
   runtime.stop();
   BOOST_REQUIRE(!gate.timed_out);
   BOOST_REQUIRE_EQUAL(destroyed.load(std::memory_order_acquire), 1U);
   return result;
}

void sweep(bool delayed, bool awaitable, bool warm) {
   const auto baseline = exercise(0, delayed, awaitable, warm);
   BOOST_REQUIRE(!baseline.threw);
   BOOST_REQUIRE_GT(baseline.calls, 2U);
   BOOST_REQUIRE_LT(baseline.calls, 64U);
   auto failures = std::size_t{0};
   auto post_failures = std::size_t{0};
   for (auto ordinal = std::size_t{1}; ordinal <= baseline.calls; ++ordinal) {
      const auto observed = exercise(ordinal, delayed, awaitable, warm);
      BOOST_TEST_MESSAGE("admission delayed=" << delayed << " awaitable=" << awaitable << " warm=" << warm
                                              << " ordinal=" << ordinal << " calls=" << observed.calls
                                              << " threw=" << observed.threw << " kind=" << observed.failed_kind);
      failures += observed.threw ? 1 : 0;
      post_failures += observed.threw && observed.failed_kind == 2 ? 1 : 0;
   }
   BOOST_REQUIRE_GT(failures, 2U);
   BOOST_REQUIRE_GT(post_failures, 0U); // Do not silently omit Boost's real post allocation.
}

struct small_target_state {
   scheduler_type* owner = nullptr;
   std::atomic_bool armed = false;
   std::atomic_size_t destructors = 0;
   std::atomic_size_t runs = 0;
};

struct small_target {
   small_target_state* state;
   small_target(const small_target&) noexcept = default;
   explicit small_target(small_target_state& value) noexcept : state{&value} {}
   ~small_target() {
      if (state->armed.load(std::memory_order_acquire)) {
         static_cast<void>(state->owner->snapshot());
         state->destructors.fetch_add(1, std::memory_order_release);
      }
   }
   void operator()() const {
      state->runs.fetch_add(1, std::memory_order_release);
   }
};

void reentrant_cleanup(bool delayed, bool cancel_one) {
   auto state = small_target_state{};
   auto gate = io_gate{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto scheduler = scheduler_type{runtime, scheduler_type::options{.max_blocking_tasks = 1, .max_pending_tasks = 32}};
   state.owner = &scheduler;
   const auto release = gate_release{gate};
   boost::asio::post(runtime.context(), [&] { gate.hold(); });
   BOOST_REQUIRE(gate.wait_for_entry());
   state.armed.store(true, std::memory_order_release);
   auto handles = std::vector<handle>{};
   for (auto index = 0U; index < 12U; ++index) {
      auto value = task{.priority = forge::asio::task::priority{static_cast<int>(index)}, .work = small_target{state}};
      handles.push_back(delayed ? scheduler.submit_after(std::move(value), std::chrono::seconds{30})
                                : scheduler.submit(std::move(value)));
      BOOST_REQUIRE(handles.back().accepted());
   }
   if (cancel_one) {
      BOOST_REQUIRE(handles[5].cancel());
      BOOST_REQUIRE_EQUAL(scheduler.pending_count(), 11U);
   }
   scheduler.request_stop();
   BOOST_REQUIRE_EQUAL(scheduler.pending_count(), 0U);
   gate.release();
   scheduler.stop();
   runtime.stop();
   BOOST_REQUIRE(!gate.timed_out);
   BOOST_REQUIRE_EQUAL(state.runs.load(std::memory_order_acquire), 0U);
   BOOST_REQUIRE_GT(state.destructors.load(std::memory_order_acquire), 12U);
   state.armed.store(false, std::memory_order_release);
}

void concurrent_ready_delayed_stop() {
   auto gate = io_gate{};
   auto runs = std::array<std::atomic_size_t, 32>{};
   auto accepted = std::array<bool, 32>{};
   auto posted_first = std::atomic_bool{false};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto scheduler = scheduler_type{
       runtime, scheduler_type::options{.max_blocking_tasks = 2, .max_awaitable_tasks = 2, .max_pending_tasks = 64}};
   const auto release = gate_release{gate};
   auto blocker = scheduler.submit(task{.work = [&] { gate.hold(); }});
   BOOST_REQUIRE(blocker.accepted());
   BOOST_REQUIRE(gate.wait_for_entry());
   auto delayed = scheduler.submit_after(task{.work = [] {}}, std::chrono::seconds{30});
   BOOST_REQUIRE(delayed.accepted());
   // Completing this ready task proves drain armed the live delayed timer
   // before returning its task batch to the context.
   auto armed_fence = scheduler.submit(task{.work = [] {}});
   wait_task(runtime, armed_fence);

   auto producer = std::jthread{[&] {
      for (auto index = std::size_t{0}; index < runs.size(); ++index) {
         auto value = task{.work = [&, index] { runs[index].fetch_add(1, std::memory_order_release); }};
         auto submitted = index % 2 == 0 ? scheduler.submit(std::move(value))
                                         : scheduler.submit_after(std::move(value), std::chrono::milliseconds{1});
         accepted[index] = submitted.accepted();
         if (index == 0) {
            posted_first.store(true, std::memory_order_release);
         }
      }
   }};
   const auto until = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!posted_first.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < until) {
      std::this_thread::yield();
   }
   // Join before assertions or captured storage teardown, even if entry timed out.
   scheduler.request_stop();
   producer.join();
   gate.release();
   scheduler.stop();
   runtime.stop();
   BOOST_REQUIRE(posted_first.load(std::memory_order_acquire));
   BOOST_REQUIRE(!gate.timed_out);
   BOOST_REQUIRE(scheduler.snapshot().stopped);
   BOOST_REQUIRE_EQUAL(scheduler.pending_count(), 0U);
   for (auto index = std::size_t{0}; index < runs.size(); ++index) {
      const auto count = runs[index].load(std::memory_order_acquire);
      BOOST_REQUIRE_LE(count, 1U);
      if (!accepted[index]) {
         BOOST_REQUIRE_EQUAL(count, 0U);
      }
   }
}

struct compute_probe {
   forge::asio::compute::pool* owner;
   std::atomic_size_t* destroyed;
   ~compute_probe() {
      static_cast<void>(owner->snapshot());
      destroyed->fetch_add(1, std::memory_order_release);
   }
};

struct compute_outcome {
   std::size_t allocations = 0;
   std::uint64_t submitted = 0;
   bool allocation_failed = false;
};

compute_outcome exercise_compute_setup(std::size_t ordinal) {
   // All borrowed probes/gates outlive the native pool and runtime on failures.
   auto gate = io_gate{};
   auto started = std::atomic_bool{false};
   auto runs = std::atomic_size_t{0};
   auto destroyed = std::atomic_size_t{0};
   auto result = compute_outcome{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1}};
   auto pool = forge::asio::compute::pool{forge::asio::compute::pool::options{.worker_threads = 1}};
   auto captured = std::shared_ptr<compute_probe>{new compute_probe{&pool, &destroyed}};
   auto candidate = boost::asio::co_spawn(
       runtime.context(),
       [&, captured = std::move(captured)]() mutable -> boost::asio::awaitable<void> {
          // Injection is IO-submit-thread only and stops as soon as the actual
          // worker enters. Never inject into post-quiescence Asio delivery.
          const auto failure = allocation_failure{ordinal, &started};
          try {
             co_await pool.get_executor().execute({.name = "allocation-execute"},
                                                  [captured = std::move(captured), &started, &runs, &gate] {
                                                     started.store(true, std::memory_order_release);
                                                     runs.fetch_add(1, std::memory_order_release);
                                                     gate.hold();
                                                  });
          } catch (const std::bad_alloc&) {
             result.allocation_failed = true;
          }
          result.allocations = failure.calls();
       },
       boost::asio::use_future);
   const auto release = gate_release{gate};
   const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!started.load(std::memory_order_acquire) &&
          candidate.wait_for(std::chrono::milliseconds{1}) != std::future_status::ready &&
          std::chrono::steady_clock::now() < deadline) {
   }
   const auto entered = started.load(std::memory_order_acquire);
   const auto premature = entered && candidate.wait_for(std::chrono::milliseconds{0}) == std::future_status::ready;
   gate.release();
   const auto ready = candidate.wait_for(std::chrono::seconds{2}) == std::future_status::ready;
   auto unexpected = std::exception_ptr{};
   if (ready) {
      try {
         candidate.get();
      } catch (...) {
         unexpected = std::current_exception();
      }
   }
   forge::asio::blocking::run(runtime, pool.shutdown());
   result.submitted = pool.snapshot().submitted;
   BOOST_REQUIRE(ready);
   BOOST_REQUIRE(unexpected == nullptr);
   BOOST_REQUIRE(!premature);
   BOOST_REQUIRE(!gate.timed_out);
   BOOST_REQUIRE_EQUAL(destroyed.load(std::memory_order_acquire), 1U);
   BOOST_REQUIRE_EQUAL(runs.load(std::memory_order_acquire), result.allocation_failed ? 0U : 1U);
   BOOST_REQUIRE_EQUAL(pool.snapshot().running, 0U);
   BOOST_REQUIRE_EQUAL(pool.snapshot().pending, 0U);
   return result;
}

} // namespace

// Boost 1.90 memory.hpp uses std::aligned_alloc for recycling_allocator's slow
// path in this C++23 profile (the owning task.cpp object references this symbol).
// The executable's standard C replacement returns NULL for that selected call;
// Boost then throws bad_alloc. No production allocator/hook is introduced.
extern "C" void* aligned_alloc(std::size_t alignment,
                               std::size_t size) noexcept(noexcept(std::aligned_alloc(alignof(std::max_align_t),
                                                                                      sizeof(std::max_align_t)))) {
   if (should_fail(2)) {
      return nullptr;
   }
   void* result = nullptr;
   if (posix_memalign(&result, alignment, size == 0 ? 1 : size) != 0) {
      return nullptr;
   }
   return result;
}

void* operator new(std::size_t size) {
   return allocate(size);
}
void* operator new[](std::size_t size) {
   return allocate(size);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
   return allocate(size, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
   return allocate(size, static_cast<std::size_t>(alignment));
}
void operator delete(void* value) noexcept {
   std::free(value);
}
void operator delete[](void* value) noexcept {
   std::free(value);
}
void operator delete(void* value, std::size_t) noexcept {
   std::free(value);
}
void operator delete[](void* value, std::size_t) noexcept {
   std::free(value);
}
void operator delete(void* value, std::align_val_t) noexcept {
   std::free(value);
}
void operator delete[](void* value, std::align_val_t) noexcept {
   std::free(value);
}
void operator delete(void* value, std::size_t, std::align_val_t) noexcept {
   std::free(value);
}
void operator delete[](void* value, std::size_t, std::align_val_t) noexcept {
   std::free(value);
}

BOOST_AUTO_TEST_CASE(task_throwing_submit_has_no_admission_cold_or_warm) {
   for (const auto warm : {false, true}) {
      for (const auto delayed : {false, true}) {
         for (const auto awaitable : {false, true}) {
            sweep(delayed, awaitable, warm);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(task_small_callable_destructors_reenter_outside_heap_mutex) {
   for (const auto delayed : {false, true}) {
      for (const auto cancel_one : {false, true}) {
         reentrant_cleanup(delayed, cancel_one);
      }
   }
}

BOOST_AUTO_TEST_CASE(task_parallel_drains_keep_delayed_timer_and_stop_ownership) {
   for (auto repeat = 0U; repeat < 8U; ++repeat) {
      concurrent_ready_delayed_stop();
   }
}

BOOST_AUTO_TEST_CASE(compute_execute_setup_failure_never_detaches_a_borrowed_callback) {
   const auto control = exercise_compute_setup(0);
   BOOST_REQUIRE_GT(control.allocations, 0U);
   BOOST_REQUIRE_LE(control.allocations, 64U);
   auto prepublication_failure = false;
   auto accepted_post_failure = false;
   for (auto ordinal = std::size_t{1}; ordinal <= control.allocations; ++ordinal) {
      const auto result = exercise_compute_setup(ordinal);
      if (result.allocation_failed) {
         prepublication_failure |= result.submitted == 0;
         accepted_post_failure |= result.submitted == 1;
      }
   }
   BOOST_CHECK(prepublication_failure);
   BOOST_CHECK(accepted_post_failure);
}
