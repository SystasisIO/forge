#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

namespace forge::net::p2p::detail {

class secure_io;

class secure_stream_concept final : public forge::net::transport::detail::stream_concept {
 public:
   explicit secure_stream_concept(std::shared_ptr<secure_io> secure);

   [[nodiscard]] bool valid() const noexcept override;

   [[nodiscard]] std::int64_t id() const noexcept override;

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override;

   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override;

   boost::asio::awaitable<void> async_close() override;

   void cancel() override;

   void request_cancel() noexcept;

 private:
   std::shared_ptr<secure_io> secure_;
};

[[nodiscard]] forge::net::transport::stream secure_transport_stream(std::shared_ptr<secure_io> secure);

} // namespace forge::net::p2p::detail
