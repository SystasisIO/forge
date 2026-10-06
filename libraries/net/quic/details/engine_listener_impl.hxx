#pragma once

#include "quic_engine_support.hxx"
#include "listener_shutdown.hxx"

namespace forge::net::quic::detail {
struct engine_listener::impl {
   impl(boost::asio::io_context& context_value, engine_endpoint endpoint_value, engine_server_options options_value);

   boost::asio::io_context& context;
   asio::strand<asio::io_context::executor_type> strand;
   std::shared_ptr<server_udp_socket> server_socket;
   engine_endpoint bind_endpoint;
   engine_server_options options;
   stateless_reset_secret reset_secret = random_stateless_reset_secret();
   initial_token_validator initial_tokens{random_initial_token_secret(), retry_token_lifetime, regular_token_lifetime};
   std::mutex cid_mutex;
   std::unordered_map<std::string, std::shared_ptr<engine_connection::impl>> connections_by_cid;
   std::unordered_map<engine_connection::impl*, std::vector<std::string>> cids_by_connection;
   std::deque<std::shared_ptr<engine_connection>> accepted;
   std::vector<std::weak_ptr<asio::steady_timer>> accept_waiters;
   std::optional<engine_error_kind> pending_accept_error;
   std::string pending_accept_failure_text;
   std::weak_ptr<impl> self;
   std::vector<std::weak_ptr<asio::steady_timer>> operation_waiters;
   std::shared_ptr<listener_shutdown> shutdown_state;
   mutable std::mutex shutdown_mutex;
   std::exception_ptr shutdown_error;
   bool stopped = false;
   std::atomic_bool stop_requested{false};
   std::size_t pending_dials = 0;
   std::size_t active_punches = 0;
   std::vector<std::weak_ptr<asio::steady_timer>> punch_waiters;
   bool receive_started = false;
   bool shutdown_started = false;
   bool shutdown_complete = false;
   std::atomic_size_t active_operations{0};
   std::atomic_size_t active_callbacks{0};

   enum class shutdown_action : std::uint8_t {
      run,
      wait,
      done,
   };

   [[nodiscard]] std::vector<std::shared_ptr<engine_connection::impl>> connections();

   void finish_operation() noexcept;

   void begin_callback() noexcept;

   void finish_callback() noexcept;

   void update_shutdown_operations() noexcept;

   void update_shutdown_operations_locked() noexcept;

   void report_callback_failure(std::exception_ptr error) noexcept;

   boost::asio::awaitable<void> wait_operations_idle();

   [[nodiscard]] shutdown_action begin_shutdown(std::vector<std::shared_ptr<engine_connection::impl>>& prepared);

   [[nodiscard]] std::exception_ptr shutdown_failure() const;

   boost::asio::awaitable<void> async_shutdown();

   boost::asio::awaitable<void> prepare_shutdown(
       std::shared_ptr<listener_shutdown> completion,
       const std::vector<std::shared_ptr<engine_connection::impl>>& prepared);

   void finish_shutdown(std::exception_ptr error = {}) noexcept;

   void clear_connection_registry();

   void stop();

   void stop(std::span<const std::shared_ptr<engine_connection::impl>> prepared);

   void start();

   [[nodiscard]] std::shared_ptr<engine_connection::impl> find_connection_by_cid(const std::string& key);

   [[nodiscard]] std::size_t connection_count();

   void register_connection_cid(const std::shared_ptr<engine_connection::impl>& connection, std::string key);

   void unregister_connection_cid(engine_connection::impl* connection, std::string key);

   [[nodiscard]] bool release_connection_slot(engine_connection::impl* connection);

   void cleanup_connection(const std::shared_ptr<engine_connection::impl>& connection, bool had_connection_ids);

   void start_handshake_deadline(const std::shared_ptr<engine_connection::impl>& connection);

   boost::asio::awaitable<void> send_retry(const ngtcp2_pkt_hd& header,
                                           forge::net::transport::datagram_io::received route);

   boost::asio::awaitable<void> send_invalid_token_close(const ngtcp2_pkt_hd& header,
                                                         forge::net::transport::datagram_io::received route);

   boost::asio::awaitable<void> handle_packet(std::vector<std::uint8_t> packet,
                                              forge::net::transport::datagram_io::received route);

   [[nodiscard]] std::shared_ptr<engine_connection::impl>
   create_server_connection(const ngtcp2_pkt_hd& hd, const initial_token_validation& token,
                            const forge::net::transport::datagram_io::received& route);
};
} // namespace forge::net::quic::detail
