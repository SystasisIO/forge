module;

#include <memory>
#include <optional>
#include <functional>
#include <boost/asio/awaitable.hpp>

export module forge.net.p2p.host_event_subscription;

import forge.net.p2p.host_event;

export namespace forge::net::p2p {

namespace detail {
struct host_event_subscription_access;
}

class host_event_subscription {
 public:
   host_event_subscription() noexcept;
   ~host_event_subscription();
   host_event_subscription(host_event_subscription&&) noexcept;
   host_event_subscription& operator=(host_event_subscription&&) noexcept;
   host_event_subscription(const host_event_subscription&) = delete;
   host_event_subscription& operator=(const host_event_subscription&) = delete;

   [[nodiscard]] bool active() const noexcept;
   boost::asio::awaitable<std::optional<host_event>> async_read();
   void close() noexcept;

 private:
   struct impl;
   using read_callback = std::function<boost::asio::awaitable<std::optional<host_event>>() >;
   using close_callback = std::function<void()>;
   using active_callback = std::function<bool()>;
   host_event_subscription(read_callback read, close_callback close, active_callback active);
   std::shared_ptr<impl> impl_;
   friend struct detail::host_event_subscription_access;
};

namespace detail {
struct host_event_subscription_access {
   [[nodiscard]] static host_event_subscription make(host_event_subscription::read_callback read,
                                                    host_event_subscription::close_callback close,
                                                    host_event_subscription::active_callback active);
};
}

} // namespace forge::net::p2p
