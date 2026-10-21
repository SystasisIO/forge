module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <utility>

module forge.api.p2p.publication;

import forge.asio.notification;

#include "details/publication_state.hxx"

namespace forge::api::p2p::detail {

publication_state::publication_state(boost::asio::any_io_executor owner_executor, close_handler close,
                                     drain_handler drain, active_handler active)
    : owner_executor_{std::move(owner_executor)}, close_{std::move(close)}, drain_{std::move(drain)},
      active_{std::move(active)} {}

publication_state::~publication_state() = default;

std::stop_token publication_state::stop_token() const noexcept { return stop_.get_token(); }

void publication_state::bind_native(std::stop_token native) {
   if (native.stop_possible()) {
      native_stop_ = std::make_unique<std::stop_callback<std::function<void()>>>(native,
         [weak = weak_from_this()]() noexcept {
            if (const auto self = weak.lock()) { self->stop_.request_stop(); }
         });
   }
}

bool publication_state::active() const noexcept {
   if (stop_.stop_requested()) { return false; }
   auto callback = active_handler{};
   try {
      {
         const auto lock = std::scoped_lock{mutex_};
         if (close_requested_) {
            return false;
         }
         callback = active_;
      }
      if (!callback || !callback()) {
         return false;
      }
      const auto lock = std::scoped_lock{mutex_};
      return !close_requested_ && !stop_.stop_requested();
   } catch (...) {
      return false;
   }
}

void publication_state::close() noexcept {
   auto callback = close_handler{};
   {
      const auto lock = std::scoped_lock{mutex_};
      if (close_requested_) {
         return;
      }
      close_requested_ = true;
      callback = std::move(close_);
   }

   // Callbacks may synchronously re-enter or destroy the public facade.
   stop_.request_stop();

   auto failure = std::exception_ptr{};
   try {
      if (callback) {
         callback();
      }
   } catch (...) {
      failure = std::current_exception();
   }
   finish_close(std::move(failure));
}

boost::asio::awaitable<void> publication_state::async_close() {
   close();
   co_await wait_for_close();

   auto drain = drain_handler{};
   auto launch = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      if (!drain_started_) {
         drain_started_ = true;
         drain = std::move(drain_);
         launch = true;
      }
   }
   if (launch) {
      launch_drain(std::move(drain));
   }
   co_await wait_for_drain();
   co_return;
}

boost::asio::awaitable<void> publication_state::wait_for_close() {
   for (;;) {
      const auto observed = close_ready_.epoch();
      {
         const auto lock = std::scoped_lock{mutex_};
         if (close_finished_) {
            co_return;
         }
      }
      co_await close_ready_.async_wait(observed);
   }
}

boost::asio::awaitable<void> publication_state::wait_for_drain() {
   for (;;) {
      const auto observed = drain_ready_.epoch();
      auto failure = std::exception_ptr{};
      auto finished = false;
      {
         const auto lock = std::scoped_lock{mutex_};
         finished = drain_finished_;
         if (finished) {
            failure = close_failure_ ? close_failure_ : drain_failure_;
         }
      }
      if (failure) {
         std::rethrow_exception(failure);
      }
      if (finished) {
         co_return;
      }
      co_await drain_ready_.async_wait(observed);
   }
}

void publication_state::launch_drain(drain_handler drain) noexcept {
   if (!drain) {
      finish_drain({});
      return;
   }
   try {
      boost::asio::co_spawn(
         owner_executor_,
         [self = shared_from_this(), drain = std::move(drain)]() mutable -> boost::asio::awaitable<void> {
            auto failure = std::exception_ptr{};
            try {
               co_await drain();
            } catch (...) {
               failure = std::current_exception();
            }
            self->finish_drain(std::move(failure));
            co_return;
         },
         boost::asio::detached);
   } catch (...) {
      finish_drain(std::current_exception());
   }
}

void publication_state::finish_close(std::exception_ptr failure) noexcept {
   {
      const auto lock = std::scoped_lock{mutex_};
      close_failure_ = std::move(failure);
      close_finished_ = true;
   }
   close_ready_.notify();
}

void publication_state::finish_drain(std::exception_ptr failure) noexcept {
   {
      const auto lock = std::scoped_lock{mutex_};
      if (drain_finished_) {
         return;
      }
      drain_failure_ = std::move(failure);
      drain_finished_ = true;
   }
   drain_ready_.notify();
}

std::shared_ptr<publication_state> make_publication_state(
   boost::asio::any_io_executor owner_executor,
   publication_state::close_handler close,
   publication_state::drain_handler drain,
   publication_state::active_handler active, std::stop_token native) {
   auto state = std::make_shared<publication_state>(
      std::move(owner_executor), std::move(close), std::move(drain), std::move(active));
   state->bind_native(native);
   return state;
}

bool publication_active(std::shared_ptr<publication_state> state) noexcept {
   return state && state->active();
}

std::stop_token publication_stop_token(std::shared_ptr<publication_state> state) noexcept {
   return state ? state->stop_token() : std::stop_token{};
}

void close_publication(std::shared_ptr<publication_state> state) noexcept {
   if (state) {
      state->close();
   }
}

boost::asio::awaitable<void> async_close_publication(
   std::shared_ptr<publication_state> state) {
   if (state) {
      co_await state->async_close();
   }
   co_return;
}

} // namespace forge::api::p2p::detail
