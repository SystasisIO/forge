module;

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.asio.notification;
import forge.net.p2p.lifecycle;

#include "details/lifecycle_tracker.hxx"
#include "details/lifecycle_wakeup.hxx"

namespace forge::net::p2p::detail {

std::shared_ptr<lifecycle_stop_source> lifecycle_stop_source::create() {
   return std::shared_ptr<lifecycle_stop_source>{new lifecycle_stop_source{}};
}

bool lifecycle_stop_source::stop_requested() const noexcept {
   return stop_requested_.load(std::memory_order_acquire);
}

lifecycle_stop_subscription lifecycle_stop_source::subscribe(std::weak_ptr<lifecycle_stop_listener> listener) {
   auto value = std::make_shared<observer>();
   value->listener = std::move(listener);
   auto notify = false;
   {
      const auto lock = std::scoped_lock{mutex_};
      if (stop_requested_.load(std::memory_order_acquire)) {
         notify = true;
      } else {
         observers_.push_back(value);
      }
   }
   if (notify) {
      if (const auto current = value->listener.lock()) {
         current->request_lifecycle_stop();
      }
      return {};
   }
   return lifecycle_stop_subscription{shared_from_this(), std::move(value)};
}

void lifecycle_stop_source::unsubscribe(const std::shared_ptr<observer>& value) noexcept {
   try {
      const auto lock = std::scoped_lock{mutex_};
      std::erase(observers_, value);
   } catch (...) {
   }
}

void lifecycle_stop_source::request_stop() noexcept {
   auto observers = std::vector<std::shared_ptr<observer>>{};
   try {
      const auto lock = std::scoped_lock{mutex_};
      if (stop_requested_.exchange(true, std::memory_order_acq_rel)) {
         return;
      }
      observers.swap(observers_);
   } catch (...) {
      stop_requested_.store(true, std::memory_order_release);
      return;
   }
   for (const auto& observer : observers) {
      if (observer) {
         if (const auto listener = observer->listener.lock()) {
            listener->request_lifecycle_stop();
         }
      }
   }
}

lifecycle_stop_subscription::lifecycle_stop_subscription(
    std::shared_ptr<lifecycle_stop_source> source,
    std::shared_ptr<lifecycle_stop_source::observer> observer)
    : source_{std::move(source)}, observer_{std::move(observer)} {}

lifecycle_stop_subscription::lifecycle_stop_subscription(lifecycle_stop_subscription&& other) noexcept
    : source_{std::move(other.source_)}, observer_{std::move(other.observer_)} {}

lifecycle_stop_subscription& lifecycle_stop_subscription::operator=(lifecycle_stop_subscription&& other) noexcept {
   if (this != &other) {
      reset();
      source_ = std::move(other.source_);
      observer_ = std::move(other.observer_);
   }
   return *this;
}

lifecycle_stop_subscription::~lifecycle_stop_subscription() {
   reset();
}

void lifecycle_stop_subscription::reset() noexcept {
   if (source_ && observer_) {
      source_->unsubscribe(observer_);
   }
   source_.reset();
   observer_.reset();
}

lifecycle_tracker::state::operation_context::operation_context(boost::asio::any_io_executor executor)
    : strand{boost::asio::make_strand(std::move(executor))} {}

lifecycle_tracker::state::state(boost::asio::any_io_executor executor_value)
    : executor{std::move(executor_value)}, stop_source{lifecycle_stop_source::create()},
      changed{std::make_shared<lifecycle_wakeup>()} {}

void lifecycle_tracker::state::release(std::uint64_t id) noexcept {
   try {
      auto notify = false;
      {
         const auto lock = std::scoped_lock{mutex};
         operations.erase(id);
         notify = stop_requested && operations.empty();
      }
      if (notify) {
         changed->notify();
      }
   } catch (...) {
      // Operation destruction must remain noexcept during process teardown.
   }
}

lifecycle_tracker::operation::operation(std::shared_ptr<state> state_value, std::uint64_t id,
                                        std::shared_ptr<state::operation_context> context)
    : state_{std::move(state_value)}, id_{id}, context_{std::move(context)} {}

lifecycle_tracker::operation::operation(operation&& other) noexcept
    : state_{std::move(other.state_)}, id_{std::exchange(other.id_, 0)}, context_{std::move(other.context_)} {}

lifecycle_tracker::operation& lifecycle_tracker::operation::operator=(operation&& other) noexcept {
   if (this != &other) {
      release();
      state_ = std::move(other.state_);
      id_ = std::exchange(other.id_, 0);
      context_ = std::move(other.context_);
   }
   return *this;
}

lifecycle_tracker::operation::~operation() {
   release();
}

bool lifecycle_tracker::operation::active() const noexcept {
   return state_ != nullptr;
}

boost::asio::any_io_executor lifecycle_tracker::operation::executor() const noexcept {
   return context_ ? boost::asio::any_io_executor{context_->strand} : boost::asio::any_io_executor{};
}

std::shared_ptr<lifecycle_stop_source> lifecycle_tracker::operation::stop_source() const noexcept {
   return state_ ? state_->stop_source : nullptr;
}

void lifecycle_tracker::operation::release() noexcept {
   context_.reset();
   if (auto state = std::move(state_)) {
      state->release(std::exchange(id_, 0));
   }
}

lifecycle_tracker::lifecycle_tracker(boost::asio::any_io_executor executor)
    : state_{std::make_shared<state>(std::move(executor))} {}

bool lifecycle_tracker::begin_start() noexcept {
   const auto lock = std::scoped_lock{state_->mutex};
   if (state_->stop_requested || state_->phase != lifecycle_phase::idle) {
      return false;
   }
   state_->phase = lifecycle_phase::hydrating;
   return true;
}

void lifecycle_tracker::set_phase(lifecycle_phase value) noexcept {
   const auto lock = std::scoped_lock{state_->mutex};
   if (!state_->stop_requested) {
      state_->phase = value;
   }
}

lifecycle_phase lifecycle_tracker::phase() const noexcept {
   const auto lock = std::scoped_lock{state_->mutex};
   return state_->phase;
}

bool lifecycle_tracker::stop_requested() const noexcept {
   return state_->stop_source->stop_requested();
}

lifecycle_tracker::operation lifecycle_tracker::track() noexcept {
   try {
      const auto context = std::make_shared<state::operation_context>(state_->executor);
      const auto lock = std::scoped_lock{state_->mutex};
      if (state_->stop_requested) {
         return {};
      }
      const auto id = state_->next_operation_id++;
      state_->operations.emplace(id, context);
      return operation{state_, id, context};
   } catch (...) {
      return {};
   }
}

lifecycle_stop_subscription
lifecycle_tracker::subscribe_stop(const std::shared_ptr<lifecycle_stop_source>& source,
                                  std::weak_ptr<lifecycle_stop_listener> listener) {
   return source ? source->subscribe(std::move(listener)) : lifecycle_stop_subscription{};
}

void lifecycle_tracker::request_stop() noexcept {
   try {
      auto stop_source = std::shared_ptr<lifecycle_stop_source>{};
      {
         const auto lock = std::scoped_lock{state_->mutex};
         if (state_->stop_requested) {
            return;
         }
         state_->stop_requested = true;
         state_->phase = lifecycle_phase::stopping;
         stop_source = state_->stop_source;
      }
      stop_source->request_stop();
      state_->changed->notify();
   } catch (...) {
      // Node-level resource teardown remains the final shutdown fallback.
   }
}

boost::asio::awaitable<void> lifecycle_tracker::wait() const {
   co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
   auto observed = state_->changed->epoch();
   while (true) {
      {
         const auto lock = std::scoped_lock{state_->mutex};
         if (state_->stop_requested && state_->operations.empty()) {
            co_return;
         }
      }
      observed = co_await state_->changed->async_wait(observed);
   }
}

void lifecycle_tracker::finish_stop() noexcept {
   try {
      auto stop_source = std::shared_ptr<lifecycle_stop_source>{};
      {
         const auto lock = std::scoped_lock{state_->mutex};
         state_->stop_requested = true;
         state_->phase = lifecycle_phase::stopped;
         stop_source = state_->stop_source;
      }
      stop_source->request_stop();
      state_->changed->notify();
   } catch (...) {
      // Final teardown must remain noexcept.
   }
}

} // namespace forge::net::p2p::detail
