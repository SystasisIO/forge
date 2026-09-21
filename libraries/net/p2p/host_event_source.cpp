module;

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.asio.notification;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.host_event_subscription;

#include "details/host_event_mailbox.hxx"
#include "details/host_event_source.hxx"

namespace forge::net::p2p::detail {

host_event_source::host_event_source(std::size_t max_subscribers)
    : max_subscribers_(max_subscribers), latest_(std::make_shared<const host_event>()) {
   if (max_subscribers == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P host event subscriber bound must be positive");
   }
}

host_event_source::~host_event_source() { close(); }

host_event_subscription host_event_source::subscribe() {
   const auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P host events are closed");
   }
   std::erase_if(subscribers_, [](const auto& entry) {
      const auto subscriber = entry.lock();
      return !subscriber || !subscriber->active();
   });
   if (subscribers_.size() >= max_subscribers_) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P host event subscriber limit reached");
   }
   auto state = std::make_shared<host_event_mailbox>();
   state->publish(latest_);
   auto subscription = host_event_subscription_access::make(
       [state] { return host_event_mailbox::read(state); },
       [state] { state->close(); }, [state] { return state->active(); });
   subscribers_.push_back(state);
   return subscription;
}

host_event host_event_source::current() const {
   const auto lock = std::scoped_lock{mutex_};
   return *latest_;
}

void host_event_source::publish(host_event value) {
   const auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      return;
   }
   if (latest_->generation == std::numeric_limits<std::uint64_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::sequence_exhausted, "P2P host event generation exhausted");
   }
   value.generation = latest_->generation + 1;
   value.resync_required = false;
   auto next = std::make_shared<const host_event>(std::move(value));
   latest_ = std::move(next);
   for (const auto& entry : subscribers_) {
      if (const auto state = entry.lock()) {
         state->publish(latest_);
      }
   }
}

void host_event_source::close() noexcept {
   const auto lock = std::scoped_lock{mutex_};
   closed_ = true;
   for (const auto& entry : subscribers_) {
      if (const auto state = entry.lock()) {
         state->close();
      }
   }
   subscribers_.clear();
}

} // namespace forge::net::p2p::detail
