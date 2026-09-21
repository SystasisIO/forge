#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

namespace forge::net::p2p::detail {

class host_event_mailbox;

class host_event_source {
 public:
   explicit host_event_source(std::size_t max_subscribers);
   ~host_event_source();
   [[nodiscard]] host_event_subscription subscribe();
   [[nodiscard]] host_event current() const;
   void publish(host_event value);
   void close() noexcept;

 private:
   mutable std::mutex mutex_;
   std::size_t max_subscribers_;
   std::shared_ptr<const host_event> latest_;
   std::vector<std::weak_ptr<host_event_mailbox>> subscribers_;
   bool closed_ = false;
};

} // namespace forge::net::p2p::detail
