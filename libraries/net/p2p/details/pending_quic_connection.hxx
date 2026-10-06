#pragma once

#include <mutex>
#include <optional>

#include <boost/asio/awaitable.hpp>

namespace forge::net::p2p::direct::detail {

// Owns a native QUIC connection until the direct profile promotes it to a
// transport session. Cancellation can race policy checks without post()ing.
struct pending_quic_connection {
   void install(forge::net::quic::connection value) noexcept;
   // A canceled/occupied rendezvous leaves ownership with the accept loop.
   [[nodiscard]] bool try_install(forge::net::quic::connection& value) noexcept;
   boost::asio::awaitable<void> async_wait();
   [[nodiscard]] forge::net::quic::connection* get() noexcept;
   [[nodiscard]] forge::net::quic::connection take() noexcept;
   void request_cancel() noexcept;

 private:
   mutable std::mutex mutex_;
   std::optional<forge::net::quic::connection> value_;
   forge::asio::notification changed_;
   bool canceled_ = false;
   bool installed_ = false;
};

} // namespace forge::net::p2p::direct::detail
