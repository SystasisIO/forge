#pragma once

#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/io_context.hpp>

namespace forge::net::p2p {
class cancellation_latch;

namespace detail {
class worker_stop_bridge;
class worker_terminal_owner;

// Native stream terminal errors and their canonical transport closed category,
// not the broader P2P closed mapper. Caller retains phase/cancellation checks.
[[nodiscard]] bool autonat_v2_remote_close(const forge::exceptions::base& error) noexcept;

struct autonat_v2_dialback {
   autonat_v2_dialback(stream channel, std::uint64_t nonce, boost::asio::io_context& context,
                     std::chrono::steady_clock::time_point parent_deadline);

   boost::asio::awaitable<void> run(std::shared_ptr<worker_terminal_owner> terminal);
   boost::asio::awaitable<void> close();

   stream channel;
   std::uint64_t nonce;
   boost::asio::io_context& context;
   std::chrono::steady_clock::time_point parent_deadline;
   std::shared_ptr<worker_stop_bridge> stop;
   bool nonce_written = false;
   bool close_completed = false;
};

// Takes a negotiated, resource-guarded stream. Completion joins its terminal
// cleanup; the caller must still close the isolated native connection attempt.
boost::asio::awaitable<reachability::v2::dial_status> async_autonat_v2_dialback(
    stream channel, std::uint64_t nonce, boost::asio::io_context& context,
    std::chrono::steady_clock::time_point parent_deadline,
    std::shared_ptr<cancellation_latch> cancellation);

} // namespace detail
} // namespace forge::net::p2p
