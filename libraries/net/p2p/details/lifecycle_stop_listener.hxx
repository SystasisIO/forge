#pragma once

namespace forge::net::p2p::detail {

class lifecycle_stop_listener {
 public:
   virtual ~lifecycle_stop_listener();
   virtual void request_lifecycle_stop() noexcept = 0;
};

} // namespace forge::net::p2p::detail
