module;

#include "details/async_waiter.hxx"
#include "details/stop_state.hxx"
#include "details/thread_name.hxx"

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>
#include <vector>

module forge.asio.affine;

namespace forge::asio::affine::detail {

struct lane_state::impl {
   explicit impl(configuration value)
       : configuration{std::move(value)}, worker{std::make_unique<boost::asio::thread_pool>(1)} {}

   configuration configuration;
   std::unique_ptr<boost::asio::thread_pool> worker;
   mutable std::mutex mutex;
   std::condition_variable drained_cv;
   std::shared_ptr<operation_state> active;
   std::deque<std::shared_ptr<operation_state>> pending;
   std::deque<std::shared_ptr<operation_state>> waiting;
   std::vector<std::function<void()>> drain_waiters;
   forge::asio::affine::metrics metrics;
   forge::asio::detail::stop_state stop;
   std::mutex finalize_mutex;
   bool finalized = false;
};

operation_state::operation_state(std::shared_ptr<lane_state> owner_value, std::string name_value)
    : owner{std::move(owner_value)}, name{std::move(name_value)} {}

operation_state::~operation_state() = default;

bool operation_state::cancel_before_start() noexcept {
   if (auto state = owner.lock()) {
      return state->cancel_before_start(shared_from_this());
   }
   return false;
}

void operation_cancellation_handler::operator()(boost::asio::cancellation_type_t type) const noexcept {
   if (type != boost::asio::cancellation_type::none) {
      if (auto current = operation.lock()) {
         static_cast<void>(current->cancel_before_start());
      }
   }
}

bool operation_state::complete_value() noexcept {
   auto wake = std::function<void()>{};
   {
      const auto lock = std::scoped_lock{completion_mutex_};
      if (completed_) {
         return false;
      }
      completed_ = true;
      wake = std::move(wake_);
   }
   if (wake) {
      wake();
   }
   return true;
}

bool operation_state::complete_exception(std::exception_ptr error) noexcept {
   auto wake = std::function<void()>{};
   {
      const auto lock = std::scoped_lock{completion_mutex_};
      if (completed_) {
         return false;
      }
      error_ = std::move(error);
      completed_ = true;
      wake = std::move(wake_);
   }
   if (wake) {
      wake();
   }
   return true;
}

boost::asio::awaitable<void> operation_state::wait() {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto waiter = std::make_shared<forge::asio::detail::async_waiter>(executor);
   auto self = shared_from_this();

   {
      const auto lock = std::scoped_lock{completion_mutex_};
      if (wait_started_) {
         throw exceptions::invalid_state{"affine operation can only be awaited once"};
      }
      wait_started_ = true;
      if (completed_) {
         if (error_) {
            std::rethrow_exception(error_);
         }
         co_return;
      }
      wake_ = [waiter] { waiter->wake(); };
   }

   if (auto state = owner.lock()) {
      state->submit(self);
   } else {
      complete_exception(std::make_exception_ptr(exceptions::invalid_state{"affine lane is unavailable"}));
   }

   for (;;) {
      static_cast<void>(co_await waiter->wait());

      auto error = std::exception_ptr{};
      auto completed = false;
      {
         const auto lock = std::scoped_lock{completion_mutex_};
         completed = completed_;
         error = error_;
         if (completed) {
            wake_ = {};
         }
      }
      if (completed) {
         if (error) {
            std::rethrow_exception(error);
         }
         co_return;
      }
      throw exceptions::internal{"affine operation woke without completion"};
   }
}

lane_state::lane_state(configuration configuration) : impl_{std::make_unique<impl>(std::move(configuration))} {}

lane_state::~lane_state() {
   shutdown_sync();
}

boost::asio::awaitable<void> lane_state::execute(std::shared_ptr<operation_state> operation) {
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   auto slot = cancellation.slot();
   if (slot.is_connected())
      slot.assign(operation_cancellation_handler{operation});
   const auto clear_slot = boost::scope::scope_exit{[&slot] {
      if (slot.is_connected())
         slot.clear();
   }};
   if (cancellation.cancelled() != boost::asio::cancellation_type::none)
      static_cast<void>(operation->cancel_before_start());
   // No admission has occurred yet. A caller await_transform cancellation
   // check may unwind safely before child initiation. After initiation only
   // the disconnected child admits/waits, on this SAME executor, to terminal
   // completion. Caller filters, history and throw policy are never reset.
   const auto current = co_await boost::asio::this_coro::executor;
   co_await boost::asio::co_spawn(
       current, operation->wait(),
       boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
   // The child is void; no default-construction constraint on typed results.
}

void lane_state::submit(const std::shared_ptr<operation_state>& operation) {
   auto& data = *impl_;
   auto post = std::shared_ptr<operation_state>{};
   auto reject = false;
   auto stopped_rejection = false;
   {
      const auto lock = std::scoped_lock{data.mutex};
      if (operation->phase == operation_phase::canceled) {
         return;
      }
      if (data.stop.stop_requested()) {
         ++data.metrics.rejected;
         reject = true;
         stopped_rejection = true;
      } else {
         operation->queued_at = std::chrono::steady_clock::now();
         if (data.active == nullptr) {
            ++data.metrics.submitted;
            operation->phase = operation_phase::dispatched;
            data.active = operation;
            data.metrics.running = 1;
            post = operation;
         } else if (data.pending.size() < data.configuration.max_pending_operations) {
            ++data.metrics.submitted;
            operation->phase = operation_phase::pending;
            data.pending.push_back(operation);
            data.metrics.pending = data.pending.size();
         } else if (data.waiting.size() < data.configuration.max_waiting_submissions) {
            ++data.metrics.submitted;
            operation->phase = operation_phase::waiting;
            data.waiting.push_back(operation);
            data.metrics.waiting = data.waiting.size();
         } else {
            ++data.metrics.rejected;
            reject = true;
         }
      }
      if (reject) {
         operation->phase = operation_phase::rejected;
      }
   }

   if (reject) {
      operation->complete_exception(std::make_exception_ptr(exceptions::rejected{
         stopped_rejection ? "affine lane is stopped" : "affine submission wait queue is full"}));
   }
   if (post != nullptr) {
      post_operation(post);
   }
}

bool lane_state::cancel_before_start(const std::shared_ptr<operation_state>& operation) noexcept {
   auto& data = *impl_;
   auto canceled = false;
   {
      const auto lock = std::scoped_lock{data.mutex};
      if (operation->phase == operation_phase::waiting) {
         const auto found = std::ranges::find(data.waiting, operation);
         if (found != data.waiting.end()) {
            data.waiting.erase(found);
            operation->phase = operation_phase::canceled;
            data.metrics.waiting = data.waiting.size();
            ++data.metrics.canceled;
            canceled = true;
         } else {
            operation->phase = operation_phase::canceled;
            ++data.metrics.canceled;
            canceled = true;
         }
      } else if (operation->phase == operation_phase::pending) {
         const auto found = std::ranges::find(data.pending, operation);
         if (found != data.pending.end()) {
            data.pending.erase(found);
         }
         operation->phase = operation_phase::canceled;
         promote_waiters_locked();
         data.metrics.pending = data.pending.size();
         data.metrics.waiting = data.waiting.size();
         ++data.metrics.canceled;
         canceled = true;
      } else if (operation->phase == operation_phase::dispatched) {
         operation->phase = operation_phase::canceled;
         ++data.metrics.canceled;
         canceled = true;
      }
   }

   if (canceled) {
      operation->complete_exception(
         std::make_exception_ptr(exceptions::canceled{"affine operation was canceled before execution"}));
   }
   return canceled;
}

void lane_state::post_operation(const std::shared_ptr<operation_state>& operation) {
   auto& data = *impl_;
   auto self = shared_from_this();
   boost::asio::post(data.worker->get_executor(), [self = std::move(self), operation] {
      forge::asio::detail::set_current_thread_name(self->impl_->configuration.thread_name);
      self->run_operation(operation);
   });
}

void lane_state::run_operation(const std::shared_ptr<operation_state>& operation) noexcept {
   auto& data = *impl_;
   auto invoke = false;
   {
      const auto lock = std::scoped_lock{data.mutex};
      if (data.active == operation && operation->phase == operation_phase::dispatched) {
         operation->phase = operation_phase::running;
         data.metrics.total_queue_time += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - operation->queued_at);
         invoke = true;
      }
   }

   auto error = std::exception_ptr{};
   const auto started_at = std::chrono::steady_clock::now();
   if (invoke) {
      try {
         operation->run();
      } catch (...) {
         error = std::current_exception();
      }
   }
   const auto execution_time = invoke
                                  ? std::chrono::duration_cast<std::chrono::nanoseconds>(
                                       std::chrono::steady_clock::now() - started_at)
                                  : std::chrono::nanoseconds{};
   finish_operation(operation, std::move(error), invoke, execution_time);
}

void lane_state::finish_operation(const std::shared_ptr<operation_state>& operation,
                                  std::exception_ptr error,
                                  bool invoked,
                                  std::chrono::nanoseconds execution_time) noexcept {
   auto& data = *impl_;
   auto next = std::shared_ptr<operation_state>{};
   auto drain = std::vector<std::function<void()>>{};
   {
      const auto lock = std::scoped_lock{data.mutex};
      if (data.active != operation) {
         return;
      }
      if (invoked) {
         operation->phase = operation_phase::completed;
         data.metrics.total_execution_time += execution_time;
         if (error) {
            ++data.metrics.failed;
         } else {
            ++data.metrics.completed;
         }
      }
      data.active.reset();
      data.metrics.running = 0;
      if (!data.stop.stop_requested()) {
         next = select_next_locked();
      }
      if (drained_locked()) {
         drain.swap(data.drain_waiters);
      }
   }

   if (invoked) {
      if (error) {
         operation->complete_exception(std::move(error));
      } else {
         operation->complete_value();
      }
   }
   if (next != nullptr) {
      post_operation(next);
   }
   for (const auto& wake : drain) {
      wake();
   }
   data.drained_cv.notify_all();
}

std::shared_ptr<operation_state> lane_state::select_next_locked() {
   auto& data = *impl_;
   auto next = std::shared_ptr<operation_state>{};
   if (!data.pending.empty()) {
      next = std::move(data.pending.front());
      data.pending.pop_front();
   } else if (!data.waiting.empty()) {
      next = std::move(data.waiting.front());
      data.waiting.pop_front();
   }
   if (next != nullptr) {
      next->phase = operation_phase::dispatched;
      data.active = next;
      data.metrics.running = 1;
   }
   promote_waiters_locked();
   data.metrics.pending = data.pending.size();
   data.metrics.waiting = data.waiting.size();
   return next;
}

void lane_state::promote_waiters_locked() {
   auto& data = *impl_;
   while (data.pending.size() < data.configuration.max_pending_operations && !data.waiting.empty()) {
      auto next = std::move(data.waiting.front());
      data.waiting.pop_front();
      next->phase = operation_phase::pending;
      data.pending.push_back(std::move(next));
   }
}

bool lane_state::drained_locked() const noexcept {
   return impl_->active == nullptr && impl_->pending.empty() && impl_->waiting.empty();
}

void lane_state::request_stop() noexcept {
   auto& data = *impl_;
   if (!data.stop.request_stop()) {
      return;
   }
   auto canceled = std::vector<std::shared_ptr<operation_state>>{};
   auto rejected = std::vector<std::shared_ptr<operation_state>>{};
   auto drain = std::vector<std::function<void()>>{};
   {
      const auto lock = std::scoped_lock{data.mutex};
      data.metrics.stopped = true;

      for (auto& operation : data.waiting) {
         operation->phase = operation_phase::rejected;
         rejected.push_back(operation);
      }
      data.waiting.clear();
      data.metrics.rejected += rejected.size();
      data.metrics.waiting = 0;

      for (auto& operation : data.pending) {
         operation->phase = operation_phase::canceled;
         canceled.push_back(operation);
      }
      data.pending.clear();
      data.metrics.canceled += canceled.size();
      data.metrics.pending = 0;

      if (data.active != nullptr && data.active->phase == operation_phase::dispatched) {
         data.active->phase = operation_phase::canceled;
         canceled.push_back(data.active);
         ++data.metrics.canceled;
      }
      if (drained_locked()) {
         drain.swap(data.drain_waiters);
      }
   }

   for (const auto& operation : rejected) {
      operation->complete_exception(
         std::make_exception_ptr(exceptions::rejected{"affine lane stopped before admission"}));
   }
   for (const auto& operation : canceled) {
      operation->complete_exception(
         std::make_exception_ptr(exceptions::canceled{"affine lane stopped before execution"}));
   }
   for (const auto& wake : drain) {
      wake();
   }
   data.drained_cv.notify_all();
}

boost::asio::awaitable<void> lane_state::wait_for_drain() {
   auto& data = *impl_;
   const auto executor = co_await boost::asio::this_coro::executor;
   auto waiter = std::make_shared<forge::asio::detail::async_waiter>(executor);
   {
      const auto lock = std::scoped_lock{data.mutex};
      if (drained_locked()) {
         co_return;
      }
      data.drain_waiters.push_back([waiter] { waiter->wake(); });
   }
   static_cast<void>(co_await waiter->wait());
}

void lane_state::finalize() {
   auto& data = *impl_;
   const auto lock = std::scoped_lock{data.finalize_mutex};
   if (data.finalized) {
      return;
   }
   data.worker->stop();
   data.worker->join();
   data.finalized = true;
}

boost::asio::awaitable<void> lane_state::shutdown() {
   request_stop();
   co_await wait_for_drain();
   finalize();
}

void lane_state::shutdown_sync() noexcept {
   auto& data = *impl_;
   request_stop();
   {
      auto lock = std::unique_lock{data.mutex};
      data.drained_cv.wait(lock, [this] { return drained_locked(); });
   }
   try {
      finalize();
   } catch (...) {
      // Destructors cannot report native worker failures.
   }
}

forge::asio::affine::metrics lane_state::snapshot() const {
   const auto lock = std::scoped_lock{impl_->mutex};
   auto value = impl_->metrics;
   value.waiting = impl_->waiting.size();
   value.pending = impl_->pending.size();
   value.running = impl_->active == nullptr ? 0U : 1U;
   value.stopped = impl_->stop.stop_requested();
   return value;
}

} // namespace forge::asio::affine::detail

namespace forge::asio::affine {

lane::lane() : lane(options{}) {}

lane::lane(options options_value)
    : state_{std::make_shared<detail::lane_state>(detail::lane_state::configuration{
          .max_pending_operations = options_value.max_pending_operations,
          .max_waiting_submissions = options_value.max_waiting_submissions,
          .thread_name = std::move(options_value.thread_name),
       })} {}

lane::~lane() {
   if (state_ != nullptr) {
      state_->shutdown_sync();
   }
}

executor lane::get_executor() const noexcept {
   return executor{state_};
}

metrics lane::snapshot() const {
   return state_->snapshot();
}

void lane::request_stop() noexcept {
   state_->request_stop();
}

namespace {

boost::asio::awaitable<void> shutdown_owned(std::shared_ptr<detail::lane_state> state) {
   // Stop admission and terminal worker drain begin only inside this owned
   // isolated child, never before a possible parent await_transform failure.
   const auto current = co_await boost::asio::this_coro::executor;
   co_await boost::asio::co_spawn(
       current, state->shutdown(),
       boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
}

} // namespace

boost::asio::awaitable<void> lane::shutdown() {
   return shutdown_owned(state_);
}

} // namespace forge::asio::affine
