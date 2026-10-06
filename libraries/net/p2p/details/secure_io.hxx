#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

#include "noise_cipher_state.hxx"

namespace forge::net::p2p::detail {

class secure_io : public std::enable_shared_from_this<secure_io> {
 public:
   explicit secure_io(forge::net::p2p::stream stream);

   [[nodiscard]] bool valid() const noexcept;

   [[nodiscard]] std::int64_t id() const noexcept;

   boost::asio::awaitable<void> write_plain_frame(std::span<const std::uint8_t> bytes);

   boost::asio::awaitable<std::vector<std::uint8_t>> read_plain_frame();

   void set_cipher_states(noise_cipher_state read_state, noise_cipher_state write_state);

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);

   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();

   boost::asio::awaitable<void> async_close();

   void cancel();

   void request_cancel() noexcept;

 private:
   boost::asio::awaitable<std::vector<std::uint8_t>> read_exact(std::size_t size);

   forge::net::p2p::stream stream_;
   std::vector<std::uint8_t> buffer_;
   noise_cipher_state read_state_;
   noise_cipher_state write_state_;
};

} // namespace forge::net::p2p::detail
