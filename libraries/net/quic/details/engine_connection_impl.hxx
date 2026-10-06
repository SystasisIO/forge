#pragma once

#include "quic_engine_support.hxx"
#include "engine_connection_metrics_state.hxx"
#include "engine_stream_impl.hxx"

#include <boost/asio/async_result.hpp>

namespace forge::net::quic::detail {
struct engine_connection::impl {
   struct queued_packet {
      std::vector<std::uint8_t> bytes;
      forge::net::transport::datagram_io::received route;
   };

   impl(asio::io_context& context_value, std::shared_ptr<udp::socket> socket_value, udp::endpoint local_endpoint_value,
        udp::endpoint remote_endpoint_value, engine_transport_limits limits_value);

   impl(asio::io_context& context_value, std::shared_ptr<server_udp_socket> server_socket_value,
        udp::endpoint local_endpoint_value, udp::endpoint remote_endpoint_value, engine_transport_limits limits_value);

   ~impl();

   asio::io_context& context;
   asio::strand<asio::io_context::executor_type> strand;
   std::shared_ptr<udp::socket> socket;
   std::shared_ptr<server_udp_socket> server_socket;
   forge::asio::gate send_gate;
   udp::endpoint local_endpoint_value;
   udp::endpoint remote_endpoint;
   engine_transport_limits limits;
   engine_connection_metrics_state metrics{};
   stateless_reset_secret reset_secret = random_stateless_reset_secret();
   std::weak_ptr<impl> self;

   ngtcp2_conn* conn = nullptr;
   ngtcp2_crypto_ossl_ctx* ossl_ctx = nullptr;
   ngtcp2_crypto_conn_ref conn_ref{};
   ssl_ctx_ptr ssl_ctx;
   ssl_ptr ssl;
   engine_security_options peer_security{};
   std::optional<engine_peer_certificate> peer_certificate_value;
   std::mutex inbound_admission_mutex;
   std::shared_ptr<void> inbound_admission;

   std::unordered_map<std::int64_t, std::shared_ptr<engine_stream::impl>> streams;
   std::deque<std::shared_ptr<engine_stream::impl>> accepted_streams;
   std::vector<std::weak_ptr<asio::steady_timer>> handshake_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> accept_stream_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> open_stream_waiters;
   std::vector<std::weak_ptr<asio::steady_timer>> background_waiters;
   std::function<void()> handshake_completed_hook;
   std::function<void(std::shared_ptr<impl>)> closed_hook;
   std::function<void(const ngtcp2_cid&)> local_connection_id_issued_hook;
   std::function<void(const ngtcp2_cid&)> local_connection_id_retired_hook;
   std::function<void(impl&)> issue_new_token;
   std::function<void(std::vector<std::uint8_t>)> client_token_store;
   std::function<bool(std::string_view)> test_failpoint;
   std::optional<std::vector<std::uint8_t>> pending_client_token;

   asio::steady_timer handshake_timer;
   asio::steady_timer expiry_timer;
   asio::steady_timer owner_drain_timer;
   asio::steady_timer close_ready_timer;
   std::deque<server_udp_socket::packet> outbound_datagrams;
   std::deque<queued_packet> inbound_packets;
   std::size_t queued_datagram_bytes = 0;
   std::size_t queued_inbound_packet_bytes = 0;
   bool handshake_done = false;
   std::optional<engine_error_kind> handshake_terminal_cause;
   boost::system::error_code udp_transport_error;
   bool closing = false;
   bool canceled = false;
   bool terminal_cleanup_complete = false;
   bool close_started = false;
   bool close_cleanup_complete = false;
   bool close_completion_pending = false;
   bool close_work_active = false;
   std::exception_ptr close_error;
   std::function<void()> shutdown_completed_hook;
   std::atomic_bool cancellation_requested{false};
   std::atomic_bool owner_released{false};
   std::atomic<ngtcp2_tstamp> owner_released_at{0};
   std::atomic_bool terminal_signaled{false};
   forge::asio::notification termination_changed;
   bool closed_hook_called = false;
   bool closed_hook_delivered = false;
   bool receive_loop_started = false;
   bool cancel_request_worker_started = false;
   bool owner_drain_timer_started = false;
   bool owner_drain_timer_expired = false;
   std::atomic_size_t background_jobs{0};
   bool drain_active = false;
   bool drain_requested = false;
   // Owner-strand generations let a caller join the native drain pass it
   // requested without waiting for unrelated future drain work to become idle.
   std::uint64_t drain_request_generation = 0;
   std::uint64_t drain_completed_generation = 0;
   forge::asio::notification drain_completion_changed;
   bool udp_send_active = false;
   // A captured FIFO prefix includes the popped packet still in flight.
   // Only actual socket-send completion advances its completed generation.
   std::uint64_t udp_enqueued_generation = 0;
   std::uint64_t udp_completed_generation = 0;
   std::uint64_t udp_inflight_generation = 0;
   std::uint64_t udp_failed_generation = 0;
   boost::system::error_code udp_send_error;
   std::exception_ptr udp_send_exception;
   // Queue discard is not a socket-send failure or proof of delivery.
   bool udp_send_discarded = false;
   bool native_remote_close_received = false;
   forge::asio::notification udp_send_changed;
   bool packet_processing_active = false;
   bool expiry_event_pending = false;
   bool server_side = false;
   bool listener_accept_notified = false;
   bool report_accept_failure = true;
   bool client_token_store_verified = false;
   bool new_token_submitted = false;
   std::int64_t last_writable_stream_id = -1;

   [[nodiscard]] udp::endpoint local_endpoint() const;

   [[nodiscard]] std::shared_ptr<engine_stream::impl> ensure_stream(std::int64_t stream_id);

   [[nodiscard]] static bool stream_is_active(const std::shared_ptr<engine_stream::impl>& stream) noexcept;

   [[nodiscard]] std::size_t active_stream_count() const;

   void update_active_stream_metrics();

   void clear_queued_work();

   void release_queued_stream_writes(const std::shared_ptr<engine_stream::impl>& stream);

   [[nodiscard]] bool reset_stream_on_owner(const std::shared_ptr<engine_stream::impl>& stream,
                                            stream_terminal_owner& cleanup) noexcept;

   boost::asio::awaitable<void>
   async_reset_stream_after_close_failure(const std::shared_ptr<engine_stream::impl>& stream);

   boost::asio::awaitable<void> drain_stream_reset(const std::shared_ptr<engine_stream::impl>& stream,
                                                   std::chrono::steady_clock::time_point deadline);

   void start_stream_cancel_worker(const std::shared_ptr<engine_stream::impl>& stream);

   void wake_and_clear_streams(bool reset_streams);

   void deliver_closed_hook_if_idle() noexcept;

   void notify_closed_once() noexcept;

   void finish_background_job() noexcept;

   boost::asio::awaitable<void> wait_background_idle();

   boost::asio::awaitable<void> async_close_on_owner(std::chrono::steady_clock::time_point deadline, bool cleanup_only);

   template <typename Launch, typename CompletionToken> auto async_wait_close(Launch launch, CompletionToken&& token) {
      return asio::async_initiate<CompletionToken, void(boost::system::error_code)>(
          [owner = self.lock(), launch = std::move(launch)](auto handler) mutable {
             const auto connection = std::move(owner);
             assert(connection && connection->strand.running_in_this_thread());
             // Match dial_batch: install native completion before any close
             // work can publish a dependent owner or fail its allocating join.
             connection->close_ready_timer.async_wait(
                 asio::bind_cancellation_slot(asio::cancellation_slot{}, std::move(handler)));
             try {
                if (connection->test_failpoint) {
                   static_cast<void>(connection->test_failpoint("async_close_terminal_wait_armed"));
                }
                if (connection->close_cleanup_complete) {
                   connection->complete_close();
                   return;
                }
                launch(connection);
             } catch (...) {
                connection->fail_all();
                connection->complete_close(std::current_exception());
             }
          },
          token);
   }

   void complete_close(std::exception_ptr error = {}) noexcept;

   void observe_shutdown(std::function<void()> completed);

   boost::asio::awaitable<void> wait_close_cleanup();

   template <typename Operation> void spawn_background(Operation operation) {
      auto shared = self.lock();
      if (!shared) {
         return;
      }
      shared->background_jobs.fetch_add(1, std::memory_order_release);
      try {
         asio::dispatch(strand, [shared, operation = std::move(operation)]() mutable {
            if (shared->closing || shared->canceled) {
               shared->finish_background_job();
               return;
            }
            try {
               asio::co_spawn(
                   shared->strand,
                   [shared, operation = std::move(operation)]() mutable -> asio::awaitable<void> {
                      const auto finish = [shared](engine_connection::impl*) noexcept {
                         shared->finish_background_job();
                      };
                      auto guard = std::unique_ptr<engine_connection::impl, decltype(finish)>{shared.get(), finish};
                      try {
                         co_await operation(shared);
                      } catch (...) {
                         shared->fail_all();
                      }
                   },
                   asio::detached);
            } catch (...) {
               shared->finish_background_job();
               shared->fail_all();
            }
         });
      } catch (...) {
         shared->finish_background_job();
         throw;
      }
   }

   void cancel_transport_io(bool close_socket);

   void signal_terminal() noexcept;
   void request_cancel() noexcept;

   void fail_all() noexcept;

   void fail_udp(boost::system::error_code error) noexcept;

   void close_transport(bool cancel_socket);

   void verify_selected_alpn(std::string_view expected);

   void verify_peer(const engine_security_options& security);

   void complete_handshake();

   void commit_pending_client_token() noexcept;

   boost::asio::awaitable<void> wait_handshake(std::chrono::milliseconds timeout);

   asio::awaitable<boost::system::error_code> send_packet_impl(server_udp_socket::packet packet);

   asio::awaitable<void> wait_send_terminal();

   asio::awaitable<boost::system::error_code>
   send_packet(server_udp_socket::packet packet, bool closing_packet = false,
               std::optional<std::chrono::steady_clock::time_point> deadline = {});

   void fail_udp_send(boost::system::error_code error, std::exception_ptr exception = {},
                      bool discarded = false) noexcept;

   void finish_udp_send_loop() noexcept;

   void rethrow_udp_failure(std::optional<std::uint64_t> generation) const;

   asio::awaitable<void> wait_udp_send_prefix(std::uint64_t generation, std::chrono::steady_clock::time_point deadline,
                                              std::shared_ptr<engine_stream::impl> stream = {});

   asio::awaitable<void> wait_udp_send_idle();

   void start_udp_send_loop();

   void enqueue_datagram(std::span<const std::uint8_t> packet, const ngtcp2_path& path);

   void request_packet_processing();

   void request_expiry_processing();

   void schedule_post_ngtcp2_work();

   void schedule_expiry();

   boost::asio::awaitable<void> handle_expiry_event();

   [[nodiscard]] std::vector<std::shared_ptr<engine_stream::impl>> writable_streams();

   void reject_unwritable_stream(const std::shared_ptr<engine_stream::impl>& stream, ngtcp2_ssize error);

   void mark_stream_data_submitted(std::shared_ptr<engine_stream::impl>& stream, ngtcp2_ssize data_len);

   void complete_submitted_writes(std::shared_ptr<engine_stream::impl>& stream);

   void complete_drain_requests(std::uint64_t generation) noexcept;

   boost::asio::awaitable<void>
   wait_for_drain_requests(std::uint64_t generation,
                           std::optional<std::chrono::steady_clock::time_point> deadline = {});

   void finish_active_drain() noexcept;

   boost::asio::awaitable<void> drain_send(std::optional<std::chrono::steady_clock::time_point> deadline = {});

   boost::asio::awaitable<void> handle_packet(std::vector<std::uint8_t> packet,
                                              forge::net::transport::datagram_io::received route);

   boost::asio::awaitable<void> process_queued_packets();

   void start_client_receive_loop();

   void start_cancel_request_worker();
};
} // namespace forge::net::quic::detail
