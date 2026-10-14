#pragma once

#include "server_session.hxx"
#include "beast_body_reader_source.hxx"

extern "C++" {
namespace forge::net::http::detail {

template <typename Stream>
class server_session::model final : public server_session, public std::enable_shared_from_this<model<Stream>> {
 public:
   model(forge::asio::runtime& runtime, Stream stream, server_config config, server_handler handler,
         std::shared_ptr<router> router_value, std::shared_ptr<server_log> log,
         forge::net::tls::context_snapshot_ptr tls_snapshot = {});
   void cancel() override;
   void cancel_after_runtime_stopped() override;
   boost::asio::awaitable<void> async_cancel() override;
   boost::asio::awaitable<void> run() override;
   [[nodiscard]] boost::asio::any_io_executor executor() noexcept override;
   [[nodiscard]] boost::asio::cancellation_slot cancellation_slot() noexcept override;

 private:
   template <typename Result> struct owner_handler_state {
      owner_handler_state(boost::asio::any_io_executor executor,
                          std::shared_ptr<request_read_ownership> ownership);
      void complete_owner(std::exception_ptr error_value, Result response_value) noexcept;
      void complete_monitor(std::exception_ptr error_value) noexcept;
      void mark_disconnected();

      boost::asio::cancellation_signal owner_cancellation;
      boost::asio::steady_timer owner_completion;
      boost::asio::steady_timer monitor_completion;
      std::shared_ptr<request_read_ownership> read_ownership;
      std::optional<Result> owner_response;
      std::exception_ptr owner_error;
      bool owner_done = false;
      bool monitor_done = false;
      bool disconnected = false;
   };

   boost::asio::awaitable<void> async_cancel_on_executor();
   void complete_run() noexcept;
   boost::asio::awaitable<void> run_loop();
   boost::asio::awaitable<void> wait_for_completion(boost::asio::steady_timer& completion, const bool& done);
   template <typename Result>
   boost::asio::awaitable<void> monitor_owner_disconnect(const std::shared_ptr<owner_handler_state<Result>>& state);
   template <typename Result>
   boost::asio::awaitable<std::optional<Result>> handle_owned_operation(
      boost::asio::awaitable<Result> operation, std::shared_ptr<request_read_ownership> read_ownership);
   boost::asio::awaitable<bool> handle_and_write_stream(
      stream_request request_value, const std::shared_ptr<beast_body_reader_source<Stream>>& body_source,
      const std::shared_ptr<int>& request_body_marker,
      const std::shared_ptr<request_read_ownership>& read_ownership, unsigned version, bool request_keep_alive);
   boost::asio::awaitable<void> write_response(response& response_value);
   boost::asio::awaitable<void> write_stream_response(stream_response& response_value);
   boost::asio::awaitable<void> send_stream_response(stream_response& response_value);
   route_context make_context(const request& request_value);
   boost::asio::awaitable<response> handle_http(route_context& context);
   boost::asio::awaitable<bool> try_upgrade(
      const boost::beast::http::request<boost::beast::http::buffer_body>& request_value, route_context& context);
   void cancel_on_executor();
   void close_http_socket();

   forge::asio::runtime& runtime_;
   Stream stream_;
   boost::asio::any_io_executor executor_;
   server_config config_;
   boost::beast::flat_buffer buffer_;
   boost::asio::steady_timer run_completion_;
   server_handler handler_;
   std::shared_ptr<router> router_;
   forge::net::tls::context_snapshot_ptr tls_snapshot_;
   std::shared_ptr<server_log> log_;
   server_log::request access_;
   std::function<void()> owner_stop_;
   boost::asio::cancellation_signal run_cancellation_;
   bool run_completed_ = false;
   bool stopping_ = false;
   bool upgraded_ = false;
};

extern template class server_session::model<boost::beast::tcp_stream>;
extern template class server_session::model<forge::net::tls::beast_tls_stream>;

} // namespace forge::net::http::detail
}
