module;

#include "details/stop_state.hxx"
#include "details/async_waiter.hxx"
#include "details/thread_name.hxx"

#include <boost/asio/any_completion_handler.hpp>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/execution/outstanding_work.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/prefer.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <latch>
#include <list>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

module forge.asio.compute;

import forge.asio.exceptions;

namespace forge::asio::compute {
namespace {

enum class admission_state : std::uint8_t {
   queued,
   granted,
   canceled,
   rejected,
   completed,
};

struct admission_waiter : forge::asio::detail::async_waiter {
   admission_waiter(boost::asio::any_io_executor executor, std::uint64_t waiter_id)
       : forge::asio::detail::async_waiter{std::move(executor)}, id{waiter_id} {}

   std::uint64_t id = 0;
   admission_state state = admission_state::queued;
   forge::asio::detail::stop_state stop_state;
   std::exception_ptr error;
};

std::exception_ptr canceled_error(std::string message = "compute operation was canceled") {
   return std::make_exception_ptr(exceptions::canceled{std::move(message)});
}

thread_local std::optional<std::size_t> current_worker_index;

} // namespace

context::context(std::stop_token stop_token) noexcept : stop_token_{std::move(stop_token)} {}

std::stop_token context::stop_token() const noexcept {
   return stop_token_;
}

bool context::stop_requested() const noexcept {
   return stop_token_.stop_requested();
}

void context::throw_if_stop_requested() const {
   if (stop_requested()) {
      throw exceptions::canceled{"compute operation was canceled"};
   }
}

namespace detail {

struct operation_state::impl {
   impl(std::shared_ptr<pool_state> owner_value, std::uint64_t operation_id)
       : owner{std::move(owner_value)}, id{operation_id} {}

   std::weak_ptr<pool_state> owner;
   std::uint64_t id = 0;
   forge::asio::detail::stop_state stop;
   mutable std::mutex mutex;
   bool completed = false;
   bool wait_started = false;
   std::exception_ptr error;
   std::exception_ptr canceled = canceled_error();
   std::shared_ptr<forge::asio::detail::async_waiter> waiter;
   boost::asio::any_completion_handler<void(std::exception_ptr)> completion;
};

operation_state::operation_state(std::shared_ptr<pool_state> owner, std::uint64_t id)
    : impl_{std::make_unique<impl>(std::move(owner), id)} {}

operation_state::~operation_state() = default;

std::uint64_t operation_state::id() const noexcept {
   return impl_->id;
}

bool operation_state::cancel() noexcept {
   const auto changed = impl_->stop.request_stop();
   if (changed) {
      if (auto owner = impl_->owner.lock()) {
         static_cast<void>(owner->cancel(impl_->id));
      }
   }
   return changed;
}

bool operation_state::stop_requested() const noexcept {
   return impl_->stop.stop_requested();
}

std::stop_token operation_state::stop_token() const noexcept {
   return impl_->stop.token();
}

void operation_state::link_parent(std::stop_token parent) {
   auto weak = weak_from_this();
   impl_->stop.link(std::move(parent), [weak] {
      if (auto self = weak.lock()) {
         static_cast<void>(self->cancel());
      }
   });
}

void operation_state::request_stop_only() noexcept {
   static_cast<void>(impl_->stop.request_stop());
}

bool operation_state::completed() const noexcept {
   const auto lock = std::scoped_lock{impl_->mutex};
   return impl_->completed;
}

std::exception_ptr operation_state::cancellation_error() const noexcept {
   return impl_->canceled;
}

void operation_state::install_completion(boost::asio::any_completion_handler<void(std::exception_ptr)>&& completion) {
   const auto lock = std::scoped_lock{impl_->mutex};
   if (impl_->wait_started || impl_->completed) {
      throw exceptions::invalid_state{"compute execution completion is already registered"};
   }
   impl_->wait_started = true;
   impl_->completion = std::move(completion);
}

bool operation_state::complete_value() {
   auto waiter = std::shared_ptr<forge::asio::detail::async_waiter>{};
   auto completion = boost::asio::any_completion_handler<void(std::exception_ptr)>{};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->completed) {
         return false;
      }
      impl_->completed = true;
      waiter = impl_->waiter;
      completion = std::move(impl_->completion);
   }
   if (waiter != nullptr) {
      waiter->wake();
   }
   if (completion) {
      completion(std::exception_ptr{});
   }
   return true;
}

bool operation_state::complete_exception(std::exception_ptr error) {
   auto waiter = std::shared_ptr<forge::asio::detail::async_waiter>{};
   auto completion = boost::asio::any_completion_handler<void(std::exception_ptr)>{};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->completed) {
         return false;
      }
      impl_->error = error;
      impl_->completed = true;
      waiter = impl_->waiter;
      completion = std::move(impl_->completion);
   }
   if (waiter != nullptr) {
      waiter->wake();
   }
   if (completion) {
      completion(std::move(error));
   }
   return true;
}

boost::asio::awaitable<void> operation_state::execute(reservation reservation_value, std::string name,
                                                      std::function<void(context&)>& work,
                                                      std::shared_ptr<continuation_state> continuation, bool drain) {
   auto self = shared_from_this();
   auto owner = impl_->owner.lock();
   if (!owner) {
      throw exceptions::invalid_state{"compute pool is unavailable"};
   }
   // Executor tracking may allocate. Prepare it while the coroutine still owns
   // its stack, BEFORE use_awaitable detaches it into the initiation handler.
   auto completion_executor = co_await boost::asio::this_coro::executor;
   auto completion_guard = boost::asio::prefer(completion_executor, boost::asio::execution::outstanding_work.tracked);
   auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   auto slot = cancellation.slot();
   if (slot.is_connected()) {
      slot.assign([weak = std::weak_ptr{self}](boost::asio::cancellation_type_t type) {
         if (type != boost::asio::cancellation_type::none) {
            if (auto operation = weak.lock()) {
               static_cast<void>(operation->cancel());
            }
         }
      });
   }
   const auto clear_slot = boost::scope::scope_exit{[&slot] {
      if (slot.is_connected()) {
         slot.clear();
      }
   }};
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      static_cast<void>(cancel());
   }

   // Initiation owns every potentially throwing continuation/cancellation setup
   // before submitting work. Cancellation only requests stop: the sole handler
   // is delivered after callback exit AND retirement of its callable captures.
   auto token = boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable);
   co_await boost::asio::async_initiate<decltype(token), void(std::exception_ptr)>(
       [self, owner = std::move(owner), reservation_value = std::move(reservation_value), name = std::move(name),
        executor = std::move(completion_executor), guard = std::move(completion_guard), &work,
        continuation = std::move(continuation), drain](auto handler) mutable {
          // A completion may run on another IO thread before initiation returns.
          // Do not access the initiation object's coroutine-frame captures after
          // publication: keep these owners on this initiating stack instead.
          auto operation = std::move(self);
          auto pool = std::move(owner);
          auto admission = std::move(reservation_value);
          auto work_name = std::move(name);
          auto retained = std::move(continuation);
          auto* invocation = &work;
          // The token above binds a disconnected slot. Extract its target via
          // the public binder API: the underlying awaitable handler and the
          // native tracked executor move without throwing. Our own slot above
          // remains the only cooperative cancellation registration.
          auto dispatch = [executor = std::move(executor), guard = std::move(guard),
                           handler = std::move(handler.get())](std::exception_ptr error) mutable {
             // Standard Asio delivery: dispatch/allocation failure AFTER Q is
             // a host-runtime failure, never an inline compute-worker resume.
             boost::asio::post(executor, [guard = std::move(guard), handler = std::move(handler),
                                          error = std::move(error)]() mutable { handler(std::move(error)); });
          };
          static_assert(std::is_nothrow_move_constructible_v<decltype(dispatch)>);
          auto completion = boost::asio::any_completion_handler<void(std::exception_ptr)>{};
          auto installed = false;
          try {
             // Boost allocates the erased wrapper BEFORE moving dispatch. Its
             // disconnected cancellation state cannot allocate on construction.
             // On allocation failure dispatch therefore still owns the handler.
             completion = boost::asio::any_completion_handler<void(std::exception_ptr)>{std::move(dispatch)};
             operation->install_completion(std::move(completion));
             installed = true;
             if (retained)
                pool->submit_continuation(std::move(retained), std::move(work_name), operation, std::move(*invocation),
                                          drain);
             else
                pool->submit(std::move(admission), std::move(work_name), operation, std::move(*invocation));
          } catch (...) {
             if (operation->completed()) {
                // A terminal completion's executor failed. Do not misreport it
                // as a second ordinary work error or swallow that host failure.
                throw;
             }
             auto error = std::current_exception();
             *invocation = {};
             if (installed) {
                operation->complete_exception(std::move(error));
             } else if (completion) {
                completion(std::move(error)); // Invariant check failed before the && transfer.
             } else {
                dispatch(std::move(error)); // Wrapper allocation failed before the target move.
             }
          }
       },
       token);
   if (!drain && cancellation.cancelled() != boost::asio::cancellation_type::none) {
      throw exceptions::canceled{"compute execution was canceled"};
   }
}

boost::asio::awaitable<void> operation_state::wait() {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   auto waiter = std::make_shared<forge::asio::detail::async_waiter>(executor);
   auto self = shared_from_this();

   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->wait_started) {
         throw exceptions::invalid_state{"compute operation can only be awaited once"};
      }
      impl_->wait_started = true;
      if (impl_->completed) {
         if (impl_->error) {
            std::rethrow_exception(impl_->error);
         }
         co_return;
      }
      impl_->waiter = waiter;
   }

   auto slot = cancellation.slot();
   if (slot.is_connected()) {
      slot.assign([weak = std::weak_ptr<operation_state>{self}](boost::asio::cancellation_type_t type) {
         if (type != boost::asio::cancellation_type::none) {
            if (auto locked = weak.lock()) {
               static_cast<void>(locked->cancel());
            }
         }
      });
   }
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      static_cast<void>(cancel());
      if (slot.is_connected()) {
         slot.clear();
      }
      throw exceptions::canceled{"compute operation wait was canceled"};
   }

   static_cast<void>(co_await waiter->wait());

   const auto wait_canceled = cancellation.cancelled() != boost::asio::cancellation_type::none;
   if (slot.is_connected()) {
      slot.clear();
   }
   if (wait_canceled) {
      static_cast<void>(cancel());
      throw exceptions::canceled{"compute operation wait was canceled"};
   }

   auto error = std::exception_ptr{};
   auto completed = false;
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      completed = impl_->completed;
      error = impl_->error;
      impl_->waiter.reset();
   }
   if (!completed) {
      static_cast<void>(cancel());
      throw exceptions::internal{"compute operation wake did not carry a completion"};
   }
   if (error) {
      std::rethrow_exception(error);
   }
}

reservation::reservation(std::shared_ptr<pool_state> owner, std::uint64_t id) noexcept
    : owner_{std::move(owner)}, id_{id} {}

reservation::~reservation() {
   if (owner_ != nullptr && id_ != 0) {
      owner_->release_reservation(id_);
   }
}

reservation::reservation(reservation&& other) noexcept
    : owner_{std::move(other.owner_)}, id_{std::exchange(other.id_, 0)} {}

reservation& reservation::operator=(reservation&& other) noexcept {
   if (this != &other) {
      if (owner_ != nullptr && id_ != 0) {
         owner_->release_reservation(id_);
      }
      owner_ = std::move(other.owner_);
      id_ = std::exchange(other.id_, 0);
   }
   return *this;
}

std::uint64_t reservation::id() const noexcept {
   return id_;
}

void reservation::consume() noexcept {
   id_ = 0;
   owner_.reset();
}

struct continuation_state::impl {
   impl(std::shared_ptr<pool_state> pool, reservation admission, std::string work_name)
       : owner{std::move(pool)}, retained{std::move(admission)}, name{std::move(work_name)} {}
   std::shared_ptr<pool_state> owner;
   reservation retained;
   std::string name;
   forge::asio::detail::stop_state stop;
   std::atomic_bool active{false};
};

continuation_state::continuation_state(std::shared_ptr<pool_state> owner, reservation admission, std::string name)
    : impl_{std::make_unique<impl>(std::move(owner), std::move(admission), std::move(name))} {}
continuation_state::~continuation_state() = default;
std::shared_ptr<pool_state> continuation_state::owner() const noexcept {
   return impl_->owner;
}
std::uint64_t continuation_state::id() const noexcept {
   return impl_->retained.id();
}
const std::string& continuation_state::name() const noexcept {
   return impl_->name;
}
std::stop_token continuation_state::stop_token() const noexcept {
   return impl_->stop.token();
}
void continuation_state::request_stop() noexcept {
   static_cast<void>(impl_->stop.request_stop());
}
bool continuation_state::begin_phase() noexcept {
   return !impl_->active.exchange(true, std::memory_order_acq_rel);
}
void continuation_state::end_phase() noexcept {
   impl_->active.store(false, std::memory_order_release);
}
void continuation_state::link_parent(std::stop_token parent) {
   impl_->stop.link(std::move(parent), [weak = weak_from_this()] {
      if (auto self = weak.lock())
         self->request_stop();
   });
}

struct pool_state::impl {
   struct job {
      std::uint64_t id = 0;
      std::string name;
      std::shared_ptr<operation_state> operation;
      std::function<void(context&)> work;
      std::chrono::steady_clock::time_point queued_at;
      std::exception_ptr dispatch_error;
      bool canceled_before_start = false;
      std::shared_ptr<continuation_state> continuation;
      bool drain = false;
   };

   explicit impl(configuration input)
       : config{std::move(input)},
         worker_threads{config.worker_threads == 0 ? std::max<std::size_t>(1, std::thread::hardware_concurrency())
                                                   : config.worker_threads},
         capacity{worker_threads + config.max_pending_tasks},
         native_pool{std::make_unique<boost::asio::thread_pool>(worker_threads)} {
      if (capacity < worker_threads) {
         throw exceptions::invalid_options{"compute pool capacity overflow"};
      }
      initialize_workers();
   }

   ~impl() = default;

   void initialize_workers() {
      auto ready = std::latch{static_cast<std::ptrdiff_t>(worker_threads)};
      auto release = std::latch{1};
      auto exited = std::latch{static_cast<std::ptrdiff_t>(worker_threads)};
      auto error_mutex = std::mutex{};
      auto error = std::exception_ptr{};

      for (std::size_t index = 0; index < worker_threads; ++index) {
         boost::asio::post(native_pool->get_executor(), [&, index] {
            current_worker_index = index;
            forge::asio::detail::set_current_thread_name(config.thread_name);
            try {
               if (config.on_worker_start) {
                  config.on_worker_start(index);
               }
            } catch (...) {
               const auto lock = std::scoped_lock{error_mutex};
               if (!error) {
                  error = std::current_exception();
               }
            }
            ready.count_down();
            release.wait();
            exited.count_down();
         });
      }

      ready.wait();
      release.count_down();
      exited.wait();
      if (error) {
         native_pool->stop();
         native_pool->join();
         throw exceptions::internal{"compute worker start hook failed"};
      }
   }

   [[nodiscard]] std::size_t occupied_locked() const noexcept {
      return reservations.size() + pending.size() + running_operations.size() + retiring - retained_phases;
   }

   [[nodiscard]] bool drained_locked() const noexcept {
      return pending.empty() && running_operations.empty() && retiring == 0 && retained.empty();
   }

   std::list<std::shared_ptr<admission_waiter>> grant_waiters_locked() {
      auto granted = std::list<std::shared_ptr<admission_waiter>>{};
      while (!stopping && occupied_locked() < capacity && !admission_waiters.empty()) {
         auto& waiter = admission_waiters.front();
         if (waiter->state != admission_state::queued) {
            admission_waiters.pop_front();
            continue;
         }
         try {
            reservations.insert(waiter->id);
            waiter->state = admission_state::granted;
         } catch (...) {
            waiter->error = std::current_exception();
            waiter->state = admission_state::rejected;
            ++current_metrics.rejected;
         }
         granted.splice(granted.end(), admission_waiters, admission_waiters.begin());
      }
      current_metrics.waiting = admission_waiters.size();
      return granted;
   }

   void start_jobs_locked(const std::shared_ptr<pool_state>& owner, std::list<job>& failed) {
      while (running_operations.size() < worker_threads && !pending.empty()) {
         if (stopping && !pending.front().drain)
            break;
         const auto next = pending.begin();
         running_operations.splice(running_operations.end(), pending, next);
         try {
            // Native thread_pool::post allocates its executor operation BEFORE
            // enqueueing it. The stable node stays owned here on that failure.
            boost::asio::post(native_pool->get_executor(), [owner, next] { owner->impl_->run_job(owner, next); });
         } catch (...) {
            next->dispatch_error = std::current_exception();
            ++retiring;
            failed.splice(failed.end(), running_operations, next);
         }
      }
      current_metrics.pending = pending.size();
      current_metrics.running = running_operations.size();
   }

   void retire_jobs(const std::shared_ptr<pool_state>& owner, std::list<job> retired) {
      while (!retired.empty()) {
         auto& current = retired.front();
         // User capture destruction may reenter the pool; never hold its lock.
         current.work = {};
         auto continuation = std::move(current.continuation);
         auto operation = current.operation;
         auto error = current.dispatch_error;
         auto granted = std::list<std::shared_ptr<admission_waiter>>{};
         auto drain = std::vector<std::shared_ptr<forge::asio::detail::async_waiter>>{};
         {
            const auto lock = std::scoped_lock{mutex};
            --retiring;
            if (continuation)
               --retained_phases;
            if (current.canceled_before_start) {
               ++current_metrics.canceled;
            } else {
               ++current_metrics.failed;
            }
            start_jobs_locked(owner, retired);
            granted = grant_waiters_locked();
            if (drained_locked()) {
               drain.swap(drain_waiters);
            }
         }
         retired.pop_front();
         if (continuation)
            continuation->end_phase();
         operation->complete_exception(std::move(error));
         for (const auto& waiter : granted) {
            waiter->wake();
         }
         for (const auto& waiter : drain) {
            waiter->wake();
         }
         drained_cv.notify_all();
      }
   }

   void run_job(const std::shared_ptr<pool_state>& owner, std::list<job>::iterator position) {
      auto& current = *position;
      const auto started_at = std::chrono::steady_clock::now();
      auto failed = false;
      auto canceled = false;
      auto error = std::exception_ptr{};

      try {
         if (current.drain) {
            auto stopped = false;
            {
               const auto lock = std::scoped_lock{mutex};
               stopped = stopping;
            }
            if (stopped)
               current.operation->request_stop_only();
         }
         auto work_context = context{current.operation->stop_token()};
         if (!current.drain)
            work_context.throw_if_stop_requested();
         current.work(work_context);
         if (!current.drain && current.operation->stop_requested()) {
            throw exceptions::canceled{"compute operation was canceled"};
         }
      } catch (const exceptions::canceled&) {
         canceled = true;
         error = std::current_exception();
      } catch (...) {
         failed = true;
         error = std::current_exception();
      }

      current.work = {};
      auto operation = current.operation;
      auto failed_jobs = std::list<job>{};
      auto retired = std::list<job>{};
      auto granted = std::list<std::shared_ptr<admission_waiter>>{};
      auto drain = std::vector<std::shared_ptr<forge::asio::detail::async_waiter>>{};
      {
         const auto lock = std::scoped_lock{mutex};
         retired.splice(retired.end(), running_operations, position);
         if (current.continuation)
            --retained_phases;
         if (canceled) {
            ++current_metrics.canceled;
         } else if (failed) {
            ++current_metrics.failed;
         } else {
            ++current_metrics.completed;
         }
         current_metrics.total_queue_time +=
             std::chrono::duration_cast<std::chrono::nanoseconds>(started_at - current.queued_at);
         current_metrics.total_execution_time +=
             std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started_at);
         start_jobs_locked(owner, failed_jobs);
         granted = grant_waiters_locked();
         if (drained_locked()) {
            drain.swap(drain_waiters);
         }
      }

      // Completion delivery is a standard Asio post, and may fail only AFTER
      // the accepted work and all callable captures have become quiescent.
      if (current.continuation)
         current.continuation->end_phase();
      if (error) {
         operation->complete_exception(std::move(error));
      } else {
         operation->complete_value();
      }
      retire_jobs(owner, std::move(failed_jobs));
      for (const auto& waiter : granted) {
         waiter->wake();
      }
      for (const auto& waiter : drain) {
         waiter->wake();
      }
      drained_cv.notify_all();
   }

   void cancel_waiter(const std::shared_ptr<admission_waiter>& waiter, bool rejected) noexcept {
      auto should_wake = false;
      auto granted = std::list<std::shared_ptr<admission_waiter>>{};
      {
         const auto lock = std::scoped_lock{mutex};
         if (waiter->state == admission_state::queued) {
            waiter->state = rejected ? admission_state::rejected : admission_state::canceled;
            const auto found = std::ranges::find(admission_waiters, waiter);
            if (found != admission_waiters.end()) {
               admission_waiters.erase(found);
            }
            if (rejected) {
               ++current_metrics.rejected;
            } else {
               ++current_metrics.canceled;
            }
            current_metrics.waiting = admission_waiters.size();
            should_wake = true;
         } else if (waiter->state == admission_state::granted) {
            waiter->state = rejected ? admission_state::rejected : admission_state::canceled;
            reservations.erase(waiter->id);
            if (rejected) {
               ++current_metrics.rejected;
            } else {
               ++current_metrics.canceled;
            }
            granted = grant_waiters_locked();
            should_wake = true;
         }
      }
      if (should_wake) {
         waiter->wake();
      }
      for (const auto& next : granted) {
         next->wake();
      }
   }

   boost::asio::awaitable<void> wait_for_drain() {
      const auto executor = co_await boost::asio::this_coro::executor;
      auto waiter = std::make_shared<forge::asio::detail::async_waiter>(executor);
      {
         const auto lock = std::scoped_lock{mutex};
         if (drained_locked()) {
            co_return;
         }
         drain_waiters.push_back(waiter);
      }
      static_cast<void>(co_await waiter->wait());
   }

   void finalize() {
      const auto lock = std::scoped_lock{finalize_mutex};
      if (finalized) {
         if (finalize_error) {
            std::rethrow_exception(finalize_error);
         }
         return;
      }

      if (config.on_worker_stop) {
         auto ready = std::latch{static_cast<std::ptrdiff_t>(worker_threads)};
         auto release = std::latch{1};
         auto exited = std::latch{static_cast<std::ptrdiff_t>(worker_threads)};
         auto error_mutex = std::mutex{};
         for (std::size_t index = 0; index < worker_threads; ++index) {
            boost::asio::post(native_pool->get_executor(), [&] {
               try {
                  if (!current_worker_index.has_value()) {
                     throw exceptions::internal{"compute worker identity is unavailable"};
                  }
                  config.on_worker_stop(*current_worker_index);
               } catch (...) {
                  const auto error_lock = std::scoped_lock{error_mutex};
                  if (!finalize_error) {
                     finalize_error = std::current_exception();
                  }
               }
               ready.count_down();
               release.wait();
               current_worker_index.reset();
               exited.count_down();
            });
         }
         ready.wait();
         release.count_down();
         exited.wait();
      }

      native_pool->join();
      finalized = true;
      if (finalize_error) {
         throw exceptions::internal{"compute worker stop hook failed"};
      }
   }

   configuration config;
   std::size_t worker_threads = 0;
   std::size_t capacity = 0;
   std::unique_ptr<boost::asio::thread_pool> native_pool;
   mutable std::mutex mutex;
   std::condition_variable drained_cv;
   std::list<std::shared_ptr<admission_waiter>> admission_waiters;
   std::unordered_set<std::uint64_t> reservations;
   // Weak membership coordinates stop/drain without a pool/capability cycle.
   std::map<std::uint64_t, std::weak_ptr<continuation_state>> retained;
   std::size_t retained_phases = 0;
   std::list<job> pending;
   std::list<job> running_operations;
   std::size_t retiring = 0;
   std::vector<std::shared_ptr<forge::asio::detail::async_waiter>> drain_waiters;
   std::atomic_uint64_t next_id = 1;
   bool stopping = false;
   mutable metrics_snapshot current_metrics{};
   std::mutex finalize_mutex;
   std::exception_ptr finalize_error;
   bool finalized = false;
};

pool_state::pool_state(configuration configuration) : impl_{std::make_unique<impl>(std::move(configuration))} {}

pool_state::~pool_state() = default;

boost::asio::awaitable<reservation> pool_state::reserve(std::stop_token parent) {
   if (parent.stop_requested()) {
      throw exceptions::canceled{"compute submission was canceled"};
   }

   const auto executor = co_await boost::asio::this_coro::executor;
   auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   auto waiter = std::shared_ptr<admission_waiter>{};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->stopping) {
         ++impl_->current_metrics.rejected;
         throw exceptions::rejected{"compute pool is stopped"};
      }
      if (impl_->admission_waiters.empty() && impl_->occupied_locked() < impl_->capacity) {
         const auto id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
         impl_->reservations.insert(id);
         co_return reservation{shared_from_this(), id};
      }
      if (impl_->admission_waiters.size() >= impl_->config.max_waiting_submissions) {
         ++impl_->current_metrics.rejected;
         throw exceptions::rejected{"compute submission wait queue is full"};
      }

      const auto id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
      waiter = std::make_shared<admission_waiter>(executor, id);
      impl_->admission_waiters.push_back(waiter);
      impl_->current_metrics.waiting = impl_->admission_waiters.size();
   }

   auto slot = cancellation.slot();
   const auto release_wait = boost::scope::scope_exit{[this, &slot, &waiter] {
      if (slot.is_connected()) {
         slot.clear();
      }
      waiter->stop_state.clear_link();
      impl_->cancel_waiter(waiter, false);
   }};
   auto weak_owner = weak_from_this();
   waiter->stop_state.link(std::move(parent), [weak_owner, weak_waiter = std::weak_ptr{waiter}] {
      if (auto owner = weak_owner.lock()) {
         if (auto locked_waiter = weak_waiter.lock()) {
            owner->impl_->cancel_waiter(locked_waiter, false);
         }
      }
   });

   if (slot.is_connected()) {
      slot.assign([weak_owner, weak_waiter = std::weak_ptr{waiter}](boost::asio::cancellation_type_t type) {
         if (type != boost::asio::cancellation_type::none) {
            if (auto owner = weak_owner.lock()) {
               if (auto locked_waiter = weak_waiter.lock()) {
                  owner->impl_->cancel_waiter(locked_waiter, false);
               }
            }
         }
      });
   }
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      impl_->cancel_waiter(waiter, false);
   }

   if (cancellation.cancelled() == boost::asio::cancellation_type::none) {
      static_cast<void>(co_await waiter->wait());
   }

   const auto canceled_after_wait = cancellation.cancelled() != boost::asio::cancellation_type::none;
   if (slot.is_connected()) {
      slot.clear();
   }
   if (canceled_after_wait) {
      impl_->cancel_waiter(waiter, false);
   }

   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (waiter->state == admission_state::granted) {
         waiter->state = admission_state::completed;
         co_return reservation{shared_from_this(), waiter->id};
      }
      if (waiter->state == admission_state::rejected) {
         if (waiter->error) {
            std::rethrow_exception(waiter->error);
         }
         throw exceptions::rejected{"compute pool stopped while submission was waiting"};
      }
   }
   throw exceptions::canceled{"compute submission was canceled"};
}

std::optional<reservation> pool_state::try_reserve(std::stop_token parent) {
   if (parent.stop_requested()) {
      throw exceptions::canceled{"compute submission was canceled"};
   }
   const auto lock = std::scoped_lock{impl_->mutex};
   if (impl_->stopping) {
      ++impl_->current_metrics.rejected;
      throw exceptions::rejected{"compute pool is stopped"};
   }
   if (!impl_->admission_waiters.empty() || impl_->occupied_locked() >= impl_->capacity) {
      ++impl_->current_metrics.rejected;
      return std::nullopt;
   }
   const auto id = impl_->next_id.fetch_add(1, std::memory_order_relaxed);
   impl_->reservations.insert(id);
   return reservation{shared_from_this(), id};
}

void pool_state::release_reservation(std::uint64_t id) noexcept {
   auto granted = std::list<std::shared_ptr<admission_waiter>>{};
   auto drain = std::vector<std::shared_ptr<forge::asio::detail::async_waiter>>{};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->reservations.erase(id) == 0) {
         return;
      }
      impl_->retained.erase(id);
      granted = impl_->grant_waiters_locked();
      if (impl_->drained_locked())
         drain.swap(impl_->drain_waiters);
   }
   for (const auto& waiter : granted) {
      waiter->wake();
   }
   for (const auto& waiter : drain)
      waiter->wake();
   impl_->drained_cv.notify_all();
}

std::uint64_t pool_state::next_operation_id() noexcept {
   return impl_->next_id.fetch_add(1, std::memory_order_relaxed);
}

std::shared_ptr<continuation_state> pool_state::retain(reservation admission, std::string name,
                                                       std::stop_token parent) {
   auto value = std::make_shared<continuation_state>(shared_from_this(), std::move(admission), std::move(name));
   value->link_parent(std::move(parent));
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->stopping) {
         ++impl_->current_metrics.rejected;
         throw exceptions::rejected{"compute pool is stopped"};
      }
      impl_->retained.emplace(value->id(), value);
   }
   return value;
}

void pool_state::submit_continuation(std::shared_ptr<continuation_state> continuation, std::string name,
                                     std::shared_ptr<operation_state> operation, std::function<void(context&)>&& work,
                                     bool drain) {
   auto incoming = std::list<impl::job>{};
   incoming.push_back(impl::job{.id = operation->id(),
                                .name = std::move(name),
                                .operation = operation,
                                .queued_at = std::chrono::steady_clock::now(),
                                .continuation = continuation,
                                .drain = drain});
   incoming.front().work.swap(work);
   auto failed = std::list<impl::job>{};
   auto owner = shared_from_this();
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->stopping && !drain) {
         ++impl_->current_metrics.rejected;
         throw exceptions::rejected{"compute pool is stopped"};
      }
      if (!impl_->retained.contains(continuation->id()))
         throw exceptions::invalid_state{"compute continuation admission is unavailable"};
      if (!continuation->begin_phase())
         throw exceptions::invalid_state{"compute continuation already has an outstanding phase"};
      ++impl_->retained_phases;
      ++impl_->current_metrics.submitted;
      if (!drain && operation->stop_requested()) {
         incoming.front().dispatch_error = operation->cancellation_error();
         incoming.front().canceled_before_start = true;
         ++impl_->retiring;
         failed.splice(failed.end(), incoming);
      } else {
         impl_->pending.splice(impl_->pending.end(), incoming);
         impl_->start_jobs_locked(owner, failed);
      }
   }
   impl_->retire_jobs(owner, std::move(failed));
}

void pool_state::submit(reservation reservation_value, std::string name, std::shared_ptr<operation_state> operation,
                        std::function<void(context&)>&& work) {
   const auto id = reservation_value.id();
   // Allocate the complete queue node BEFORE publication or consuming the
   // reservation. Thereafter list splice/dispatch rollback cannot allocate.
   auto incoming = std::list<impl::job>{};
   incoming.push_back(impl::job{
       .id = id, .name = std::move(name), .operation = operation, .queued_at = std::chrono::steady_clock::now()});
   // Unlike moving std::function (whose source is valid but unspecified), swap
   // proves the initiating coroutine holds NO remaining callable copy at publish.
   incoming.front().work.swap(work);
   auto failed = std::list<impl::job>{};
   auto owner = shared_from_this();
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->stopping) {
         ++impl_->current_metrics.rejected;
         throw exceptions::rejected{"compute pool is stopped"};
      }
      if (impl_->reservations.erase(id) == 0) {
         throw exceptions::internal{"compute admission reservation is invalid"};
      }
      reservation_value.consume();
      ++impl_->current_metrics.submitted;
      if (operation->stop_requested()) {
         incoming.front().dispatch_error = operation->cancellation_error();
         incoming.front().canceled_before_start = true;
         ++impl_->retiring;
         failed.splice(failed.end(), incoming);
      } else {
         impl_->pending.splice(impl_->pending.end(), incoming);
         impl_->start_jobs_locked(owner, failed);
      }
   }
   impl_->retire_jobs(owner, std::move(failed));
}

bool pool_state::cancel(std::uint64_t id) noexcept {
   auto retired = std::list<impl::job>{};
   auto found_running = false;
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      const auto found = std::ranges::find_if(impl_->pending, [id](const impl::job& value) { return value.id == id; });
      if (found != impl_->pending.end() && !found->drain) {
         found->dispatch_error = found->operation->cancellation_error();
         found->canceled_before_start = true;
         retired.splice(retired.end(), impl_->pending, found);
         ++impl_->retiring;
         impl_->current_metrics.pending = impl_->pending.size();
      } else {
         found_running =
             found != impl_->pending.end() ||
             std::ranges::any_of(impl_->running_operations, [id](const impl::job& value) { return value.id == id; });
      }
   }
   const auto canceled = !retired.empty();
   impl_->retire_jobs(shared_from_this(), std::move(retired));
   return canceled || found_running;
}

void pool_state::request_stop() noexcept {
   auto waiting = std::list<std::shared_ptr<admission_waiter>>{};
   auto pending = std::list<impl::job>{};
   auto drain = std::vector<std::shared_ptr<forge::asio::detail::async_waiter>>{};
   auto stop_limit = std::uint64_t{};
   {
      const auto lock = std::scoped_lock{impl_->mutex};
      if (impl_->stopping) {
         return;
      }
      impl_->stopping = true;
      impl_->current_metrics.stopped = true;
      stop_limit = impl_->next_id.load(std::memory_order_relaxed);

      for (auto& waiter : impl_->admission_waiters) {
         waiter->state = admission_state::rejected;
      }
      waiting.splice(waiting.end(), impl_->admission_waiters);
      impl_->current_metrics.rejected += waiting.size();
      impl_->admission_waiters.clear();
      impl_->current_metrics.waiting = 0;

      for (auto current = impl_->pending.begin(); current != impl_->pending.end();) {
         const auto next = std::next(current);
         if (!current->drain)
            pending.splice(pending.end(), impl_->pending, current);
         current = next;
      }
      impl_->retiring += pending.size();
      impl_->current_metrics.pending = impl_->pending.size();

      if (impl_->drained_locked()) {
         drain.swap(impl_->drain_waiters);
      }
   }

   for (const auto& waiter : waiting) {
      waiter->wake();
   }
   for (auto& value : pending) {
      value.operation->request_stop_only();
      value.dispatch_error = value.operation->cancellation_error();
      value.canceled_before_start = true;
   }
   // Finite snapshot walk without allocating or invoking stop callbacks under
   // the pool mutex. Later mandatory drains cannot extend this snapshot walk.
   auto previous = std::uint64_t{0};
   for (;;) {
      auto operation = std::shared_ptr<operation_state>{};
      auto next = std::uint64_t{0};
      {
         const auto lock = std::scoped_lock{impl_->mutex};
         for (const auto& value : impl_->running_operations) {
            if (value.id > previous && value.id < stop_limit && (next == 0 || value.id < next)) {
               next = value.id;
               operation = value.operation;
            }
         }
      }
      if (!operation) {
         break;
      }
      operation->request_stop_only();
      previous = next;
   }
   previous = 0;
   for (;;) {
      auto retained = std::shared_ptr<continuation_state>{};
      {
         const auto lock = std::scoped_lock{impl_->mutex};
         auto current = impl_->retained.upper_bound(previous);
         for (; current != impl_->retained.end(); ++current) {
            previous = current->first;
            retained = current->second.lock();
            if (retained)
               break;
         }
      }
      if (!retained)
         break;
      retained->request_stop();
   }
   impl_->retire_jobs(shared_from_this(), std::move(pending));
   for (const auto& waiter : drain) {
      waiter->wake();
   }

   // Native jobs and retained admissions stay owned through physical retirement.
   impl_->drained_cv.notify_all();
}

boost::asio::awaitable<void> pool_state::shutdown() {
   request_stop();
   co_await impl_->wait_for_drain();
   impl_->finalize();
}

void pool_state::shutdown_sync() noexcept {
   request_stop();
   {
      auto lock = std::unique_lock{impl_->mutex};
      impl_->drained_cv.wait(lock, [this] { return impl_->drained_locked(); });
   }
   try {
      impl_->finalize();
   } catch (...) {
      // Destructors cannot report hook failures.
   }
}

pool_state::metrics_snapshot pool_state::snapshot() const {
   const auto lock = std::scoped_lock{impl_->mutex};
   auto value = impl_->current_metrics;
   value.waiting = impl_->admission_waiters.size();
   value.pending = impl_->pending.size();
   value.running = impl_->running_operations.size();
   value.stopped = impl_->stopping;
   value.retained = impl_->retained.size();
   return value;
}

} // namespace detail

continuation::continuation(std::shared_ptr<detail::continuation_state> state) : state_{std::move(state)} {}
boost::asio::awaitable<void> continuation::drain_phase_owned(std::shared_ptr<detail::continuation_state> state,
                                                             std::shared_ptr<detail::operation_state> result,
                                                             std::function<void(context&)> invoke) {
   co_await result->execute({}, state->name(), invoke, state, true);
}
bool continuation::valid() const noexcept {
   return state_ != nullptr;
}
std::uint64_t continuation::id() const noexcept {
   return state_ ? state_->id() : 0;
}
bool continuation::stop_requested() const noexcept {
   return stop_token().stop_requested();
}
std::stop_token continuation::stop_token() const noexcept {
   return state_ ? state_->stop_token() : std::stop_token{};
}
void continuation::release() noexcept {
   state_.reset();
}

boost::asio::awaitable<continuation> executor::retain(task_options options) const {
   require_valid();
   return retain_owned(state_, std::move(options));
}

boost::asio::awaitable<continuation> executor::retain_owned(std::shared_ptr<detail::pool_state> owner,
                                                            task_options options) {
   auto admission = co_await owner->reserve(options.parent_stop_token);
   co_return continuation{owner->retain(std::move(admission), std::move(options.name), options.parent_stop_token)};
}

pool::pool() : pool(options{}) {}

pool::pool(options options_value)
    : state_{std::make_shared<detail::pool_state>(detail::pool_state::configuration{
          .worker_threads = options_value.worker_threads,
          .max_pending_tasks = options_value.max_pending_tasks,
          .max_waiting_submissions = options_value.max_waiting_submissions,
          .thread_name = std::move(options_value.thread_name),
          .on_worker_start = std::move(options_value.on_worker_start),
          .on_worker_stop = std::move(options_value.on_worker_stop),
      })} {}

pool::~pool() {
   if (state_ != nullptr) {
      state_->shutdown_sync();
   }
}

executor pool::get_executor() const noexcept {
   return executor{state_};
}

metrics pool::snapshot() const {
   return state_->snapshot();
}

void pool::request_stop() noexcept {
   state_->request_stop();
}

namespace {

boost::asio::awaitable<void> shutdown_owned(std::shared_ptr<detail::pool_state> state) {
   co_await state->shutdown();
}

} // namespace

boost::asio::awaitable<void> pool::shutdown() {
   return shutdown_owned(state_);
}

} // namespace forge::asio::compute
