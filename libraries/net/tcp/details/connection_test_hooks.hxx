#pragma once

#include <memory>
#include <boost/asio/ip/tcp.hpp>

namespace forge::net::tcp::detail {

enum class startup_stage { owner_allocation, terminal_launch };
enum class native_close_stage { before_close, after_close };

// Private fixture seam. Callbacks and their state are prepared before startup;
// the close callback must not throw and runs with the actual socket still owned.
struct connection_test_hooks {
   std::shared_ptr<void> state;
   void (*startup)(void*, startup_stage) = nullptr;
   void (*native_close)(void*, const boost::asio::ip::tcp::socket&, native_close_stage) noexcept = nullptr;
};

} // namespace forge::net::tcp::detail
