module;

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.asio.notification;
import forge.net.p2p.exceptions;
import forge.net.p2p.lifecycle;

#include "details/lifecycle_tracker.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p::detail {
namespace {

[[nodiscard]] bool is_stop_exception(const std::exception_ptr& error) noexcept {
   if (!error) {
      return false;
   }
   try {
      std::rethrow_exception(error);
   } catch (const boost::system::system_error& value) {
      return value.code() == boost::asio::error::operation_aborted;
   } catch (const forge::exceptions::base& value) {
      return exceptions::is(value, exceptions::code::canceled);
   } catch (...) {
      return false;
   }
}

} // namespace

struct worker_stop_bridge_run_state final {
   std::shared_ptr<worker_terminal_owner> terminal;
   forge::asio::notification completed;
   std::exception_ptr work_failure;
   std::exception_ptr stop_failure;
   std::size_t remaining = 0;
   bool stop_requested = false;
};

namespace {

void complete_branch(const std::shared_ptr<worker_stop_bridge_run_state>& state, std::exception_ptr error,
                     bool is_work_branch) noexcept {
   if (is_work_branch) {
      state->work_failure = std::move(error);
      state->terminal->seal();
   } else {
      state->stop_failure = std::move(error);
      if (state->stop_failure) {
         state->terminal->request_stop();
      }
   }
   if (state->remaining != 0) {
      --state->remaining;
   }
   if (state->remaining == 0) {
      state->completed.notify();
   }
}

boost::asio::awaitable<void> async_join_branches(const std::shared_ptr<worker_stop_bridge_run_state>& state) {
   while (state->remaining != 0) {
      const auto observed = state->completed.epoch();
      if (state->remaining != 0) {
         static_cast<void>(co_await state->completed.async_wait(observed));
      }
   }
}

} // namespace

struct worker_stop_bridge_state final : lifecycle_stop_listener {
   forge::asio::notification notification;
   std::atomic_bool stop_requested = false;

   void request_lifecycle_stop() noexcept override {
      stop_requested.store(true, std::memory_order_release);
      notification.notify();
   }
};

worker_stop_bridge::worker_stop_bridge() : state_{std::make_shared<worker_stop_bridge_state>()} {}

worker_terminal_owner::worker_terminal_owner(std::shared_ptr<worker_stop_bridge_state> stop_state)
    : stop_state_{std::move(stop_state)} {}

bool worker_terminal_owner::publish(callback cancel) noexcept {
   if (state_ == state::sealed || published_ || !cancel) {
      return false;
   }
   published_ = true;
   if (stop_state_->stop_requested.load(std::memory_order_acquire)) {
      state_ = state::stop_requested;
   }
   if (state_ == state::stop_requested) {
      invoked_ = true;
      cancel();
      return true;
   }
   cancel_.swap(cancel);
   return true;
}

void worker_terminal_owner::request_stop() noexcept {
   if (state_ == state::sealed || state_ == state::stop_requested) {
      return;
   }
   state_ = state::stop_requested;
   if (cancel_ && !invoked_) {
      invoked_ = true;
      auto cancel = callback{};
      cancel.swap(cancel_);
      cancel();
   }
}

void worker_terminal_owner::seal() noexcept {
   state_ = state::sealed;
   cancel_ = nullptr;
}

bool worker_terminal_owner::stop_requested() const noexcept {
   return state_ == state::stop_requested;
}

void worker_stop_bridge::request_stop() noexcept {
   state_->request_lifecycle_stop();
}

bool worker_stop_bridge::stop_requested() const noexcept {
   return state_->stop_requested.load(std::memory_order_acquire);
}

boost::asio::awaitable<void> async_run_with_stop_bridge(std::shared_ptr<worker_stop_bridge> stop,
                                                         worker_stop_work work,
                                                         worker_stop_bridge_options options) {
   namespace asio = boost::asio;

   const auto caller_executor = co_await asio::this_coro::executor;
   const auto worker_executor = asio::make_strand(caller_executor);
   const auto stop_state = stop->state_;
   auto lifecycle_subscription =
       lifecycle_tracker::subscribe_stop(options.lifecycle_stop, std::weak_ptr<lifecycle_stop_listener>{stop_state});
   if (stop_state->stop_requested.load(std::memory_order_acquire) ||
       (options.lifecycle_stop && options.lifecycle_stop->stop_requested())) {
      co_return;
   }
   const auto observed_stop = stop_state->notification.epoch();
   if (stop_state->stop_requested.load(std::memory_order_acquire) ||
       (options.lifecycle_stop && options.lifecycle_stop->stop_requested())) {
      co_return;
   }

   // The parent is the transaction owner. It publishes the stop waiter first,
   // then either publishes work or wakes and joins that waiter on every setup
   // failure. Only the worker strand accesses terminal callback state.
   const auto inherited_cancellation = co_await asio::this_coro::cancellation_state;
   if (inherited_cancellation.cancelled() != asio::cancellation_type::none) {
      stop->request_stop();
   }
   co_await asio::this_coro::reset_cancellation_state(
       [stop](asio::cancellation_type type) noexcept {
          if (type != asio::cancellation_type::none) {
             stop->request_stop();
          }
          return asio::cancellation_type::none;
       },
       asio::disable_cancellation{});
   co_await asio::co_spawn(
       worker_executor,
       [worker_executor, stop_state, observed_stop, work = std::move(work), options = std::move(options),
        lifecycle_subscription = std::move(lifecycle_subscription)]() mutable
           -> asio::awaitable<void> {
          static_cast<void>(lifecycle_subscription);
          auto state = std::make_shared<worker_stop_bridge_run_state>();
          state->terminal = std::make_shared<worker_terminal_owner>(stop_state);
          auto setup_failure = std::exception_ptr{};
          auto join_failure = std::exception_ptr{};

          ++state->remaining;
          try {
             asio::co_spawn(
                 worker_executor,
                 [state, stop_state, observed_stop, lifecycle_stop = options.lifecycle_stop,
                  before_stop_wait = std::move(options.before_stop_wait)]() mutable
                     -> asio::awaitable<void> {
                    if (before_stop_wait) {
                       before_stop_wait();
                    }
                    try {
                       static_cast<void>(co_await stop_state->notification.async_wait(observed_stop));
                    } catch (const boost::system::system_error& error) {
                       if (error.code() == asio::error::operation_aborted) {
                          co_return;
                       }
                       throw;
                    }
                    if (stop_state->stop_requested.load(std::memory_order_acquire) ||
                        (lifecycle_stop && lifecycle_stop->stop_requested())) {
                       state->stop_requested = true;
                       state->terminal->request_stop();
                    }
                 },
                 asio::bind_executor(worker_executor, [state](std::exception_ptr error) noexcept {
                    complete_branch(state, std::move(error), false);
                 }));
          } catch (...) {
             --state->remaining;
             throw;
          }

          if (!stop_state->stop_requested.load(std::memory_order_acquire) && !state->stop_requested &&
              !state->stop_failure) {
             auto work_reserved = false;
             try {
                if (options.before_work_spawn) {
                   options.before_work_spawn();
                }
                ++state->remaining;
                work_reserved = true;
                asio::co_spawn(
                    worker_executor,
                    [work = std::move(work), terminal = state->terminal,
                     lifecycle_stop = std::move(options.lifecycle_stop)]() mutable
                        -> asio::awaitable<void> {
                       if (lifecycle_stop && lifecycle_stop->stop_requested()) {
                          co_return;
                       }
                       co_await work(std::move(terminal));
                    },
                    asio::bind_executor(worker_executor, [state, stop_state](std::exception_ptr error) noexcept {
                       complete_branch(state, std::move(error), true);
                       // Wake the stop branch after terminal ownership is sealed.
                       stop_state->notification.notify();
                    }));
             } catch (...) {
                if (work_reserved) {
                   --state->remaining;
                }
                setup_failure = std::current_exception();
                stop_state->notification.notify();
             }
          }

          while (state->remaining != 0) {
             try {
                co_await async_join_branches(state);
             } catch (...) {
                if (!join_failure) {
                   join_failure = std::current_exception();
                }
                state->terminal->request_stop();
                stop_state->notification.notify();
             }
          }

          if (setup_failure) {
             std::rethrow_exception(setup_failure);
          }
          if (join_failure) {
             std::rethrow_exception(join_failure);
          }
          if (state->stop_failure) {
             std::rethrow_exception(state->stop_failure);
          }
          if (state->stop_requested) {
             if (state->work_failure && !is_stop_exception(state->work_failure)) {
                std::rethrow_exception(state->work_failure);
             }
             co_return;
          }
          if (state->work_failure) {
             std::rethrow_exception(state->work_failure);
          }
       },
       asio::use_awaitable);
}

} // namespace forge::net::p2p::detail
