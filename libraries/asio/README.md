# forge_asio

`forge_asio` owns the shared async runtime primitives used by FORGE networking and
applications. It wraps Boost.Asio with explicit runtime ownership, blocking
boundaries, a priority task scheduler, a bounded CPU compute pool, FIFO gates,
race-safe notifications and single-thread affine execution.

## When To Use

- A library needs an owned `boost::asio::io_context` runtime.
- Background work needs bounded queues, cancellation and deterministic shutdown.
- Long synchronous CPU work must not occupy runtime workers.
- Blocking code must be isolated from coroutine-first paths.

## When Not To Use

- Do not use it as a generic global job system.
- Do not encode application priority names here; `forge::asio::task::priority` is numeric.
- Do not expose `std::future` as public async API. FORGE async APIs use
  `boost::asio::awaitable<T>`.

## Public Modules

- `forge.asio.runtime` — owned `io_context` and worker threads.
- `forge.asio.blocking` — explicit blocking boundary helpers.
- `forge.asio.task` — bounded priority scheduler and task handles.
- `forge.asio.compute` — bounded FIFO execution for synchronous CPU work.
- `forge.asio.gate` — cancellation-aware FIFO admission with RAII tickets.
  `acquire(stop_token)` combines an explicit standard stop token with existing
  Asio cancellation without changing the caller's cancellation policy.
- `forge.asio.notification` — sticky epoch notifications for shared async state.
  Explicit stop-token waits also retain associated Asio cancellation; both
  sources wake the same mutex-protected waiter without changing caller filters.
- `forge.asio.affine` — bounded synchronous execution on one owned OS thread.
- `forge.asio.child_signal_registration` — checked native SIGCHLD registration
  shared by child-process owners across injected runtimes on macOS. It owns no
  child, process handle, scheduler, signal callback, or wait/reap policy.

Target: `forge_asio`.

Dependencies: Boost.Asio and threads.

`child_signal_registration` starts empty. Call `acquire(executor)` outside a
throwing owner constructor: a failure after native registration commits retains
that same registration for explicit cleanup. The first owner requires the exact
native default SIGCHLD disposition (`SIG_DFL`, zero flags, empty mask). Later
owners may join only the unchanged registration installed by this component.
Foreign handlers, ignored signals, custom default flags/masks, and auto-reaping
are rejected without changing the disposition. Do not install another handler
or reap another owner's child while registrations are active.

After every owned child has exited/reaped and every process handle and callback
has drained, call `release()` while its executor's context remains alive. Release
is idempotent. A foreign change or checked unregister failure retains ownership
and reports a typed Asio error; callers must restore the agreed handler and
retry cleanup. Destroying an acquired registration terminates rather than
silently unregistering or overwriting a foreign handler. Normal shutdown must
explicitly release before destroying runtimes. This is a registration guard,
not a process or shutdown framework.

## Stability

The `forge.asio.task`, `forge.asio.compute`, `forge.asio.gate`,
`forge.asio.notification` and `forge.asio.affine` public C++ APIs are Preview in
Forge 8.x. MINOR releases may make documented source-level changes to these
surfaces. Runtime ownership, bounded admission and deterministic shutdown are
production requirements, but the exact type vocabulary may still evolve before
it is declared Stable.

The 8.3 migration from `forge.asio.task_scheduler` to `forge.asio.task` is
documented in the Forge 8.3 release notes.

## Examples

### Own A Runtime

```cpp
import forge.asio.runtime;

auto runtime = forge::asio::runtime{{.worker_threads = 2, .thread_name = "worker"}};
auto& io = runtime.context();
runtime.stop();
```

### Bridge Coroutine Code To A Blocking Entrypoint

Use `blocking::run` at the edge of a command-line tool, test, migration or
small utility. Do not push it into reusable async APIs.

```cpp
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <chrono>

import forge.asio.blocking;
import forge.asio.runtime;

boost::asio::awaitable<int> calculate_after_delay() {
   boost::asio::any_io_executor executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{25}};
   co_await timer.async_wait(boost::asio::use_awaitable);
   co_return 42;
}

auto runtime = forge::asio::runtime{};
auto value = forge::asio::blocking::run(runtime, calculate_after_delay());
```

### Bound A Blocking Wait With Timeout

`run_for` is useful for tests and operator probes where a missing callback should
be reported as a timeout, not as an infinite hang.

```cpp
import forge.asio.blocking;

auto completed = forge::asio::blocking::run_for(
   runtime,
   wait_for_readiness_probe(),
   std::chrono::seconds{5});

if (!completed) {
   report_unavailable("readiness probe timed out");
}
```

### Submit Priority Work

```cpp
#include <boost/asio/awaitable.hpp>

import forge.asio.task;

boost::asio::awaitable<void> submit_metadata_refresh(forge::asio::runtime& runtime) {
   auto scheduler = forge::asio::task::scheduler{
      runtime,
      forge::asio::task::scheduler::options{
         .max_blocking_tasks = 4,
         .max_awaitable_tasks = 1024,
         .max_pending_tasks = 4096,
      },
   };
   auto refresh = scheduler.submit({
      .priority = forge::asio::task::priority{100},
      .name = "metadata-refresh",
      .work = [] { refresh_metadata(); },
   });

   co_await refresh.wait();
}
```

### Runnable Semantics

`scheduler` has one delayed/ready priority queue. Runnable work is selected
by numeric priority and FIFO order within the same priority, but admission uses
separate budgets:

- `max_blocking_tasks` limits blocking `task` functions until `.work()` returns.
- `max_awaitable_tasks` limits in-flight `awaitable` coroutines until the
  coroutine completes.
- `max_pending_tasks` limits not-yet-started work across both task types.

A saturated blocking task at the head of the queue does not block a runnable
awaitable task, and a saturated awaitable budget does not consume blocking
capacity. This mirrors the useful `fc::thread` scheduling property without
copying FC fibers or hiding another runtime under FORGE.

This matters for re-entrant workflows: an awaitable pass may submit a short
blocking companion task to the same scheduler and wait for it. That must not
deadlock just because the awaitable itself is already in flight.

### Inspect Admission

`task::handle::accepted()` reports whether a task was admitted to the bounded
scheduler queue. It becomes true on admission and remains true if that task is
later canceled or fails. It is false for an immediate rejection because the
scheduler is stopped or its pending queue is full; `wait()` reports that
rejection as an exception.

`submit(...)` and `submit_after(...)` provide strong admission: if submission
throws before returning a handle, its candidate was not admitted and will not
run later. Queue growth and posting complete before the accepted state is
published. Callable payloads retired by failed submission, cancellation or
queue shutdown are destroyed outside the scheduler queue mutex, so their
destructors do not run while that mutex is held.

An accepted task may start before submission returns. Observe its result with
`handle::wait()`; strong admission does not roll back already accepted work or
promise recovery from every later asynchronous allocation failure. Immediate
stopped/full-queue rejection still returns an unaccepted handle as above.

### Delegate CPU Work To Compute

`task::scheduler` owns orchestration, numeric priority and delayed admission.
`compute::pool` owns parallel synchronous CPU execution. It has no priority or
deadline policy. A scheduler task explicitly captures a compute executor and
passes its cancellation token:

```cpp
#include <boost/asio/awaitable.hpp>

import forge.asio.compute;
import forge.asio.task;

boost::asio::awaitable<void> execute_vm(
    forge::asio::task::scheduler& scheduler,
    forge::asio::compute::executor compute) {
   auto scheduled = scheduler.submit(forge::asio::task::awaitable{
      .priority = forge::asio::task::priority{100},
      .name = "vm",
      .work = [compute](forge::asio::task::context& task_context)
          -> boost::asio::awaitable<void> {
         co_await compute.execute(
            {
               .name = "vm-exec",
               .parent_stop_token = task_context.stop_token(),
            },
            [](forge::asio::compute::context& compute_context) {
               run_vm(compute_context.stop_token());
            });
      },
   });

   co_await scheduled.wait();
}
```

The coroutine suspends while the callable runs on a compute worker. Timers,
network continuations and database coroutines remain runnable on the original
Asio runtime. Completion is dispatched back to the executor that awaited the
operation.

Independent jobs can be submitted before they are awaited:

```cpp
auto left = co_await compute.submit({.name = "left"}, [] { return calculate_left(); });
auto right = co_await compute.submit({.name = "right"}, [] { return calculate_right(); });

auto left_value = co_await std::move(left).wait();
auto right_value = co_await std::move(right).wait();
```

`max_pending_tasks` bounds admitted jobs beyond the running workers.
`max_waiting_submissions` separately bounds coroutines waiting for admission.
`try_submit()` never waits. Cancellation removes pending work; running work sees
a `std::stop_token` and must cooperate. FORGE never terminates a worker thread.

Use `execute()` when a callable borrows a caller-owned transaction, buffer or
other scoped object. It registers its completion and cooperative cancellation
before publishing the job. After admission, it does not finish (including on
cancellation or a work exception) until the callback has exited and its callable
captures have been destroyed. Pending cancellation retires those captures without
invoking the callback. Capture destructors run outside the pool mutex. A worker
exception is preserved; cancellation observed after a successful callback reports
`canceled`. Keep the Asio executor and compute pool alive through this drain, and
make synchronous callbacks cooperate with their stop token. This can take longer
than the caller's cancellation request; cancellation is not forced termination.

`submit()`/`try_submit()` and `operation::wait()` remain detached-handle APIs:
discarding a handle does not join its work, and canceling a wait may return before
running work exits. Their callables must independently own everything they use,
or an external owner must explicitly retain and drain the pool. Do not substitute
`submit()` followed by a newly created `wait()` for the scoped `execute()` contract.

Completion uses standard Asio `post` on the awaiting executor, including its strand;
it never resumes the caller inline on a compute worker. Executor/service delivery
failures after work quiescence remain host-runtime failures, not ordinary work
cancellation. This is not a global out-of-memory or executor-availability guarantee;
there is no allocator fallback, swallowed delivery error or forced abort path.

### Serialize Async Ownership With A Gate

`gate` is a neutral FIFO admission primitive. Its move-only ticket releases
automatically, including exception paths. Cancellation before a grant throws
`forge::asio::exceptions::canceled`; closing the gate rejects queued and future
acquisitions.

```cpp
import forge.asio.gate;

forge::asio::gate writer_gate;

auto ticket = co_await writer_gate.acquire();
co_await mutate_shared_state();
// ticket releases here
```

The gate does not start detached work and does not own an executor. Waiting
coroutines remain owned by their callers. A ticket may be released from another
thread. `acquire()` confines its grant cancellation filter to an awaited child
coroutine, preserving the caller's cancellation policy and pending cancellation
for work performed after acquisition, including after a rejected acquisition.

### Wait For Shared State Changes

`notification` publishes a monotonically changing epoch. A waiter supplies the
epoch it has already observed; it completes immediately if that epoch has since
changed, so a notification cannot be lost between checking state and starting
the wait. `async_wait_until()` adds a bounded deadline. The no-token overloads
preserve the awaiting coroutine's associated Asio cancellation. The
`std::stop_token` overloads provide explicit cancellation that remains scoped
to one waiter and is safe when requested from another thread; they do not
replace or assign the coroutine's Asio cancellation slot.

```cpp
import forge.asio.notification;

forge::asio::notification changed;

const auto observed = changed.epoch();
publish_shared_state();
changed.notify();

const auto current = co_await changed.async_wait(observed);
```

For a cancellable wait, pass an owned stop token:

```cpp
std::stop_source stop;
const auto current = co_await changed.async_wait(observed, stop.get_token());
```

### Execute Thread-Affine Native Work

`affine::lane` owns exactly one OS thread and gives consumers a copyable
executor. It is intended for native libraries whose handles or transactions
must remain on one thread. Callables are synchronous; returning an Asio
`awaitable` is rejected at compile time.

```cpp
import forge.asio.affine;

forge::asio::affine::lane lane{{
   .max_pending_operations = 1024,
   .max_waiting_submissions = 1024,
   .thread_name = "native-db",
}};

auto executor = lane.get_executor();
auto value = co_await executor.execute(
   {.name = "native-read"},
   [] { return perform_native_read(); });

co_await lane.shutdown();
```

Admission is FIFO and bounded independently for accepted pending operations and
submissions waiting for capacity. Cancellation before execution removes the
operation and throws `canceled`. Once the callable starts, completion wins: its
value or exception is returned. `request_stop()` rejects new and waiting work,
cancels pending work and lets the running callable finish; `shutdown()` then
joins the worker. Completions resume on the executor that awaited `execute()`.
The operation's completion-wins filter is confined to an awaited child coroutine;
returning a value or exception preserves the caller's cancellation policy,
pending cancellation and `throw_if_cancelled` setting for subsequent async work.
`snapshot()` reports queue depths, terminal outcomes and accumulated queue and
execution time. The lane owner is the lifecycle boundary: it must outlive all
consumers of its executor and shut them down before calling `lane.shutdown()`.

#### Scoped Cancellation Composition

Affine execution and gate acquisition register a scoped handler on the caller's
existing output cancellation slot. They do not replace its input/output filters,
clear its recorded cancellation, or change `throw_if_cancelled`. Only cancellation
types admitted by the caller's filters reach the operation's existing cancel
path. After success, work failure, rejection or caught cancellation, the caller
retains its own cancellation state; its next pending timer or native operation
can receive a later admitted cancellation normally.

The terminal wait is a joined, disconnected `co_spawn` child on the SAME caller
executor, not a new worker or runtime. Before child initiation, no work, gate
waiter or shutdown request has been admitted, so the caller's ordinary
pre-await cancellation check can unwind safely. After initiation, cancellation
only requests the existing operation transition: the child does not finish
until that transition is terminal. For a running affine callable its actual
value/exception wins after callback exit; a queued gate cancellation releases
an unconsumed grant, whereas an already returned ticket remains its owner's.
The void affine child preserves move-constructible results that have no default
constructor.

`lane.shutdown()` also initiates stop/drain only inside its joined disconnected
child. It waits for the running callback and joins the worker without resetting
the caller's filters, cancellation history or throw policy. This is operation
ownership, not cancellation shielding for the caller's subsequent work, and
does not add the stronger compute-execute guarantee about retirement of all
callable captures. The lane owner must still drain its consumers before shutdown.

Ordinary Asio regressions exercise custom filters, both throw policies, existing
cancellation history, next-timer delivery, queued/grant races and active
completion/shutdown. Store startup additionally retains both its callback-owned
timer cancellation control and the original upstream-cancellation path, including
rollback and reopening the same native Store. Test-source coverage is not a
claim of successful owning/runtime execution.

### Isolate Blocking Work Behind The Scheduler

The scheduler is the boundary for short blocking functions that should not run
inline on a coroutine hot path. The reusable library still exposes a coroutine
wait handle instead of `std::future`.

```cpp
#include <boost/asio/awaitable.hpp>

import forge.asio.task;

boost::asio::awaitable<void> refresh_index(forge::asio::task::scheduler& scheduler) {
   auto index_job = scheduler.submit({
      .priority = forge::asio::task::priority{25},
      .name = "index-refresh",
      .work = [] {
         rebuild_small_index_from_disk();
      },
   });

   co_await index_job.wait();
}
```

Do not capture stack references in `.work` unless you can prove the work will
finish before the referenced object goes away.

### Schedule Coroutine Work

Use `awaitable` when a background pass needs to call coroutine APIs while
still using the scheduler's bounded queue, priority, cancellation and metrics.
The task remains single-shot.

```cpp
#include <boost/asio/awaitable.hpp>

import forge.asio.task;

boost::asio::awaitable<void> refresh_remote_index();

boost::asio::awaitable<void> refresh_once(forge::asio::task::scheduler& scheduler) {
   auto handle = scheduler.submit(forge::asio::task::awaitable{
      .priority = forge::asio::task::priority{-25},
      .name = "remote-index-refresh",
      .work =
         [](forge::asio::task::context& context) -> boost::asio::awaitable<void> {
         context.throw_if_cancel_requested();
         co_await refresh_remote_index();
      },
   });

   co_await handle.wait();
}
```

### Repeat Work By Resubmitting

The scheduler intentionally does not own periodic policy. A host or plugin owns
the loop by submitting the next single-shot task after the current pass
completes.

```cpp
#include <boost/asio/awaitable.hpp>

#include <chrono>

import forge.asio.task;

class scrub_worker {
 public:
   explicit scrub_worker(forge::asio::task::scheduler& scheduler) : scheduler_{&scheduler} {}

   void start() {
      running_ = true;
      schedule_next();
   }

   void request_stop() noexcept {
      running_ = false;
      if (handle_.valid()) {
         handle_.cancel();
      }
   }

   boost::asio::awaitable<void> shutdown() {
      if (handle_.valid()) {
         try {
            co_await handle_.wait();
         } catch (const forge::asio::exceptions::canceled&) {
            // Normal stop path for a pending background pass.
         }
      }
   }

 private:
   void schedule_next() {
      if (!running_) {
         return;
      }

      handle_ = scheduler_->submit_after(
         forge::asio::task::awaitable{
            .priority = forge::asio::task::priority{-50},
            .name = "scrub-pass",
            .work =
               [this](forge::asio::task::context& context) -> boost::asio::awaitable<void> {
               context.throw_if_cancel_requested();
               co_await run_one_pass();
               schedule_next();
            },
         },
         std::chrono::seconds{30});
   }

   boost::asio::awaitable<void> run_one_pass();

   forge::asio::task::scheduler* scheduler_ = nullptr;
   forge::asio::task::handle handle_;
   bool running_ = false;
};
```

### Handle Queue Backpressure

`max_pending_tasks` is part of correctness. Callers must handle rejection
instead of assuming the queue can grow forever.

```cpp
boost::asio::awaitable<void> run_small_job(forge::asio::runtime& runtime) {
   auto scheduler = forge::asio::task::scheduler{
      runtime,
      forge::asio::task::scheduler::options{
         .max_blocking_tasks = 1,
         .max_pending_tasks = 2,
      },
   };

   auto accepted = scheduler.submit({
      .priority = forge::asio::task::priority{0},
      .name = "small-job",
      .work = [] { do_small_job(); },
   });

   try {
      co_await accepted.wait();
   } catch (const forge::asio::exceptions::rejected& error) {
      report_busy(error.what()); // for example: scheduler queue is full
   }
}
```

Queue rejection is an application signal. A daemon should surface a typed busy or
backpressure error, not spawn an unbounded fallback thread to “make progress”.

### Use Numeric Priorities Without Application Vocabulary

The scheduler only knows numbers. An application can define its own named constants
near the component that owns those meanings.

```cpp
namespace priorities {
   inline constexpr auto foreground = forge::asio::task::priority{500};
   inline constexpr auto background = forge::asio::task::priority{-100};
}

auto hot = scheduler.submit({
   .priority = priorities::foreground,
   .name = "foreground-read",
   .work = [] { serve_foreground_request(); },
});

auto cold = scheduler.submit({
   .priority = priorities::background,
   .name = "background-refresh",
   .work = [] { refresh_cache_index(); },
});
```

### Cancel Delayed Work

```cpp
auto handle = scheduler.submit_after(
   {.priority = forge::asio::task::priority{0}, .name = "retry", .work = [] { retry(); }},
   std::chrono::milliseconds{250});

handle.cancel();
```

### Use Timers Instead Of Poll Loops

```cpp
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

boost::asio::awaitable<void> retry_later() {
   boost::asio::any_io_executor executor = co_await boost::asio::this_coro::executor;
   auto timer = boost::asio::steady_timer{executor, std::chrono::milliseconds{200}};
   co_await timer.async_wait(boost::asio::use_awaitable);
   co_await retry_once();
}
```

This keeps cancellation and shutdown visible to the runtime. Avoid background
threads that sleep and poll shared state.

### Shut Down Deterministically

Coroutine owners call `shutdown()` when the service is stopping. Pending work
is canceled, running work is allowed to finish through its normal function
body, and the wait does not block the Asio worker needed by active awaitables.
Owners that must stop admission before draining another executor call
`request_stop()` first and await `shutdown()` after that executor has drained.
The synchronous `stop()` is reserved for host-side/destructor boundaries where
another runtime worker can still make progress.

```cpp
boost::asio::awaitable<void> stop_with_pending_work(forge::asio::task::scheduler& scheduler) {
   auto pending = scheduler.submit_after(
      {.priority = forge::asio::task::priority{0}, .name = "slow-retry", .work = [] { retry(); }},
      std::chrono::minutes{1});

   co_await scheduler.shutdown();
   co_await pending.wait();

   auto metrics = scheduler.snapshot();
   assert(metrics.stopped);
   assert(metrics.canceled >= 1);
   assert(metrics.running_blocking == 0);
   assert(metrics.running_awaitable == 0);
}
```

## Backpressure And Shutdown

`scheduler::options::max_pending_tasks` is a correctness knob. Saturated
queues reject work instead of growing without bound. Blocking and awaitable
budgets are separate, so a daemon can prevent blocking pool exhaustion without
accidentally throttling coroutine progress. `request_stop()` rejects new work
and cancels queued work without waiting. `shutdown()` performs the same stop
request idempotently and asynchronously waits for both blocking and awaitable
running counts to reach zero. `stop()` provides the equivalent blocking
host-side boundary.

`compute::pool::request_stop()` rejects waiting/new submissions, cancels pending
jobs and requests cooperative stop from running jobs. `shutdown()` awaits every
running callable, runs worker stop hooks and joins the underlying
`boost::asio::thread_pool`. The App owner stops plugins first, requests task
scheduler stop, drains compute while already-running scheduler continuations
can finish, then drains the scheduler and finally stops the Asio runtime.

`affine::lane` has a stricter single-worker lifecycle: its owner must stop all
native sessions using the copyable executor before awaiting lane shutdown.
Long-running callables delay shutdown by design, so native operations must stay
bounded.

## Runtime Risks And Anti-Patterns

- Do not detach raw `std::thread` workers around the scheduler. That bypasses
  cancellation, metrics and deterministic shutdown.
- Do not use `std::async` as a daemon worker pool. It has no FORGE backpressure or
  lifecycle integration.
- Do not submit long CPU work as a synchronous scheduler task. Delegate it to a
  compute executor from an awaitable scheduler task.
- Do not sleep in polling loops on runtime threads. Use timers, task handles and
  explicit cancellation.
- Do not make blocking tasks wait for awaitable work that can only resume on the
  same saturated runtime. Keep blocking sections short, or move the wait back to
  an awaitable task.
- Do not build product-local `steady_timer` plus `co_spawn` scheduling loops for
  plugin background work. Use `awaitable` and resubmit single-shot passes
  through the scheduler so backpressure, cancellation and metrics remain visible.
- Do not call `stop()` before consumers have awaited cleanup handles. A stopped
  scheduler rejects new cleanup work by design.
- Do not capture stack references in queued tasks unless the owner awaits or
  cancels the handle before the referenced object can die.

## Typical Mistakes

- Do not sleep/poll inside runtime loops; use timers and scheduler handles.
- Do not let a task capture references whose lifetime is shorter than the
  scheduler queue.
- Do not hide blocking I/O in coroutine paths without the blocking boundary.

## Tests

`test_forge_asio` covers priority/FIFO ordering, delayed execution, cancellation,
queue saturation, separated blocking/awaitable budgets, compute parallelism,
bounded admission, gate grant races, thread affinity, continuation placement,
completion-wins cancellation, cooperative cancellation, worker hooks and
deterministic shutdown.
