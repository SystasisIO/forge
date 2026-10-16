module;

#include "details/async_waiter.hxx"

#include <forge/exceptions/macros.hpp>

#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/scope/scope_exit.hpp>
#include <boost/system/error_code.hpp>

#include <algorithm>
#include <deque>
#include <memory>
#include <mutex>
#include <ranges>
#include <utility>

module forge.asio.gate;

namespace forge::asio {

namespace {

boost::asio::awaitable<gate::ticket> acquire_owned(std::shared_ptr<detail::gate_state> state) {
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "gate acquire was canceled");
   }
   const auto executor = co_await boost::asio::this_coro::executor;
   auto signal = std::make_shared<detail::async_waiter>(executor);
   auto waiter = std::make_shared<detail::gate_waiter>();
   waiter->wake = [signal] { signal->wake(); };
   auto slot = cancellation.slot();
   if (slot.is_connected())
      slot.assign(detail::gate_cancellation_handler{state, waiter});
   const auto clear_slot = boost::scope::scope_exit{[&slot] {
      if (slot.is_connected())
         slot.clear();
   }};
   if (cancellation.cancelled() != boost::asio::cancellation_type::none)
      detail::gate_cancellation_handler{state, waiter}(cancellation.cancelled());
   // Wake storage exists before cancellation registration, but no waiter has
   // entered the gate. Parent pre-initiation cancellation may safely unwind.
   // Only the disconnected child admits/waits, using the SAME caller executor.
   co_return co_await boost::asio::co_spawn(
       executor, state->acquire(waiter, signal->wait()),
       boost::asio::bind_cancellation_slot(boost::asio::cancellation_slot{}, boost::asio::use_awaitable));
}

} // namespace

gate::gate() : state_{std::make_shared<detail::gate_state>()} {}

gate::~gate() {
   close();
}

boost::asio::awaitable<gate::ticket> gate::acquire() {
   return acquire_owned(state_);
}

void gate::close() noexcept {
   state_->close();
}

bool gate::closed() const noexcept {
   return state_->closed();
}

gate::ticket::ticket(std::shared_ptr<detail::gate_state> state) : state_{std::move(state)} {}

gate::ticket::~ticket() {
   release();
}

gate::ticket::ticket(ticket&& other) noexcept : state_{std::move(other.state_)} {}

gate::ticket& gate::ticket::operator=(ticket&& other) noexcept {
   if (this != &other) {
      release();
      state_ = std::move(other.state_);
   }
   return *this;
}

bool gate::ticket::active() const noexcept {
   return state_ != nullptr;
}

void gate::ticket::release() noexcept {
   auto state = std::move(state_);
   if (state != nullptr) {
      state->release_one();
   }
}

} // namespace forge::asio

namespace forge::asio::detail {

boost::asio::awaitable<forge::asio::gate::ticket>
gate_state::acquire(std::shared_ptr<gate_waiter> waiter, boost::asio::awaitable<boost::system::error_code> wait) {
   const auto self = shared_from_this();

   {
      auto lock = std::scoped_lock{mutex_};
      if (waiter->state == gate_wait_state::canceled) {
         throw exceptions::canceled{"gate acquire was canceled"};
      }
      if (closed_) {
         throw exceptions::rejected{"gate is closed"};
      }
      if (!held_) {
         held_ = true;
         waiter->state = gate_wait_state::completed;
         co_return forge::asio::gate::ticket{self};
      }
      waiters_.push_back(waiter);
   }

   static_cast<void>(co_await std::move(wait));

   auto state = gate_wait_state::canceled;
   {
      auto lock = std::scoped_lock{mutex_};
      state = waiter->state;
      if (state == gate_wait_state::granted) {
         waiter->state = gate_wait_state::completed;
         if (granted_ == waiter) {
            granted_.reset();
         }
         co_return forge::asio::gate::ticket{self};
      }
      if (state == gate_wait_state::queued) {
         waiter->state = gate_wait_state::canceled;
         const auto found = std::ranges::find(waiters_, waiter);
         if (found != waiters_.end()) {
            waiters_.erase(found);
         }
      }
   }

   if (state == gate_wait_state::rejected) {
      throw exceptions::rejected{"gate closed while acquire was waiting"};
   }
   if (state == gate_wait_state::canceled) {
      throw exceptions::canceled{"gate acquire was canceled"};
   }
   throw exceptions::internal{"gate waiter woke without a terminal state"};
}

void gate_cancellation_handler::operator()(boost::asio::cancellation_type_t type) const noexcept {
   if (type != boost::asio::cancellation_type::none) {
      if (auto state = owner.lock()) {
         if (auto pending = waiter.lock()) {
            state->cancel(pending);
         }
      }
   }
}

void gate_state::cancel(const std::shared_ptr<gate_waiter>& waiter) noexcept {
   auto release_grant = false;
   {
      auto lock = std::scoped_lock{mutex_};
      if (waiter->state == gate_wait_state::queued) {
         waiter->state = gate_wait_state::canceled;
         const auto found = std::ranges::find(waiters_, waiter);
         if (found != waiters_.end()) {
            waiters_.erase(found);
         }
      } else if (waiter->state == gate_wait_state::granted) {
         waiter->state = gate_wait_state::canceled;
         if (granted_ == waiter) {
            granted_.reset();
         }
         release_grant = true;
      }
   }

   waiter->wake();
   if (release_grant) {
      release_one();
   }
}

void gate_state::release_one() noexcept {
   auto waiter = std::shared_ptr<gate_waiter>{};
   {
      auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         held_ = false;
         return;
      }
      while (!waiters_.empty() && waiter == nullptr) {
         waiter = std::move(waiters_.front());
         waiters_.pop_front();
         if (waiter->state != gate_wait_state::queued) {
            waiter.reset();
         }
      }
      if (waiter != nullptr) {
         waiter->state = gate_wait_state::granted;
         granted_ = waiter;
      } else {
         held_ = false;
      }
   }
   if (waiter != nullptr) {
      waiter->wake();
   }
}

void gate_state::close() noexcept {
   auto waiters = std::deque<std::shared_ptr<gate_waiter>>{};
   auto granted = std::shared_ptr<gate_waiter>{};
   {
      auto lock = std::scoped_lock{mutex_};
      if (closed_) {
         return;
      }
      closed_ = true;
      waiters.swap(waiters_);
      for (const auto& waiter : waiters) {
         if (waiter->state == gate_wait_state::queued) {
            waiter->state = gate_wait_state::rejected;
         }
      }
      granted = std::move(granted_);
      if (granted != nullptr && granted->state == gate_wait_state::granted) {
         granted->state = gate_wait_state::rejected;
         held_ = false;
      }
   }

   for (const auto& waiter : waiters) {
      waiter->wake();
   }
   if (granted != nullptr) {
      granted->wake();
   }
}

bool gate_state::closed() const noexcept {
   const auto lock = std::scoped_lock{mutex_};
   return closed_;
}

} // namespace forge::asio::detail
