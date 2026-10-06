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

#include "io_gates.hxx"

namespace forge::net::stcp::detail {

class stream_model final : public transport::detail::stream_concept {
 public:
   stream_model(boost::asio::strand<boost::asio::any_io_executor> strand, std::shared_ptr<io_gates> gates,
      std::shared_ptr<forge::asio::notification> terminal_completed,
      std::shared_ptr<std::exception_ptr> terminal_failure,
      std::size_t read_chunk_size, std::int64_t id, std::shared_ptr<void> lifetime);
   ~stream_model() override;
   [[nodiscard]] bool valid() const noexcept override;
   [[nodiscard]] std::int64_t id() const noexcept override;
   void attach(std::shared_ptr<detail::stream_backend> stream) noexcept;
   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override;
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override;
   boost::asio::awaitable<transport::chunk> async_read_chunk() override;
   boost::asio::awaitable<void> async_close() override;
   void cancel() override;
   void request_cancel() noexcept;

 private:
   std::shared_ptr<stream_backend> stream_;
   boost::asio::strand<boost::asio::any_io_executor> strand_;
   std::shared_ptr<io_gates> gates_;
   std::size_t read_chunk_size_ = 64 * 1024;
   transport::buffer_pool pool_;
   std::int64_t id_ = -1;
   std::shared_ptr<forge::asio::notification> terminal_completed_;
   std::shared_ptr<std::exception_ptr> terminal_failure_;
   std::shared_ptr<void> lifetime_;
};

} // namespace forge::net::stcp::detail
