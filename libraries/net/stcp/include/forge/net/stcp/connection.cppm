module;

#include <cstdint>
#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>

namespace forge::net::stcp::detail {
struct connection_access;
struct connection_test_hooks;
}

export module forge.net.stcp.connection;

export import forge.net.stcp.exceptions;
export import forge.net.stcp.options;
export import forge.net.tcp.connection;
import forge.net.tls.context;
export import forge.net.transport.connector;

namespace forge::net::stcp::detail {
class stream_backend;
}

export namespace forge::net::stcp {

class connection {
 public:
   connection();
   ~connection();

   connection(connection&&) noexcept;
   connection& operator=(connection&&) noexcept;

   connection(const connection&) = delete;
   connection& operator=(const connection&) = delete;

   [[nodiscard]] bool valid() const noexcept;
   [[nodiscard]] transport::endpoint local_endpoint() const;
   [[nodiscard]] transport::endpoint remote_endpoint() const;
   [[nodiscard]] std::optional<peer_certificate> peer_certificate() const;
   [[nodiscard]] certificate_chain peer_certificate_chain() const;
   [[nodiscard]] std::string selected_alpn() const;

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes);
   boost::asio::awaitable<std::size_t> async_read_some(std::span<std::uint8_t> bytes);
   boost::asio::awaitable<std::vector<std::uint8_t>> async_read();
   boost::asio::awaitable<void> async_close();
   void cancel();

   [[nodiscard]] transport::stream_connection into_transport_stream() &&;

 private:
   friend struct detail::connection_access;
   friend boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options);
   friend boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                                  std::chrono::milliseconds timeout);
   friend boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                                  std::optional<std::chrono::milliseconds> timeout,
                                                                  std::stop_token stop);
   friend boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options);
   friend boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                                  std::chrono::milliseconds timeout);
   friend boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                                  std::optional<std::chrono::milliseconds> timeout,
                                                                  std::stop_token stop);
   friend boost::asio::awaitable<connection>
   async_upgrade_client(transport::stream_connection source, client_options options,
                        std::optional<std::chrono::milliseconds> timeout, std::stop_token stop);
   friend boost::asio::awaitable<connection>
   async_upgrade_server(transport::stream_connection source, server_options options,
                        std::optional<std::chrono::milliseconds> timeout, std::stop_token stop);

   struct backend_token {};
   struct impl;

   connection(backend_token, std::shared_ptr<detail::stream_backend> stream, tls::context_snapshot_ptr context,
              std::size_t read_chunk_size, transport::endpoint local, transport::endpoint remote,
              std::shared_ptr<void> lifetime, std::shared_ptr<detail::connection_test_hooks> hooks = {});

   static boost::asio::awaitable<connection>
   async_upgrade_native(tcp::connection source, client_options options,
                        std::optional<std::chrono::milliseconds> timeout, std::stop_token stop,
                        std::shared_ptr<detail::connection_test_hooks> hooks = {});
   static boost::asio::awaitable<connection>
   async_upgrade_native(tcp::connection source, server_options options,
                        std::optional<std::chrono::milliseconds> timeout, std::stop_token stop);

   std::shared_ptr<impl> impl_;
};

boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options);
boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::chrono::milliseconds timeout);
boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::stop_token stop);
boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::chrono::milliseconds timeout, std::stop_token stop);
boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options);
boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::chrono::milliseconds timeout);
boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::stop_token stop);
boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::chrono::milliseconds timeout, std::stop_token stop);
boost::asio::awaitable<connection>
async_upgrade_client(transport::stream_connection source, client_options options,
                     std::optional<std::chrono::milliseconds> timeout = std::nullopt, std::stop_token stop = {});
boost::asio::awaitable<connection>
async_upgrade_server(transport::stream_connection source, server_options options,
                     std::optional<std::chrono::milliseconds> timeout = std::nullopt, std::stop_token stop = {});

} // namespace forge::net::stcp
