#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

#include "socket_stream.hxx"

namespace forge::net::tcp {

struct connection::impl final : std::enable_shared_from_this<connection::impl> {
   enum class terminal_request_result {
      requested,
      already_terminal,
      stream_handed_off,
   };

   impl(std::shared_ptr<boost::asio::ip::tcp::socket> socket_value, options tcp_options_value,
      std::shared_ptr<void> lifetime_value, std::shared_ptr<detail::connection_test_hooks> hooks);
   ~impl();
   void start_terminal_worker();
   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] transport::endpoint local_endpoint() const;
   [[nodiscard]] transport::endpoint remote_endpoint() const;
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<std::size_t> async_read_some(std::span<std::uint8_t> bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();
   boost::asio::awaitable<void> async_close();
   void cancel();
   void request_cancel() noexcept;
   [[nodiscard]] transport::stream_connection into_transport_stream();
   [[nodiscard]] boost::asio::ip::tcp::socket release_socket(std::shared_ptr<void>* lifetime_out);
   [[nodiscard]] terminal_request_result request_terminal(detail::socket_state requested) noexcept;
   static void close_on_owner(boost::asio::ip::tcp::socket& current, std::atomic<detail::socket_state>& state,
      const std::shared_ptr<detail::connection_test_hooks>& hooks) noexcept;
   [[nodiscard]] std::shared_ptr<boost::asio::ip::tcp::socket> detach_socket();
   void commit_stream_handoff();
   void claim_operation();
   void release_operation() noexcept;

   std::shared_ptr<boost::asio::ip::tcp::socket> socket;
   options tcp_options;
   boost::asio::strand<boost::asio::any_io_executor> strand;
   transport::endpoint local;
   transport::endpoint remote;
   std::int64_t id = -1;
   mutable std::mutex state_mutex;
   std::shared_ptr<std::atomic<detail::socket_state>> terminal_state;
   std::shared_ptr<forge::asio::notification> terminal_requested;
   std::shared_ptr<forge::asio::notification> terminal_completed;
   std::shared_ptr<void> lifetime;
   std::shared_ptr<detail::connection_test_hooks> test_hooks;
   std::size_t active_operations = 0;
   bool stream_handed_off = false;
};

} // namespace forge::net::tcp
