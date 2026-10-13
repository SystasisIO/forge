module;

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <boost/asio/awaitable.hpp>

export module forge.net.p2p.stream;

export import forge.net.transport.buffer;
export import forge.net.transport.frame;
import forge.net.transport.stream;

export namespace forge::net::p2p {

enum class peer_authentication : std::uint8_t {
   unverified = 0,
   quic_tls = 1,
   libp2p_tls = 2,
   noise = 3,
};

namespace detail {
struct stream_access;
} // namespace detail

class stream {
 public:
   stream();
   explicit stream(forge::net::transport::stream value);
   stream(forge::net::transport::stream value, std::vector<std::uint8_t> buffered);
   ~stream();

   stream(stream&&) noexcept;
   stream& operator=(stream&&) noexcept;

   stream(const stream&) = delete;
   stream& operator=(const stream&) = delete;

   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] std::int64_t id() const noexcept;
   [[nodiscard]] peer_authentication authentication() const noexcept;

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<void> async_write(forge::net::transport::chunk bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();
   boost::asio::awaitable<forge::net::transport::chunk> async_read_chunk();
   boost::asio::awaitable<void> async_write_frame(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<void> async_write_frame(forge::net::transport::chunk bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read_frame();
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read_frame(forge::net::transport::frame_options options);
   boost::asio::awaitable<forge::net::transport::chunk> async_read_frame_chunk();
   boost::asio::awaitable<forge::net::transport::chunk>
   async_read_frame_chunk(forge::net::transport::frame_options options);
   // FIN only: retain reads, cancellation and resource ownership until close.
   // Serialize with writes and terminal close.
   boost::asio::awaitable<void> async_close_write();
   boost::asio::awaitable<void> async_close();
   void cancel();
   void request_cancel() noexcept;
   [[nodiscard]] forge::net::transport::stream into_transport_stream() &&;

 private:
   friend struct detail::stream_access;

   struct impl;

   std::shared_ptr<impl> impl_;
};

namespace detail {

struct stream_access {
   [[nodiscard]] static stream with_buffer(stream value, std::vector<std::uint8_t> buffered);
   static void set_authentication(stream& value, peer_authentication authentication) noexcept;
};

} // namespace detail

} // namespace forge::net::p2p
