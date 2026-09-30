module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.connection;
import forge.api.core.types;
import forge.api.transport.connection;
import forge.asio.notification;

#include "details/managed_remote_state.hxx"

extern "C++" {
namespace forge::plugins::net::p2p::resolver::detail {

managed_remote_state::reconnect_flight::reconnect_flight(boost::asio::any_io_executor executor)
    : executor_{boost::asio::make_strand(std::move(executor))} {}

const boost::asio::any_io_executor& managed_remote_state::reconnect_flight::executor() const noexcept {
   return executor_;
}

forge::asio::notification& managed_remote_state::reconnect_flight::completed() noexcept {
   return completed_;
}

forge::asio::notification& managed_remote_state::reconnect_flight::stop_changed() noexcept {
   return stop_changed_;
}

forge::asio::notification& managed_remote_state::reconnect_flight::cleanup_completed() noexcept {
   return cleanup_completed_;
}

boost::asio::cancellation_signal& managed_remote_state::reconnect_flight::cancellation() noexcept {
   return cancellation_;
}

managed_remote_state::timer_state::timer_state(boost::asio::any_io_executor executor, std::chrono::milliseconds delay)
    : timer_{std::move(executor), delay} {}

boost::asio::steady_timer& managed_remote_state::timer_state::timer() noexcept {
   return timer_;
}

managed_remote_state::managed_remote_state(std::size_t max_waiters) noexcept : max_waiters_{max_waiters} {}

managed_remote_state::acquisition managed_remote_state::acquire_or_join(boost::asio::any_io_executor executor) {
   auto lock = std::scoped_lock{mutex_};
   if (stopped_) {
      return acquisition{.status = acquire_status::stopped};
   }
   if (current_) {
      return acquisition{.status = acquire_status::current, .current = current_};
   }
   if (flight_ && flight_->done_) {
      if (flight_->waiters_ >= max_waiters_) {
         return acquisition{.status = acquire_status::backpressure};
      }
      ++flight_->waiters_;
      return acquisition{
          .status = acquire_status::draining,
          .flight = flight_,
          .observed = flight_->cleanup_completed_.epoch(),
      };
   }
   auto start = false;
   if (!flight_) {
      flight_ = std::make_shared<managed_remote_reconnect_flight>(std::move(executor));
      start = true;
   }
   if (flight_->waiters_ >= max_waiters_) {
      return acquisition{.status = acquire_status::backpressure};
   }
   ++flight_->waiters_;
   return acquisition{
       .status = acquire_status::joined,
       .flight = flight_,
       .observed = flight_->completed_.epoch(),
       .start = start,
   };
}

managed_remote_state::stop_effects managed_remote_state::request_stop() noexcept {
   auto effects = stop_effects{};
   {
      auto lock = std::scoped_lock{mutex_};
      if (stopped_) {
         return {};
      }
      stopped_ = true;
      effects = stop_effects{
          .current = std::exchange(current_, {}),
          .flight = flight_,
          .initiated = true,
      };
      if (flight_) {
         flight_->stop_requested_ = true;
         flight_->stop_timer_ = std::exchange(timer_, {});
      } else {
         timer_.reset();
      }
   }
   if (effects.flight) {
      effects.flight->stop_changed_.notify();
   }
   return effects;
}

managed_remote_state::flight_observation managed_remote_state::observe_active_flight() const noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!flight_) {
      return {};
   }
   return flight_observation{
       .flight = flight_,
       .observed = flight_->completed_.epoch(),
       .done = flight_->done_,
   };
}

managed_remote_state::completion_snapshot
managed_remote_state::read_completion(const flight_ptr& flight) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!flight) {
      return completion_snapshot{.stopped = stopped_};
   }
   return completion_snapshot{
       .result = flight->result_,
       .error = flight->error_,
       .done = flight->done_,
       .stopped = stopped_,
   };
}

managed_remote_state::completion_effects managed_remote_state::complete(const flight_ptr& flight, generation_ptr result,
                                                                        std::exception_ptr error,
                                                                        std::exception_ptr stopped_error) noexcept {
   auto effects = completion_effects{};
   auto publish = false;
   {
      auto lock = std::scoped_lock{mutex_};
      if (flight->done_) {
         effects.canceled = std::move(result);
         return effects;
      }
      if (result && (stopped_ || flight_ != flight)) {
         effects.canceled = std::exchange(result, {});
         if (!error) {
            error = std::move(stopped_error);
         }
      } else if (result) {
         current_ = result;
         next_peer_ = result->peer_index;
      }
      flight->result_ = std::move(result);
      flight->error_ = std::move(error);
      flight->done_ = true;
      publish = true;
      if (flight_ == flight && flight->watcher_done_ && flight->child_done_) {
         flight_.reset();
      }
   }
   if (publish) {
      flight->completed_.notify();
      flight->stop_changed_.notify();
   }
   return effects;
}

managed_remote_state::stop_observation managed_remote_state::observe_stop(const flight_ptr& flight) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!flight) {
      return {};
   }
   return stop_observation{
       .timer = flight->stop_timer_,
       .observed = flight->stop_changed_.epoch(),
       .requested = flight->stop_requested_,
       .done = flight->done_,
   };
}

void managed_remote_state::finish_watcher(const flight_ptr& flight) noexcept {
   auto cleanup = false;
   {
      auto lock = std::scoped_lock{mutex_};
      if (!flight || flight->watcher_done_) {
         return;
      }
      flight->watcher_done_ = true;
      cleanup = flight->child_done_;
      if (cleanup && flight_ == flight && flight->done_) {
         flight_.reset();
      }
   }
   if (cleanup) {
      flight->cleanup_completed_.notify();
   }
}

void managed_remote_state::finish_child(const flight_ptr& flight, std::exception_ptr error) noexcept {
   auto publish = false;
   auto cleanup = false;
   {
      auto lock = std::scoped_lock{mutex_};
      if (!flight || flight->child_done_) {
         return;
      }
      flight->child_done_ = true;
      if (!flight->done_) {
         flight->error_ = std::move(error);
         flight->done_ = true;
         publish = true;
      }
      cleanup = flight->watcher_done_;
      if (cleanup && flight_ == flight && flight->done_) {
         flight_.reset();
      }
   }
   if (publish) {
      flight->completed_.notify();
      flight->stop_changed_.notify();
   }
   if (cleanup) {
      flight->cleanup_completed_.notify();
   }
}

managed_remote_state::cleanup_observation
managed_remote_state::observe_cleanup(const flight_ptr& flight) const noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (!flight) {
      return {};
   }
   return cleanup_observation{
       .observed = flight->cleanup_completed_.epoch(),
       .done = flight->watcher_done_ && flight->child_done_,
   };
}

managed_remote_state::generation_ptr managed_remote_state::invalidate(const generation_ptr& value,
                                                                      std::size_t peer_count) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (current_ != value) {
      return {};
   }
   next_peer_ = peer_count == 0 ? 0 : (current_->peer_index + 1U) % peer_count;
   return std::exchange(current_, {});
}

void managed_remote_state::leave(const flight_ptr& flight) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (flight && flight->waiters_ != 0) {
      --flight->waiters_;
   }
}

bool managed_remote_state::stopped() const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return stopped_;
}

std::size_t managed_remote_state::next_peer() const noexcept {
   auto lock = std::scoped_lock{mutex_};
   return next_peer_;
}

bool managed_remote_state::install_timer(const timer_ptr& timer) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (stopped_) {
      return false;
   }
   timer_ = timer;
   return true;
}

void managed_remote_state::clear_timer(const timer_ptr& timer) noexcept {
   auto lock = std::scoped_lock{mutex_};
   if (timer_ == timer) {
      timer_.reset();
   }
}

} // namespace forge::plugins::net::p2p::resolver::detail
}
