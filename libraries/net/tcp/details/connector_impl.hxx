#pragma once

namespace forge::net::tcp {
namespace asio = boost::asio;
using asio_tcp = boost::asio::ip::tcp;
struct connector::impl final : transport::detail::stream_connector_concept,
                               std::enable_shared_from_this<connector::impl> {
   impl(boost::asio::any_io_executor executor_value, options tcp_options_value);

   ~impl() override;

   using socket_map = std::map<std::uint64_t, std::shared_ptr<asio_tcp::socket>>;
   using resolver_map = std::map<std::uint64_t, std::shared_ptr<asio_tcp::resolver>>;

   void start_terminal_worker();
   void finish_terminal() noexcept;

   [[nodiscard]] bool valid() const noexcept override;

   boost::asio::awaitable<connection> async_connect_connection(transport::endpoint remote,
                                                               std::shared_ptr<void> lifetime);

   boost::asio::awaitable<transport::stream_connection> async_connect(transport::endpoint remote,
                                                                      transport::connect_options) override;

   void cancel() override;

   void request_cancel() noexcept;
   boost::asio::awaitable<void> async_stop();

   asio::strand<asio::any_io_executor> strand;
   options tcp_options;
   std::optional<transport::endpoint> local;
   reuse_policy reuse = reuse_policy::required;
   std::function<bool()> source_open;
   std::shared_ptr<forge::asio::notification> source_closed;
   std::shared_ptr<void> source_owner;
   std::shared_ptr<socket_map> sockets;
   std::shared_ptr<resolver_map> resolvers;
   std::shared_ptr<forge::asio::notification> terminal_requested;
   std::shared_ptr<forge::asio::notification> terminal_completed;
   std::exception_ptr terminal_failure;
   bool terminal_worker_completed = false;
   bool terminal_published = false;
   std::atomic_bool fail_terminal_wait_for_test = false;
   std::function<boost::asio::awaitable<void>()> before_attempt_release_for_test;
   std::function<boost::asio::awaitable<void>(const asio_tcp::socket&, int, boost::system::error_code)>
       before_reuse_fallback_for_test;
   std::uint64_t next_generation = 1;
   std::atomic_bool canceled = false;
};
} // namespace forge::net::tcp
