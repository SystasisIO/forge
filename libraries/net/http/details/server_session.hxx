#pragma once

// Textual private types shared by implementation units have ordinary C++ linkage.
extern "C++" {
namespace forge::net::http::detail {

class server_session {
 public:
   virtual ~server_session();
   virtual void cancel() = 0;
   virtual void cancel_after_runtime_stopped() = 0;
   virtual boost::asio::awaitable<void> async_cancel() = 0;
   virtual boost::asio::awaitable<void> run() = 0;
   [[nodiscard]] virtual boost::asio::any_io_executor executor() noexcept = 0;
   [[nodiscard]] virtual boost::asio::cancellation_slot cancellation_slot() noexcept = 0;

   static std::shared_ptr<server_session> create(
      forge::asio::runtime& runtime, boost::beast::tcp_stream stream, server_config config,
      server_handler handler, std::shared_ptr<router> router_value, std::shared_ptr<server_log> log);
   static std::shared_ptr<server_session> create(
      forge::asio::runtime& runtime, forge::net::tls::beast_tls_stream stream, server_config config,
      server_handler handler, std::shared_ptr<router> router_value, std::shared_ptr<server_log> log,
      forge::net::tls::context_snapshot_ptr snapshot);

 private:
   template <typename Stream> class model;
};

} // namespace forge::net::http::detail
}
