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

#include "connection_test_hooks.hxx"

namespace forge::net::tcp::detail {

enum class socket_state : std::uint8_t {
   active,
   cancel_requested,
   close_requested,
   handed_off,
   closed,
};

class socket_stream final : public transport::detail::stream_concept,
                            public std::enable_shared_from_this<socket_stream> {
 public:
   socket_stream(std::shared_ptr<boost::asio::ip::tcp::socket> socket, boost::asio::strand<boost::asio::any_io_executor> strand,
      options tcp_options, std::int64_t id, std::shared_ptr<std::atomic<socket_state>> state,
      std::shared_ptr<forge::asio::notification> terminal_requested,
      std::shared_ptr<forge::asio::notification> terminal_completed, std::shared_ptr<void> lifetime);
   ~socket_stream() override;
   void activate() noexcept;
   [[nodiscard]] bool valid() const noexcept override;
   [[nodiscard]] std::int64_t id() const noexcept override;
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override;
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override;
   boost::asio::awaitable<transport::chunk> async_read_chunk() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;
   void request_cancel() noexcept;

 private:
   [[nodiscard]] bool request_terminal(socket_state requested) noexcept;

   std::shared_ptr<boost::asio::ip::tcp::socket> socket_;
   boost::asio::strand<boost::asio::any_io_executor> strand_;
   options options_;
   transport::buffer_pool pool_;
   std::int64_t id_ = -1;
   std::shared_ptr<std::atomic<socket_state>> state_;
   std::shared_ptr<forge::asio::notification> terminal_requested_;
   std::shared_ptr<forge::asio::notification> terminal_completed_;
   std::shared_ptr<void> lifetime_;
   std::atomic_bool ownership_active_ = false;
};

[[nodiscard]] std::pair<std::shared_ptr<socket_stream>, transport::stream>
prepare_stream(std::shared_ptr<boost::asio::ip::tcp::socket> socket, boost::asio::strand<boost::asio::any_io_executor> strand,
      options tcp_options, std::int64_t id, std::shared_ptr<std::atomic<socket_state>> state,
      std::shared_ptr<forge::asio::notification> terminal_requested,
      std::shared_ptr<forge::asio::notification> terminal_completed, std::shared_ptr<void> lifetime);
[[noreturn]] void throw_io_error(std::string message, const boost::system::error_code& error);
[[noreturn]] void throw_read_write_error(const boost::system::error_code& error);
void cancel_socket(boost::asio::ip::tcp::socket& socket,
      const std::shared_ptr<connection_test_hooks>& hooks = {}) noexcept;

} // namespace forge::net::tcp::detail
