module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.stream;

#include "details/secure_io.hxx"

namespace forge::net::p2p::detail {

secure_io::secure_io(forge::net::p2p::stream stream) : stream_(std::move(stream)) {}

[[nodiscard]] bool secure_io::valid() const noexcept {
   return stream_.valid();
}

[[nodiscard]] std::int64_t secure_io::id() const noexcept {
   return stream_.id();
}

boost::asio::awaitable<void> secure_io::write_plain_frame(std::span<const std::uint8_t> bytes) {
   if (bytes.size() > std::numeric_limits<std::uint16_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::codec_error, "Noise frame is too large");
   }
   auto out = std::vector<std::uint8_t>{
       static_cast<std::uint8_t>((bytes.size() >> 8U) & 0xffU),
       static_cast<std::uint8_t>(bytes.size() & 0xffU),
   };
   out.insert(out.end(), bytes.begin(), bytes.end());
   co_await stream_.async_write(out);
}

boost::asio::awaitable<std::vector<std::uint8_t>> secure_io::read_plain_frame() {
   const auto header = co_await read_exact(2);
   const auto size = (static_cast<std::uint16_t>(header[0]) << 8U) | header[1];
   co_return co_await read_exact(size);
}

void secure_io::set_cipher_states(noise_cipher_state read_state, noise_cipher_state write_state) {
   read_state_ = std::move(read_state);
   write_state_ = std::move(write_state);
}

boost::asio::awaitable<void> secure_io::async_write(std::span<const std::uint8_t> bytes) {
   constexpr auto authentication_tag_size = std::size_t{16};
   constexpr auto maximum_plaintext =
       static_cast<std::size_t>((std::numeric_limits<std::uint16_t>::max)()) - authentication_tag_size;
   if (bytes.empty()) {
      auto encrypted = write_state_.encrypt({}, bytes);
      co_await write_plain_frame(encrypted);
      co_return;
   }
   for (auto offset = std::size_t{}; offset < bytes.size();) {
      const auto size = std::min(maximum_plaintext, bytes.size() - offset);
      auto encrypted = write_state_.encrypt({}, bytes.subspan(offset, size));
      co_await write_plain_frame(encrypted);
      offset += size;
   }
}

boost::asio::awaitable<std::vector<std::uint8_t>> secure_io::async_read() {
   auto encrypted = co_await read_plain_frame();
   auto plain = read_state_.decrypt({}, encrypted);
   co_return plain;
}

boost::asio::awaitable<void> secure_io::async_close() {
   co_await stream_.async_close();
}

void secure_io::cancel() {
   stream_.cancel();
}

void secure_io::request_cancel() noexcept {
   stream_.request_cancel();
}

boost::asio::awaitable<std::vector<std::uint8_t>> secure_io::read_exact(std::size_t size) {
   while (buffer_.size() < size) {
      auto chunk = co_await stream_.async_read();
      if (chunk.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "Noise stream closed");
      }
      buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
   }
   auto out = std::vector<std::uint8_t>{buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(size)};
   buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(size));
   co_return out;
}

} // namespace forge::net::p2p::detail
