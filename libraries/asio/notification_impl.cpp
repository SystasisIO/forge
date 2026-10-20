module;

#include "details/notification_waiter.hxx"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <boost/scope/scope_exit.hpp>

module forge.asio.notification;

#include "details/notification_impl.hxx"

namespace forge::asio {

notification::epoch_type notification::impl::epoch() const noexcept {
   auto lock = std::scoped_lock{mutex};
   return current_epoch;
}

std::optional<notification::epoch_type>
notification::impl::subscribe(epoch_type observed_epoch,
                              const std::shared_ptr<detail::notification_waiter>& value) {
   auto lock = std::scoped_lock{mutex};
   if (current_epoch != observed_epoch) {
      return current_epoch;
   }
   std::erase_if(waiters, [](const auto& waiter) { return waiter.expired(); });
   waiters.emplace_back(value);
   return std::nullopt;
}

void notification::impl::unsubscribe(const std::shared_ptr<detail::notification_waiter>& value) noexcept {
   auto lock = std::scoped_lock{mutex};
   std::erase_if(waiters, [&value](const auto& candidate) {
      const auto pending = candidate.lock();
      return !pending || pending == value;
   });
}

boost::asio::awaitable<notification::epoch_type>
notification::impl::async_wait(epoch_type observed_epoch) {
   co_return co_await async_wait_until(observed_epoch, boost::asio::steady_timer::time_point::max());
}

boost::asio::awaitable<notification::epoch_type>
notification::impl::async_wait(epoch_type observed_epoch, std::stop_token stop) {
   co_return co_await async_wait_until(observed_epoch, boost::asio::steady_timer::time_point::max(),
                                      std::move(stop));
}

boost::asio::awaitable<notification::epoch_type>
notification::impl::async_wait_until(epoch_type observed_epoch,
                                     std::chrono::steady_clock::time_point deadline) {
   co_return co_await async_wait_until_impl(observed_epoch, deadline, std::nullopt);
}

boost::asio::awaitable<notification::epoch_type>
notification::impl::async_wait_until(epoch_type observed_epoch,
                                     std::chrono::steady_clock::time_point deadline,
                                     std::stop_token stop) {
   co_return co_await async_wait_until_impl(observed_epoch, deadline, std::move(stop));
}

boost::asio::awaitable<notification::epoch_type>
notification::impl::async_wait_until_impl(epoch_type observed_epoch,
                                          std::chrono::steady_clock::time_point deadline,
                                          std::optional<std::stop_token> stop) {
   auto executor = co_await boost::asio::this_coro::executor;
   auto pending = std::make_shared<detail::notification_waiter>(executor);
   if (const auto current = subscribe(observed_epoch, pending)) {
      co_return *current;
   }
   const auto subscription = boost::scope::scope_exit{[this, pending] { unsubscribe(pending); }};
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   auto slot = cancellation.slot();
   if (slot.is_connected()) {
      slot.assign([weak = std::weak_ptr{pending}](boost::asio::cancellation_type_t type) noexcept {
         if (type != boost::asio::cancellation_type::none) {
            if (auto waiter = weak.lock()) { waiter->wake(); }
         }
      });
   }
   const auto clear_slot = boost::scope::scope_exit{[&slot] { if (slot.is_connected()) { slot.clear(); } }};
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) { pending->wake(); }

   const auto wait_error = stop ? co_await pending->wait_until(deadline, *stop)
                                : co_await pending->wait_until(deadline);

   // Terminal delivery already completed. This dispatch only restores the
   // caller executor before exposing the epoch or wait error to the caller.
   auto restore_error = boost::system::error_code{};
   co_await boost::asio::dispatch(
       executor,
       boost::asio::bind_cancellation_slot(
           boost::asio::cancellation_slot{},
           boost::asio::redirect_error(boost::asio::use_awaitable, restore_error)));
   (void)restore_error;

   const auto current = epoch();
   if (wait_error && current == observed_epoch) {
      throw boost::system::system_error{wait_error};
   }
   co_return current;
}

void notification::impl::notify() noexcept {
   auto pending = std::vector<std::weak_ptr<detail::notification_waiter>>{};
   {
      auto lock = std::scoped_lock{mutex};
      ++current_epoch;
      if (current_epoch == 0) {
         ++current_epoch;
      }
      // Transfer the already allocated waiter storage. notify() is a noexcept
      // terminal path, so it must not reserve or materialize strong references.
      pending.swap(waiters);
   }
   for (const auto& value : pending) {
      if (const auto waiter = value.lock()) {
         waiter->wake();
      }
   }
}

} // namespace forge::asio
