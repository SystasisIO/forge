#pragma once

#include <exception>

namespace forge::plugins::net::p2p::node::detail {

// Result fields use the owning component's resource mutex. Once done, the
// result never changes, even when another external caller starts a retry.
struct shutdown_attempt {
   forge::asio::notification completed;
   bool done = false;
   bool terminal = false;
   std::exception_ptr error;
};

} // namespace forge::plugins::net::p2p::node::detail
