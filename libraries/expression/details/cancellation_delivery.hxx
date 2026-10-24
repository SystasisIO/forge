#pragma once

#include <atomic>
#include <new>
#include <utility>

namespace forge::expression::detail {

enum class cancellation_delivery_error { none, resource_exhausted, unavailable };

// Stop callbacks cannot propagate exceptions. Keep delivery refusal explicit
// so the operation owner reports it after joining the callback and exchange.
template <typename Delivery>
void deliver_cancellation(Delivery&& deliver, std::atomic<cancellation_delivery_error>& error) noexcept {
   try {
      std::forward<Delivery>(deliver)();
   } catch (const std::bad_alloc&) {
      error.store(cancellation_delivery_error::resource_exhausted, std::memory_order_release);
   } catch (...) {
      error.store(cancellation_delivery_error::unavailable, std::memory_order_release);
   }
}

} // namespace forge::expression::detail
