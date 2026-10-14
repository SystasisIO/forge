#pragma once

#include "server_session.hxx"

namespace forge::net::http {

struct server::impl : std::enable_shared_from_this<server::impl> {
   impl(forge::asio::runtime& runtime_value, server_config config_value, server_handler handler_value,
        std::shared_ptr<router> router_value);
   boost::asio::awaitable<void> accept_loop();
   boost::asio::awaitable<std::shared_ptr<detail::server_session>>
   complete_tls_handshake(std::shared_ptr<forge::net::tls::beast_tls_stream> stream,
                          forge::net::tls::context_snapshot_ptr snapshot);
   void prune_sessions();
   void remember_session(const std::shared_ptr<detail::server_session>& session);
   std::vector<std::shared_ptr<detail::server_session>> active_sessions();
   bool reserve_tls_handshake();
   void remember_pending_tls_handshake(const std::shared_ptr<forge::net::tls::beast_tls_stream>& stream);
   void release_tls_handshake() noexcept;
   void cancel_pending_tls_handshakes();
   void cancel_pending_tls_handshakes_after_runtime_stopped();
   boost::asio::awaitable<void> wait_until_tls_handshakes_complete();
   void cancel_sessions_after_runtime_stopped();
   void begin_cancel_sessions();
   boost::asio::awaitable<void> async_cancel_sessions();
   boost::asio::awaitable<void> wait_until_stop_attempt_finished(std::shared_ptr<std::exception_ptr> pending);
   boost::asio::awaitable<void> wait_until_accept_loop_completed();
   void complete_accept_loop() noexcept;
   void complete_stop() noexcept;
   void start_on_executor();
   void stop_on_executor();
   void stop_after_runtime_stopped();
   boost::asio::awaitable<void> async_stop_on_executor();

   forge::asio::runtime& runtime;
   server_config config;
   server_handler handler;
   std::shared_ptr<router> router_value;
   boost::asio::strand<boost::asio::io_context::executor_type> acceptor_executor;
   boost::asio::ip::tcp::acceptor acceptor;
   boost::asio::steady_timer accept_loop_completion;
   boost::asio::steady_timer stop_completion;
   // Only the active attempt owns this slot. Joined callers retain its result
   // across failure/retry, independently of the server's drain state.
   std::shared_ptr<std::exception_ptr> stop_attempt;
   boost::asio::steady_timer tls_handshake_completion;
   std::vector<std::weak_ptr<detail::server_session>> sessions;
   std::vector<std::weak_ptr<forge::net::tls::beast_tls_stream>> pending_tls_handshakes;
   std::atomic_bool stopped = true;
   bool started = false;
   bool stopping = false;
   bool accept_loop_completed = true;
   bool tls_enabled = false;
   std::uint64_t pending_tls_handshake_count = 0;
   std::shared_ptr<detail::server_log> log = std::make_shared<detail::server_log>();
};

} // namespace forge::net::http
