module;

#include <memory>
#include <optional>
#include <functional>
#include <utility>
#include <boost/asio/awaitable.hpp>

module forge.net.p2p.host_event_subscription;

import forge.net.p2p.host_event;

#include "details/host_event_subscription_impl.hxx"

namespace forge::net::p2p {

host_event_subscription::host_event_subscription() noexcept = default;
host_event_subscription::~host_event_subscription() { close(); }
host_event_subscription::host_event_subscription(host_event_subscription&&) noexcept = default;

host_event_subscription& host_event_subscription::operator=(host_event_subscription&& other) noexcept {
   if (this != &other) {
      close();
      impl_ = std::move(other.impl_);
   }
   return *this;
}

host_event_subscription::host_event_subscription(read_callback read, close_callback close, active_callback active)
    : impl_(std::make_shared<impl>(impl{std::move(read), std::move(close), std::move(active)})) {}

bool host_event_subscription::active() const noexcept {
   try {
      return impl_ && impl_->active();
   } catch (...) {
      return false;
   }
}

boost::asio::awaitable<std::optional<host_event>> host_event_subscription::async_read() {
   // Invoke eagerly so the producer retains its state before the caller can drop this handle.
   return impl_ ? impl_->read() : []( ) -> boost::asio::awaitable<std::optional<host_event>> {
      co_return std::nullopt;
   }();
}

void host_event_subscription::close() noexcept {
   try {
      if (impl_) {
         impl_->close();
      }
   } catch (...) {
   }
}

host_event_subscription detail::host_event_subscription_access::make(host_event_subscription::read_callback read,
                                                                     host_event_subscription::close_callback close,
                                                                     host_event_subscription::active_callback active) {
   return host_event_subscription{std::move(read), std::move(close), std::move(active)};
}

} // namespace forge::net::p2p
