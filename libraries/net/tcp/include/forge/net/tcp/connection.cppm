module;

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>

namespace forge::net::tcp::detail {
struct connection_access;
struct connection_test_hooks;
}

export module forge.net.tcp.connection;

export import forge.net.tcp.exceptions;
export import forge.net.tcp.options;
export import forge.net.transport.connector;

export namespace forge::net::tcp {

class connection {
 public:
   connection();
   // Keeps an owner-supplied native resource alive until the socket stream is
   // closed. TCP itself does not interpret this value.
   explicit connection(boost::asio::ip::tcp::socket socket, options tcp_options = {},
                       std::shared_ptr<void> lifetime = {});
   ~connection();

   connection(connection&&) noexcept;
   connection& operator=(connection&&) noexcept;

   connection(const connection&) = delete;
   connection& operator=(const connection&) = delete;

   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] transport::endpoint local_endpoint() const;
   [[nodiscard]] transport::endpoint remote_endpoint() const;

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<std::size_t> async_read_some(std::span<std::uint8_t> bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();
   boost::asio::awaitable<void> async_close();
   void cancel();
   void request_cancel() noexcept;

   [[nodiscard]] transport::stream_connection into_transport_stream() &&;
   [[nodiscard]] boost::asio::ip::tcp::socket release_socket() &&;
   [[nodiscard]] boost::asio::ip::tcp::socket release_socket(std::shared_ptr<void>& lifetime) &&;

 private:
   friend struct detail::connection_access;
   connection(boost::asio::ip::tcp::socket socket, options tcp_options, std::shared_ptr<void> lifetime,
              std::shared_ptr<detail::connection_test_hooks> hooks);
   struct impl;
   std::shared_ptr<impl> impl_;
};

} // namespace forge::net::tcp
