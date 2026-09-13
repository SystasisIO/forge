#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <boost/asio/awaitable.hpp>

namespace forge::net::p2p::detail {

class host_event_mailbox {
 public:
   void publish(std::shared_ptr<const host_event> value) noexcept;
   void close() noexcept;
   [[nodiscard]] bool active() const noexcept;
   static boost::asio::awaitable<std::optional<host_event>> read(std::shared_ptr<host_event_mailbox> self);

 private:
   mutable std::mutex mutex;
   forge::asio::notification changed;
   std::shared_ptr<const host_event> pending;
   bool resync = false;
   bool closed = false;
   bool reading = false;
};

} // namespace forge::net::p2p::detail
