#pragma once

namespace forge::net::tcp {
namespace asio = boost::asio;
using asio_tcp = boost::asio::ip::tcp;
struct listener::impl final : transport::detail::stream_listener_concept, std::enable_shared_from_this<listener::impl> {
   enum class state_value : std::uint8_t {
      open,
      close_requested,
      closed,
   };
   [[noreturn]] static void throw_invalid_endpoint(const transport::endpoint& endpoint, std::string message);
   [[noreturn]] static void throw_invalid_options(std::string message);
   [[noreturn]] static void throw_listen_failed(const transport::endpoint& endpoint,
                                                const boost::system::error_code& error);
   static void validate_options(const options& value);
   static boost::asio::ip::tcp::endpoint to_bind_endpoint(const transport::endpoint& endpoint);
   static transport::endpoint from_asio_endpoint(const boost::asio::ip::tcp::endpoint& endpoint);
   static void configure_socket(boost::asio::ip::tcp::socket& socket, const options& tcp_options);

   impl(boost::asio::any_io_executor executor, transport::endpoint requested, transport::listen_options listen_options,
        options tcp_options_value);

   [[nodiscard]] bool valid() const noexcept override;
   ~impl() override;

   [[nodiscard]] transport::endpoint local_endpoint() const override;

   boost::asio::awaitable<connection> async_accept_connection(std::shared_ptr<void> lifetime);

   boost::asio::awaitable<transport::stream_connection> async_accept() override;

   boost::asio::awaitable<void> async_close() override;

   void close();

   void cancel() override;

   [[nodiscard]] bool request_close() noexcept;

   void close_on_owner() noexcept;

   asio::strand<asio::any_io_executor> strand;
   asio_tcp::acceptor acceptor;
   transport::endpoint local;
   options tcp_options;
   std::shared_ptr<forge::asio::notification> reuse_closed = std::make_shared<forge::asio::notification>();
   mutable std::mutex reuse_mutex;
   std::vector<std::weak_ptr<forge::asio::notification>> connector_drains;
   std::size_t active_accepts = 0;
   forge::asio::notification accepts_changed;
   std::atomic<state_value> state{state_value::open};
};
} // namespace forge::net::tcp
