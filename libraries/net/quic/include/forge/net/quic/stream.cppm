module;

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <boost/asio/awaitable.hpp>

export module forge.net.quic.stream;

import forge.net.transport.buffer;

export namespace forge::net::quic {

namespace detail {
struct stream_handle;
struct stream_access;
} // namespace detail

class stream {
 public:
   stream();
   ~stream();

   stream(stream&&) noexcept;
   stream& operator=(stream&&) noexcept;

   stream(const stream&) = delete;
   stream& operator=(const stream&) = delete;

   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] std::int64_t id() const noexcept;

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();
   // Require submitted/drained FIN; retain reads and cancellation. Serialize
   // with writes and terminal close. Prior rejection/reset is not success.
   boost::asio::awaitable<void> async_close_write();
   boost::asio::awaitable<void> async_close();
   void cancel();
   void request_cancel() noexcept;

 private:
   friend struct detail::stream_access;

   explicit stream(detail::stream_handle handle);

   struct impl;
   std::shared_ptr<impl> impl_;
};

namespace detail {

struct stream_access {
   static stream make(stream_handle handle);
   static boost::asio::awaitable<void> async_write_chunk(stream& value, forge::net::transport::chunk bytes);
   static void cancel_write(stream& value);
};

} // namespace detail

} // namespace forge::net::quic
