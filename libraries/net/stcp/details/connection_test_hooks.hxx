#pragma once

#include <memory>
#include <boost/asio/ip/tcp.hpp>
#include <boost/system/error_code.hpp>

namespace forge::net::stcp::detail {

enum class startup_stage { backend_allocation, owner_allocation, terminal_launch };
enum class native_close_stage { before_close, after_close };

// Fixture-only callbacks are installed before startup. They can hold a real
// native close or timer callback, but cannot replace either production action.
// Both terminal callbacks must not throw.
struct connection_test_hooks {
   std::shared_ptr<void> state;
   void (*startup)(void*, startup_stage) = nullptr;
   void (*native_close)(void*, const boost::asio::ip::tcp::socket&, native_close_stage) noexcept = nullptr;
   void (*timer_callback)(void*, const boost::system::error_code&) noexcept = nullptr;
};

} // namespace forge::net::stcp::detail
