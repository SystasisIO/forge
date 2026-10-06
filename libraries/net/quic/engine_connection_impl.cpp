#include "details/engine_connection_impl.hxx"
#include "details/engine_listener_impl.hxx"

namespace forge::net::quic::detail {
engine_connection::impl::impl(asio::io_context& context_value, std::shared_ptr<udp::socket> socket_value,
                              udp::endpoint local_endpoint_value, udp::endpoint remote_endpoint_value,
                              engine_transport_limits limits_value)
    : context(context_value), strand(asio::make_strand(context_value)), socket(std::move(socket_value)),
      local_endpoint_value(std::move(local_endpoint_value)), remote_endpoint(std::move(remote_endpoint_value)),
      limits(limits_value), handshake_timer(strand), expiry_timer(strand), owner_drain_timer(strand),
      close_ready_timer(strand, asio::steady_timer::time_point::max()) {}

engine_connection::impl::impl(asio::io_context& context_value, std::shared_ptr<server_udp_socket> server_socket_value,
                              udp::endpoint local_endpoint_value, udp::endpoint remote_endpoint_value,
                              engine_transport_limits limits_value)
    : context(context_value), strand(asio::make_strand(context_value)), server_socket(std::move(server_socket_value)),
      local_endpoint_value(std::move(local_endpoint_value)), remote_endpoint(std::move(remote_endpoint_value)),
      limits(limits_value), handshake_timer(strand), expiry_timer(strand), owner_drain_timer(strand),
      close_ready_timer(strand, asio::steady_timer::time_point::max()) {}

engine_connection::impl::~impl() {
   if (conn != nullptr) {
      ngtcp2_conn_del(conn);
   }
   if (ossl_ctx != nullptr) {
      ngtcp2_crypto_ossl_ctx_del(ossl_ctx);
   }
   if (ssl) {
      SSL_set_app_data(ssl.get(), nullptr);
   }
}

[[nodiscard]] udp::endpoint engine_connection::impl::local_endpoint() const {
   return local_endpoint_value;
}

[[nodiscard]] std::shared_ptr<engine_stream::impl> engine_connection::impl::ensure_stream(std::int64_t stream_id) {
   if (auto it = streams.find(stream_id); it != streams.end()) {
      return it->second;
   }
   auto stream = std::make_shared<engine_stream::impl>(stream_id);
   stream->connection = self;
   streams.emplace(stream_id, stream);
   update_active_stream_metrics();
   if (conn != nullptr) {
      (void)ngtcp2_conn_set_stream_user_data(conn, stream_id, stream.get());
   }
   return stream;
}

[[nodiscard]] bool
engine_connection::impl::stream_is_active(const std::shared_ptr<engine_stream::impl>& stream) noexcept {
   return stream && !stream->closed && !stream->reset &&
          !(stream->local_write_closed && (stream->remote_read_closed || stream->remote_read_reset));
}

[[nodiscard]] std::size_t engine_connection::impl::active_stream_count() const {
   return static_cast<std::size_t>(
       std::ranges::count_if(streams, [](const auto& item) { return stream_is_active(item.second); }));
}

void engine_connection::impl::update_active_stream_metrics() {
   metrics.active_streams.store(active_stream_count(), std::memory_order_relaxed);
}

void engine_connection::impl::clear_queued_work() {
   if (udp_completed_generation < udp_enqueued_generation) {
      fail_udp_send(asio::error::operation_aborted, {}, true);
   }
   outbound_datagrams.clear();
   inbound_packets.clear();
   queued_datagram_bytes = 0;
   queued_inbound_packet_bytes = 0;
   metrics.queued_bytes.store(0, std::memory_order_relaxed);
}

void engine_connection::impl::release_queued_stream_writes(const std::shared_ptr<engine_stream::impl>& stream) {
   auto released = std::size_t{0};
   for (auto& write : stream->outbound) {
      if (released <= (std::numeric_limits<std::size_t>::max)() - write.data.size()) {
         released += write.data.size();
      } else {
         released = (std::numeric_limits<std::size_t>::max)();
      }
      wake(write.waiters);
   }
   for (const auto& write : stream->retained) {
      if (released <= (std::numeric_limits<std::size_t>::max)() - write.data.size()) {
         released += write.data.size();
      } else {
         released = (std::numeric_limits<std::size_t>::max)();
      }
   }
   if (released > 0) {
      const auto queued_bytes = metrics.queued_bytes.load(std::memory_order_relaxed);
      if (queued_bytes >= released) {
         metrics.queued_bytes.store(queued_bytes - released, std::memory_order_relaxed);
      } else {
         metrics.queued_bytes.store(0, std::memory_order_relaxed);
      }
   }
   stream->outbound.clear();
   stream->retained.clear();
   if (owner_released.load(std::memory_order_acquire) && metrics.queued_bytes.load(std::memory_order_relaxed) == 0) {
      try {
         static_cast<void>(owner_drain_timer.cancel());
      } catch (...) {
      }
      termination_changed.notify();
   }
}

[[nodiscard]] bool engine_connection::impl::reset_stream_on_owner(const std::shared_ptr<engine_stream::impl>& stream,
                                                                  stream_terminal_owner& cleanup) noexcept {
   assert(strand.running_in_this_thread());
   if (!stream || stream->reset || stream->closed) {
      return false;
   }
   // ngtcp2 may synchronously invoke stream_close_cb during shutdown. Claim
   // recovery ownership first so that callback cannot complete our join.
   cleanup = claim_stream_terminal_owner(stream);
   auto shutdown_result = 0;
   auto should_drain = false;
   if (conn != nullptr && !closing && !canceled) {
      shutdown_result = ngtcp2_conn_shutdown_stream(conn, 0, stream->id, 0);
      should_drain = shutdown_result == 0;
   }
   release_queued_stream_writes(stream);
   stream->reset = true;
   udp_send_changed.notify();
   publish_stream_terminal(stream);
   if (test_failpoint) {
      try {
         static_cast<void>(test_failpoint("stream_reset_published"));
      } catch (...) {
         // Test instrumentation must not alter a noexcept reset transition.
      }
   }
   wake(stream->read_waiters);
   wake(stream->write_waiters);
   if (!std::exchange(stream->reset_counted, true)) {
      metrics.streams_reset.fetch_add(1, std::memory_order_relaxed);
   }
   update_active_stream_metrics();
   if (shutdown_result != 0) {
      fail_all();
      return false;
   }
   return should_drain;
}

boost::asio::awaitable<void>
engine_connection::impl::async_reset_stream_after_close_failure(const std::shared_ptr<engine_stream::impl>& stream) {
   assert(strand.running_in_this_thread());
   // Recovery has its own bounded lifetime; it never renews the failed FIN operation.
   const auto deadline = std::chrono::steady_clock::now() + detached_write_drain_timeout;
   auto cleanup = stream_terminal_owner{nullptr, &release_stream_terminal_owner};
   if (!reset_stream_on_owner(stream, cleanup)) {
      // A reset with no native drain needed must not wait on its own claim.
      cleanup.reset();
      // Another owner may have published reset before it finishes draining
      // the native RESET_STREAM. Join that owner before reporting close.
      if (test_failpoint) {
         static_cast<void>(test_failpoint("stream_reset_before_terminal_join"));
      }
      co_await wait_for_stream_terminal_cleanup(stream);
      co_return;
   }
   co_await drain_stream_reset(stream, deadline);
}

boost::asio::awaitable<void>
engine_connection::impl::drain_stream_reset(const std::shared_ptr<engine_stream::impl>& stream,
                                            std::chrono::steady_clock::time_point deadline) {
   assert(strand.running_in_this_thread());
   // ngtcp2 owns pending RESET_STREAM and PTO after accepted shutdown. This
   // join covers queued UDP/coroutines only; a zero write does not prove RST emission.
   auto prefix = udp_enqueued_generation;
   try {
      co_await drain_send(deadline);
      prefix = udp_enqueued_generation;
      co_await wait_udp_send_prefix(prefix, deadline);
   } catch (...) {
      prefix = udp_enqueued_generation;
      if (!stream->terminal_cleanup_error) {
         stream->terminal_cleanup_error = std::current_exception();
      }
      // Only a failed reset recovery with outstanding socket work needs transport abort.
      // An expired FIN operation alone never takes this path with its old deadline.
      if (udp_completed_generation < prefix && !closing && !canceled) {
         fail_all();
      }
   }
   // A transport failure must join the actual send race before releasing the claim.
   if (closing || canceled) {
      co_await wait_udp_send_idle();
   }
}

void engine_connection::impl::start_stream_cancel_worker(const std::shared_ptr<engine_stream::impl>& stream) {
   assert(strand.running_in_this_thread());
   if (stream->cancel_worker_started) {
      return;
   }
   auto shared = self.lock();
   if (!shared) {
      throw_engine(engine_error_kind::connection_closed, "QUIC connection expired before stream publication");
   }

   stream->cancel_worker_started = true;
   background_jobs.fetch_add(1, std::memory_order_release);
   try {
      asio::co_spawn(
          strand,
          [shared = std::move(shared), stream]() -> asio::awaitable<void> {
             auto finish = std::unique_ptr<engine_connection::impl, void (*)(engine_connection::impl*)>{
                 shared.get(), [](engine_connection::impl* value) { value->finish_background_job(); }};
             try {
                static_cast<void>(co_await stream->cancel_requested.async_wait(0));
             } catch (...) {
                // Failure to arm the waiter terminalizes this stream on its owner.
             }
             auto cleanup = stream_terminal_owner{nullptr, &release_stream_terminal_owner};
             if (!shared->reset_stream_on_owner(stream, cleanup)) {
                co_return;
             }
             const auto deadline = std::chrono::steady_clock::now() + detached_write_drain_timeout;
             if (shared->test_failpoint && shared->test_failpoint("stream_reset_after_publish_before_drain")) {
                // Test-only seam: retain the existing cancel worker between
                // reset publication and its native terminal drain.
                while (!shared->closing && !shared->canceled && std::chrono::steady_clock::now() < deadline &&
                       shared->test_failpoint("stream_reset_after_publish_before_drain_wait")) {
                   auto timer = asio::steady_timer{shared->strand};
                   timer.expires_after(std::chrono::milliseconds{1});
                   auto ec = boost::system::error_code{};
                   co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
                }
             }
             co_await shared->drain_stream_reset(stream, deadline);
          },
          asio::detached);
   } catch (...) {
      stream->cancel_worker_started = false;
      finish_background_job();
      throw;
   }
}

void engine_connection::impl::wake_and_clear_streams(bool reset_streams) {
   for (auto& [_, stream] : streams) {
      if (reset_streams) {
         stream->reset = true;
      } else {
         stream->closed = true;
      }
      wake(stream->read_waiters);
      wake(stream->write_waiters);
      release_queued_stream_writes(stream);
      stream->cancel_requested.notify();
      finish_stream_terminal_cleanup(stream);
   }
   update_active_stream_metrics();
}

void engine_connection::impl::deliver_closed_hook_if_idle() noexcept {
   if (!closed_hook_called || closed_hook_delivered || background_jobs.load(std::memory_order_acquire) != 0 ||
       !closed_hook) {
      return;
   }
   closed_hook_delivered = true;
   if (auto shared = self.lock()) {
      try {
         closed_hook(std::move(shared));
      } catch (...) {
      }
   }
}

void engine_connection::impl::notify_closed_once() noexcept {
   if (!closed_hook_called) {
      closed_hook_called = true;
   }
   deliver_closed_hook_if_idle();
}

void engine_connection::impl::finish_background_job() noexcept {
   const auto previous = background_jobs.fetch_sub(1, std::memory_order_acq_rel);
   if (previous == 0) {
      background_jobs.store(0, std::memory_order_release);
      return;
   }
   if (previous == 1) {
      wake(background_waiters);
      if (close_completion_pending) {
         complete_close();
      }
      deliver_closed_hook_if_idle();
   }
}

boost::asio::awaitable<void> engine_connection::impl::wait_background_idle() {
   assert(strand.running_in_this_thread());
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   co_await asio::dispatch(strand, asio::use_awaitable);
   while (background_jobs.load(std::memory_order_acquire) != 0) {
      if (test_failpoint) {
         static_cast<void>(test_failpoint("background_join_before_waiter_allocation"));
      }
      auto timer = std::make_shared<asio::steady_timer>(strand);
      timer->expires_after(std::chrono::minutes{10});
      background_waiters.emplace_back(timer);
      boost::system::error_code ec;
      co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
      co_await asio::dispatch(strand, asio::use_awaitable);
   }
}

boost::asio::awaitable<void>
engine_connection::impl::async_close_on_owner(std::chrono::steady_clock::time_point deadline, bool cleanup_only) {
   assert(strand.running_in_this_thread());
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   auto* connection = this;
   auto primary_error = std::exception_ptr{};
   auto canceled_discard = false;
   auto prefix = connection->udp_enqueued_generation;
   auto prefix_captured = false;
   if (cleanup_only) {
      if (!connection->terminal_cleanup_complete) {
         connection->fail_all();
      }
   } else {
      connection->metrics.connections_closed.fetch_add(1, std::memory_order_relaxed);
      try {
         co_await connection->drain_send(deadline);
         prefix = connection->udp_enqueued_generation;
         prefix_captured = true;
         if (connection->test_failpoint) {
            static_cast<void>(connection->test_failpoint("async_close_after_udp_prefix_capture"));
         }
         co_await connection->wait_udp_send_prefix(prefix, deadline);
         if (connection->canceled || connection->cancellation_requested.load(std::memory_order_acquire)) {
            throw_engine(engine_error_kind::canceled, "QUIC graceful close canceled during UDP drain");
         }
         if (connection->closing && !connection->terminal_cleanup_complete) {
            throw_engine(engine_error_kind::connection_closed, "QUIC peer closed during graceful UDP drain");
         }
         if (!connection->terminal_cleanup_complete) {
            connection->closing = true;
            connection->signal_terminal();
            if (connection->test_failpoint) {
               static_cast<void>(connection->test_failpoint("async_close_before_send"));
            }
            if (connection->conn != nullptr) {
               auto packet = std::array<std::uint8_t, max_udp_payload_size>{};
               auto path = ngtcp2_path_storage{};
               ngtcp2_path_storage_zero(&path);
               auto packet_info = ngtcp2_pkt_info{};
               auto close_error = ngtcp2_ccerr{};
               ngtcp2_ccerr_default(&close_error);
               ngtcp2_ccerr_set_application_error(&close_error, 0, nullptr, 0);
               const auto written = ngtcp2_conn_write_connection_close(
                   connection->conn, &path.path, &packet_info, packet.data(), packet.size(), &close_error, timestamp());
               if (written > 0) {
                  const auto packet_size = static_cast<std::size_t>(written);
                  const auto send_error = co_await connection->send_packet(
                      {.bytes = {packet.begin(), packet.begin() + written}, .route = copy_route(path.path)}, true,
                      deadline);
                  if (send_error) {
                     throw boost::system::system_error{send_error, "QUIC CONNECTION_CLOSE send failed"};
                  }
                  connection->metrics.packets_sent.fetch_add(1, std::memory_order_relaxed);
                  connection->metrics.bytes_sent.fetch_add(packet_size, std::memory_order_relaxed);
               } else if (written < 0) {
                  throw_engine(engine_error_kind::internal_error,
                               std::string{"QUIC CONNECTION_CLOSE encoding failed: "} +
                                   ngtcp2_strerror(static_cast<int>(written)));
               }
            }
            if (connection->cancellation_requested.load(std::memory_order_acquire) || connection->canceled) {
               throw_engine(engine_error_kind::canceled, "QUIC graceful close canceled during close send");
            }
            connection->close_transport(!connection->server_side);
         }
      } catch (const boost::system::system_error& error) {
         if (!prefix_captured) {
            prefix = connection->udp_enqueued_generation;
         }
         // Native remote close may discard queued control output. Only
         // its typed abort is cleanup; FIN and real socket failures stay strict.
         if (error.code() == asio::error::operation_aborted && connection->udp_send_discarded &&
             connection->udp_send_error == asio::error::operation_aborted && !connection->udp_send_exception &&
             connection->udp_failed_generation != 0 && connection->udp_failed_generation <= prefix &&
             connection->cancellation_requested.load(std::memory_order_acquire)) {
            canceled_discard = true;
            primary_error = std::current_exception();
            connection->fail_all();
         } else if (error.code() != asio::error::operation_aborted || !connection->native_remote_close_received ||
                    !connection->udp_send_discarded || !connection->terminal_cleanup_complete || connection->canceled ||
                    connection->cancellation_requested.load(std::memory_order_acquire)) {
            primary_error = std::current_exception();
            connection->fail_all();
         }
      } catch (...) {
         if (!prefix_captured) {
            prefix = connection->udp_enqueued_generation;
         }
         primary_error = std::current_exception();
         connection->fail_all();
      }
   }
   auto background_joined = false;
   try {
      co_await connection->wait_background_idle();
      background_joined = true;
   } catch (...) {
      primary_error = std::current_exception();
      canceled_discard = false;
      connection->fail_all();
   }
   // A failed join returns its original error to the native completion callback.
   // That callback caches it but cannot wake the prearmed callers until jobs reach zero.
   if (background_joined && (!primary_error || canceled_discard) && !cleanup_only) {
      try {
         if (connection->udp_failed_generation != 0 && connection->udp_failed_generation <= prefix &&
             !connection->udp_send_discarded) {
            co_await connection->wait_udp_send_prefix(prefix, deadline);
         }
         if (canceled_discard || connection->cancellation_requested.load(std::memory_order_acquire) ||
             connection->canceled) {
            throw_engine(engine_error_kind::canceled, "QUIC graceful close canceled before cleanup joined");
         }
      } catch (...) {
         primary_error = std::current_exception();
      }
   }
   if (primary_error) {
      std::rethrow_exception(primary_error);
   }
}

void engine_connection::impl::complete_close(std::exception_ptr error) noexcept {
   assert(strand.running_in_this_thread());
   if (!close_error && error) {
      close_error = std::move(error);
   }
   // Published native callbacks retain this owner through the last job. A
   // failed join waiter must not publish a terminal barrier before those callbacks.
   if (close_work_active || background_jobs.load(std::memory_order_acquire) != 0) {
      close_completion_pending = true;
      return;
   }
   close_completion_pending = false;
   close_cleanup_complete = true;
   termination_changed.notify();
   // All terminal waits are already installed. Wake cannot allocate/retry;
   // ignoring a native wake failure would strand an admitted close owner.
   try {
      static_cast<void>(close_ready_timer.cancel());
   } catch (...) {
      std::terminate();
   }
}

boost::asio::awaitable<void> engine_connection::impl::wait_close_cleanup() {
   assert(strand.running_in_this_thread());
   if (!close_cleanup_complete) {
      auto error = boost::system::error_code{};
      co_await async_wait_close([](const auto&) {}, asio::redirect_error(asio::use_awaitable, error));
      if (error != asio::error::operation_aborted) {
         throw boost::system::system_error{error ? error : asio::error::fault, "QUIC terminal wait failed"};
      }
   }
   if (close_error) {
      std::rethrow_exception(close_error);
   }
}

void engine_connection::impl::cancel_transport_io(bool close_socket) {
   assert(strand.running_in_this_thread());
   boost::system::error_code ignored;
   if (close_socket && socket) {
      send_gate.close();
      socket->cancel(ignored);
      socket->close(ignored);
   }
   try {
      handshake_timer.cancel();
   } catch (...) {
      // Continue draining the remaining transport work.
   }
   try {
      expiry_timer.cancel();
   } catch (...) {
      // Continue draining the remaining transport work.
   }
   try {
      owner_drain_timer.cancel();
   } catch (...) {
      // Continue draining the remaining transport work.
   }
}

void engine_connection::impl::signal_terminal() noexcept {
   terminal_signaled.store(true, std::memory_order_release);
   termination_changed.notify();
}

void engine_connection::impl::fail_all() noexcept {
   assert(strand.running_in_this_thread());
   if (terminal_cleanup_complete) {
      return;
   }
   signal_terminal();
   canceled = true;
   closing = true;
   pending_client_token.reset();
   metrics.closed.store(true, std::memory_order_relaxed);
   clear_queued_work();
   cancel_transport_io(!server_side);
   wake(handshake_waiters);
   wake(accept_stream_waiters);
   wake(open_stream_waiters);
   wake_and_clear_streams(true);
   complete_drain_requests(drain_request_generation);
   {
      auto lock = std::scoped_lock{inbound_admission_mutex};
      inbound_admission.reset();
   }
   notify_closed_once();
   terminal_cleanup_complete = true;
}

void engine_connection::impl::fail_udp(boost::system::error_code error) noexcept {
   assert(strand.running_in_this_thread());
   if (!handshake_done && !closing && !canceled && !handshake_terminal_cause &&
       error != asio::error::operation_aborted) {
      handshake_transport_error = error;
      if (error == asio::error::connection_refused || error == asio::error::connection_reset ||
          error == asio::error::network_unreachable || error == asio::error::host_unreachable ||
          error == asio::error::network_down) {
         handshake_terminal_cause = engine_error_kind::connection_closed;
      } else if (error == asio::error::timed_out) {
         handshake_terminal_cause = engine_error_kind::handshake_timeout;
      } else {
         handshake_terminal_cause = engine_error_kind::internal_error;
      }
   }
   fail_all();
}

void engine_connection::impl::close_transport(bool cancel_socket) {
   assert(strand.running_in_this_thread());
   if (terminal_cleanup_complete) {
      return;
   }
   signal_terminal();
   closing = true;
   pending_client_token.reset();
   metrics.closed.store(true, std::memory_order_relaxed);
   clear_queued_work();
   cancel_transport_io(cancel_socket);
   wake(handshake_waiters);
   wake(accept_stream_waiters);
   wake(open_stream_waiters);
   wake_and_clear_streams(false);
   complete_drain_requests(drain_request_generation);
   {
      auto lock = std::scoped_lock{inbound_admission_mutex};
      inbound_admission.reset();
   }
   notify_closed_once();
   terminal_cleanup_complete = true;
}

void engine_connection::impl::verify_selected_alpn(std::string_view expected) {
   const unsigned char* selected = nullptr;
   unsigned int selected_len = 0;
   SSL_get0_alpn_selected(ssl.get(), &selected, &selected_len);
   if (selected_len != expected.size() || selected == nullptr ||
       std::memcmp(selected, expected.data(), selected_len) != 0) {
      throw_engine(engine_error_kind::alpn_mismatch, "QUIC ALPN mismatch");
   }
}

void engine_connection::impl::verify_peer(const engine_security_options& security) {
   assert(strand.running_in_this_thread());
   peer_certificate_value.reset();
   if (security.verify_peer && server_side && SSL_get_peer_cert_chain(ssl.get()) == nullptr) {
      throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer did not present a certificate");
   }
   auto cert = x509_ptr{SSL_get1_peer_certificate(ssl.get())};
   if (!cert) {
      if (security.verify_peer) {
         throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer did not present a certificate");
      }
      return;
   }
   auto der = der_from_certificate(cert.get());
   auto peer = engine_peer_certificate{.der = std::move(der)};
   peer.sha256_fingerprint = engine_sha256_fingerprint(peer.der);
   if (security.verify_peer) {
      if (security.expected_sha256_fingerprint &&
          peer.sha256_fingerprint != normalize_engine_sha256_fingerprint(*security.expected_sha256_fingerprint)) {
         throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer certificate fingerprint mismatch");
      }
      if (security.verifier && !security.verifier(peer)) {
         throw_engine(engine_error_kind::peer_verification_failed, "QUIC peer verifier rejected certificate");
      }
   }
   peer_certificate_value = std::move(peer);
}

void engine_connection::impl::complete_handshake() {
   assert(strand.running_in_this_thread());
   if (handshake_done) {
      return;
   }
   handshake_done = true;
   try {
      handshake_timer.cancel();
   } catch (...) {
   }
   metrics.handshakes_completed.fetch_add(1, std::memory_order_relaxed);
   wake(handshake_waiters);
   if (handshake_completed_hook) {
      handshake_completed_hook();
   }
}

void engine_connection::impl::commit_pending_client_token() noexcept {
   assert(strand.running_in_this_thread());
   if (!client_token_store_verified || !pending_client_token || !client_token_store) {
      return;
   }
   auto token = std::move(*pending_client_token);
   pending_client_token.reset();
   try {
      client_token_store(std::move(token));
   } catch (...) {
      // Cache failures must not affect a verified QUIC connection.
   }
}

boost::asio::awaitable<void> engine_connection::impl::wait_handshake(std::chrono::milliseconds timeout) {
   assert(strand.running_in_this_thread());
   co_await asio::dispatch(strand, asio::use_awaitable);
   if (handshake_done) {
      co_return;
   }
   if (!closing && !canceled) {
      auto timer = std::make_shared<asio::steady_timer>(strand);
      timer->expires_after(timeout);
      handshake_waiters.emplace_back(timer);
      boost::system::error_code ec;
      co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
   }
   if (handshake_done) {
      co_return;
   }
   if (handshake_terminal_cause) {
      if (test_failpoint) {
         // Observation barrier only: retain the real socket failure and let
         // the outer active_connect arbitration decide the terminal winner.
         static_cast<void>(test_failpoint("handshake_udp_failure_before_report"));
      }
      throw_engine(*handshake_terminal_cause, "QUIC handshake UDP failure: " + handshake_transport_error.message());
   }
   if (canceled && metrics.backpressure_rejections.load(std::memory_order_relaxed) > 0) {
      throw_engine(engine_error_kind::backpressure_rejected, "QUIC handshake stopped by inbound packet backpressure");
   }
   if (canceled) {
      throw_engine(engine_error_kind::canceled, "QUIC handshake was canceled");
   }
   if (closing) {
      throw_engine(engine_error_kind::connection_closed, "QUIC connection closed before handshake completed");
   }
   metrics.handshakes_failed.fetch_add(1, std::memory_order_relaxed);
   metrics.timeouts.fetch_add(1, std::memory_order_relaxed);
   throw_engine(engine_error_kind::handshake_timeout, "QUIC handshake timed out");
}

asio::awaitable<boost::system::error_code> engine_connection::impl::send_packet_impl(server_udp_socket::packet packet) {
   try {
      auto ticket = co_await send_gate.acquire();
      if (server_socket) {
         co_return co_await server_socket->async_send(std::move(packet));
      }
      auto ec = boost::system::error_code{};
      const auto written =
          co_await socket->async_send(asio::buffer(packet.bytes), asio::redirect_error(asio::use_awaitable, ec));
      if (!ec && written != packet.bytes.size()) {
         ec = asio::error::message_size;
      }
      co_return ec;
   } catch (const forge::asio::exceptions::canceled&) {
      co_return asio::error::operation_aborted;
   } catch (const forge::asio::exceptions::rejected&) {
      co_return asio::error::operation_aborted;
   }
}

asio::awaitable<void> engine_connection::impl::wait_send_terminal() {
   auto observed = termination_changed.epoch();
   while (!terminal_signaled.load(std::memory_order_acquire)) {
      observed = co_await termination_changed.async_wait(observed);
   }
}

asio::awaitable<boost::system::error_code>
engine_connection::impl::send_packet(server_udp_socket::packet packet, bool closing_packet,
                                     std::optional<std::chrono::steady_clock::time_point> deadline_at) {
   // The dedicated client owns one immutable connected path. Never silently
   // send a packet on a different path than ngtcp2 requested.
   if (!server_socket && (packet.route.local != local_endpoint_value || packet.route.remote != remote_endpoint)) {
      throw_engine(engine_error_kind::internal_error, "QUIC client output changed its fixed UDP path");
   }
   using namespace asio::experimental::awaitable_operators;
   auto deadline = asio::steady_timer{strand};
   deadline.expires_at(deadline_at.value_or(std::chrono::steady_clock::now() + detached_write_drain_timeout));
   // Structured races join the canceled branch, including shared-socket gate
   // acquisition/readiness. Never cancel the listener FD for one connection.
   if (closing_packet) {
      auto result = co_await (send_packet_impl(std::move(packet)) || deadline.async_wait(asio::use_awaitable));
      co_return result.index() == 0 ? std::get<0>(result) : asio::error::timed_out;
   }
   auto result = co_await (send_packet_impl(std::move(packet)) || deadline.async_wait(asio::use_awaitable) ||
                           wait_send_terminal());
   if (result.index() == 0) {
      co_return std::get<0>(result);
   }
   co_return result.index() == 1 ? asio::error::timed_out : asio::error::operation_aborted;
}

void engine_connection::impl::fail_udp_send(boost::system::error_code error, std::exception_ptr exception,
                                            bool discarded) noexcept {
   assert(strand.running_in_this_thread());
   if (udp_failed_generation == 0 || (udp_send_discarded && !discarded)) {
      if (udp_failed_generation == 0) {
         udp_failed_generation = udp_completed_generation + 1;
      }
      udp_send_error = error;
      udp_send_exception = std::move(exception);
      udp_send_discarded = discarded;
   }
   udp_send_changed.notify();
}

void engine_connection::impl::finish_udp_send_loop() noexcept {
   udp_inflight_generation = 0;
   udp_send_active = false;
   udp_send_changed.notify();
}

asio::awaitable<void> engine_connection::impl::wait_udp_send_prefix(std::uint64_t generation,
                                                                    std::chrono::steady_clock::time_point deadline,
                                                                    std::shared_ptr<engine_stream::impl> stream) {
   assert(strand.running_in_this_thread());
   if (generation > udp_enqueued_generation) {
      throw_engine(engine_error_kind::internal_error, "QUIC UDP drain captured an unqueued generation");
   }
   while (true) {
      const auto observed = udp_send_changed.epoch();
      if (udp_failed_generation != 0 && udp_failed_generation <= generation) {
         if (udp_send_exception) {
            std::rethrow_exception(udp_send_exception);
         }
         throw boost::system::system_error{udp_send_error, "QUIC UDP send failed before captured drain"};
      }
      if (stream && stream->reset) {
         throw_engine(engine_error_kind::stream_reset, "QUIC stream reset during FIN send drain");
      }
      if (udp_completed_generation >= generation) {
         co_return;
      }
      if (canceled || cancellation_requested.load(std::memory_order_acquire)) {
         throw_engine(engine_error_kind::canceled, "QUIC UDP send drain canceled");
      }
      if (closing || terminal_cleanup_complete) {
         throw_engine(engine_error_kind::connection_closed, "QUIC connection closed before captured UDP drain");
      }
      if (std::chrono::steady_clock::now() >= deadline) {
         throw boost::system::system_error{asio::error::timed_out, "QUIC UDP send drain timed out"};
      }
      static_cast<void>(co_await udp_send_changed.async_wait_until(observed, deadline));
   }
}

asio::awaitable<void> engine_connection::impl::wait_udp_send_idle() {
   assert(strand.running_in_this_thread());
   while (udp_send_active) {
      const auto observed = udp_send_changed.epoch();
      if (udp_send_active) {
         static_cast<void>(co_await udp_send_changed.async_wait(observed));
      }
   }
}

void engine_connection::impl::start_udp_send_loop() {
   assert(strand.running_in_this_thread());
   if (udp_send_active || closing || canceled) {
      return;
   }
   if (self.expired()) {
      return;
   }
   udp_send_active = true;
   auto finish =
       std::unique_ptr<impl, void (*)(impl*)>{this, [](impl* owner) noexcept { owner->finish_udp_send_loop(); }};
   try {
      spawn_background([owned = std::pair{self.lock(), std::move(finish)}](
                           const std::shared_ptr<impl>& value) mutable -> asio::awaitable<void> {
         while (!value->closing && !value->canceled) {
            if (value->outbound_datagrams.empty()) {
               break;
            }
            auto packet = std::move(value->outbound_datagrams.front());
            value->outbound_datagrams.pop_front();
            value->udp_inflight_generation = value->udp_completed_generation + 1;
            value->udp_send_changed.notify();
            if (value->queued_datagram_bytes >= packet.bytes.size()) {
               value->queued_datagram_bytes -= packet.bytes.size();
            } else {
               value->queued_datagram_bytes = 0;
            }

            const auto packet_size = packet.bytes.size();
            auto ec = boost::system::error_code{};
            try {
               ec = co_await value->send_packet(std::move(packet));
            } catch (...) {
               value->fail_udp_send(asio::error::fault, std::current_exception());
               value->fail_all();
               co_return;
            }
            if (ec) {
               value->fail_udp_send(ec, {},
                                    ec == asio::error::operation_aborted &&
                                        value->terminal_signaled.load(std::memory_order_acquire));
               if (!value->closing) {
                  value->fail_udp(ec);
               }
               break;
            }
            value->metrics.packets_sent.fetch_add(1, std::memory_order_relaxed);
            value->metrics.bytes_sent.fetch_add(packet_size, std::memory_order_relaxed);
            value->udp_completed_generation = value->udp_inflight_generation;
            value->udp_inflight_generation = 0;
            value->udp_send_changed.notify();
         }
         owned.second.reset();
         if (!value->outbound_datagrams.empty() && !value->closing && !value->canceled) {
            value->start_udp_send_loop();
         }
      });
   } catch (...) {
      fail_udp_send(asio::error::fault, std::current_exception());
      finish_udp_send_loop();
      fail_all();
      throw;
   }
}

void engine_connection::impl::enqueue_datagram(std::span<const std::uint8_t> packet, const ngtcp2_path& path) {
   if (packet.empty()) {
      return;
   }
   if (closing || canceled || udp_failed_generation != 0) {
      throw_engine(engine_error_kind::connection_closed, "QUIC UDP output is closed");
   }
   if (udp_enqueued_generation == (std::numeric_limits<std::uint64_t>::max)()) {
      fail_all();
      throw_engine(engine_error_kind::internal_error, "QUIC UDP send generation exhausted");
   }
   if (queued_datagram_bytes + packet.size() > max_queued_datagram_bytes) {
      metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
      fail_all();
      throw_engine(engine_error_kind::backpressure_rejected, "QUIC UDP datagram queue exceeds limit");
   }
   outbound_datagrams.push_back({.bytes = {packet.begin(), packet.end()}, .route = copy_route(path)});
   ++udp_enqueued_generation;
   queued_datagram_bytes += packet.size();
   udp_send_changed.notify();
   start_udp_send_loop();
}

void engine_connection::impl::request_packet_processing() {
   if (packet_processing_active || drain_active || inbound_packets.empty()) {
      return;
   }
   if (self.expired()) {
      return;
   }
   spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
      try {
         co_await value->process_queued_packets();
      } catch (const engine_failure&) {
         value->fail_all();
      }
   });
}

void engine_connection::impl::request_expiry_processing() {
   if (self.expired()) {
      return;
   }
   spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
      try {
         co_await value->handle_expiry_event();
      } catch (const engine_failure&) {
         value->fail_all();
      }
   });
}

void engine_connection::impl::schedule_post_ngtcp2_work() {
   if (expiry_event_pending && !drain_active && !packet_processing_active) {
      request_expiry_processing();
   }
   if (!inbound_packets.empty() && !drain_active && !packet_processing_active) {
      request_packet_processing();
   }
}

void engine_connection::impl::schedule_expiry() {
   assert(strand.running_in_this_thread());
   if (conn == nullptr || closing) {
      return;
   }
   const auto expiry = ngtcp2_conn_get_expiry(conn);
   if (expiry == (std::numeric_limits<ngtcp2_tstamp>::max)()) {
      expiry_timer.cancel();
      return;
   }
   const auto now = timestamp();
   const auto delay = expiry <= now ? std::chrono::nanoseconds{1} : std::chrono::nanoseconds{expiry - now};
   expiry_timer.expires_after(delay);
   auto shared = self.lock();
   if (!shared) {
      return;
   }
   background_jobs.fetch_add(1, std::memory_order_release);
   try {
      expiry_timer.async_wait([shared](boost::system::error_code ec) {
         if (ec) {
            shared->finish_background_job();
            return;
         }
         try {
            asio::co_spawn(
                shared->strand,
                [shared]() -> asio::awaitable<void> {
                   const auto finish = [shared](impl*) noexcept { shared->finish_background_job(); };
                   auto guard = std::unique_ptr<impl, decltype(finish)>{shared.get(), finish};
                   try {
                      if (shared->test_failpoint && shared->test_failpoint("expiry_worker_failure")) {
                         throw std::runtime_error{"QUIC test failpoint: expiry worker failure"};
                      }
                      co_await shared->handle_expiry_event();
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
      finish_background_job();
      fail_all();
   }
}

boost::asio::awaitable<void> engine_connection::impl::handle_expiry_event() {
   assert(strand.running_in_this_thread());
   co_await asio::dispatch(strand, asio::use_awaitable);
   if (conn == nullptr || closing) {
      co_return;
   }
   if (drain_active || packet_processing_active) {
      expiry_event_pending = true;
      co_return;
   }
   expiry_event_pending = false;
   const auto rv = ngtcp2_conn_handle_expiry(conn, timestamp());
   if (rv == NGTCP2_ERR_IDLE_CLOSE) {
      close_transport(!server_side);
      co_return;
   }
   if (rv != 0) {
      fail_all();
      co_return;
   }
   co_await drain_send();
}

[[nodiscard]] std::vector<std::shared_ptr<engine_stream::impl>> engine_connection::impl::writable_streams() {
   auto out = std::vector<std::shared_ptr<engine_stream::impl>>{};
   out.reserve(streams.size());
   for (auto& [_, stream] : streams) {
      if (!stream->outbound.empty() && !stream->local_write_closed && !stream->reset) {
         out.push_back(stream);
      }
   }
   std::ranges::sort(out, {}, &engine_stream::impl::id);
   const auto next = std::ranges::upper_bound(out, last_writable_stream_id, {}, &engine_stream::impl::id);
   std::ranges::rotate(out, next);
   return out;
}

void engine_connection::impl::reject_unwritable_stream(const std::shared_ptr<engine_stream::impl>& stream,
                                                       ngtcp2_ssize error) {
   if (!stream) {
      return;
   }
   release_queued_stream_writes(stream);
   stream->local_write_closed = true;
   if (error == NGTCP2_ERR_STREAM_NOT_FOUND) {
      stream->closed = true;
   }
   finish_stream_terminal_cleanup(stream);
   wake(stream->write_waiters);
   update_active_stream_metrics();
}

void engine_connection::impl::mark_stream_data_submitted(std::shared_ptr<engine_stream::impl>& stream,
                                                         ngtcp2_ssize data_len) {
   if (!stream || data_len <= 0) {
      return;
   }
   auto& write = stream->outbound.front();
   if (!write.base_offset_set) {
      write.base_offset = stream->send_next_offset;
      write.base_offset_set = true;
   }
   write.submitted += static_cast<std::size_t>(data_len);
   stream->send_next_offset += static_cast<std::uint64_t>(data_len);
   complete_submitted_writes(stream);
}

void engine_connection::impl::complete_submitted_writes(std::shared_ptr<engine_stream::impl>& stream) {
   while (!stream->outbound.empty()) {
      auto& write = stream->outbound.front();
      if (write.submitted < write.data.size() || (!write.fin && write.data.empty()) ||
          (write.fin && write.data.empty() && !write.base_offset_set)) {
         break;
      }
      if (!write.data.empty()) {
         stream->retained.push_back(engine_stream::impl::retained_write{
             .data = std::move(write.data),
             .lifetime = std::move(write.lifetime),
             .base_offset = write.base_offset,
             .fin = write.fin,
         });
      }
      if (write.fin) {
         stream->local_write_closed = true;
         finish_stream_terminal_cleanup(stream);
      }
      wake(write.waiters);
      stream->outbound.pop_front();
   }
   wake(stream->write_waiters);
   update_active_stream_metrics();
}

void engine_connection::impl::complete_drain_requests(std::uint64_t generation) noexcept {
   assert(strand.running_in_this_thread());
   if (generation <= drain_completed_generation) {
      return;
   }
   drain_completed_generation = generation;
   drain_completion_changed.notify();
}

boost::asio::awaitable<void>
engine_connection::impl::wait_for_drain_requests(std::uint64_t generation,
                                                 std::optional<std::chrono::steady_clock::time_point> deadline) {
   assert(strand.running_in_this_thread());
   while (drain_completed_generation < generation) {
      const auto observed = drain_completion_changed.epoch();
      if (drain_completed_generation < generation) {
         if (deadline) {
            if (std::chrono::steady_clock::now() >= *deadline) {
               throw boost::system::system_error{asio::error::timed_out, "QUIC native drain timed out"};
            }
            static_cast<void>(co_await drain_completion_changed.async_wait_until(observed, *deadline));
         } else {
            static_cast<void>(co_await drain_completion_changed.async_wait(observed));
         }
      }
   }
}

void engine_connection::impl::finish_active_drain() noexcept {
   assert(strand.running_in_this_thread());
   drain_active = false;
   // A connection-level terminal state supersedes native stream draining.
   complete_drain_requests(drain_request_generation);
}

boost::asio::awaitable<void>
engine_connection::impl::drain_send(std::optional<std::chrono::steady_clock::time_point> deadline) {
   assert(strand.running_in_this_thread());
   co_await asio::dispatch(strand, asio::use_awaitable);
   if (closing || canceled || conn == nullptr) {
      co_return;
   }
   if (drain_active) {
      const auto generation = ++drain_request_generation;
      drain_requested = true;
      if (test_failpoint) {
         static_cast<void>(test_failpoint("drain_request_before_completion_wait"));
      }
      co_await wait_for_drain_requests(generation, deadline);
      co_return;
   }

   drain_active = true;
   auto clear =
       std::unique_ptr<void, void (*)(void*)>{this, [](void* ptr) { static_cast<impl*>(ptr)->finish_active_drain(); }};
   if (test_failpoint && test_failpoint("drain_after_owner_claim_before_native_write")) {
      // Test-only seam: pause the existing drain owner after its claim.
      while (!closing && !canceled && test_failpoint("drain_after_owner_claim_before_native_write_wait")) {
         if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            throw boost::system::system_error{asio::error::timed_out, "QUIC native drain timed out"};
         }
         auto timer = asio::steady_timer{strand};
         timer.expires_after(std::chrono::milliseconds{1});
         auto ec = boost::system::error_code{};
         co_await timer.async_wait(asio::redirect_error(asio::use_awaitable, ec));
      }
   }

   do {
      if (closing || canceled || conn == nullptr) {
         break;
      }
      drain_requested = false;
      const auto completed_generation = drain_request_generation;
      auto packets_this_drain = std::size_t{0};
      for (;;) {
         if (deadline && std::chrono::steady_clock::now() >= *deadline) {
            throw boost::system::system_error{asio::error::timed_out, "QUIC native drain timed out"};
         }
         auto packet = std::array<std::uint8_t, max_udp_payload_size>{};
         auto ps = ngtcp2_path_storage{};
         ngtcp2_path_storage_zero(&ps);
         auto pi = ngtcp2_pkt_info{};
         const auto packet_ts = timestamp();
         auto nwrite = ngtcp2_ssize{0};
         auto selected = std::shared_ptr<engine_stream::impl>{};
         auto data_len = ngtcp2_ssize{0};
         auto candidates = writable_streams();
         auto candidate = candidates.begin();

         for (;;) {
            data_len = 0;
            auto flags = std::uint32_t{0};
            auto stream_id = std::int64_t{-1};
            auto datav = ngtcp2_vec{};
            auto datavcnt = std::size_t{0};
            selected = candidate == candidates.end() ? std::shared_ptr<engine_stream::impl>{} : *candidate++;
            if (selected) {
               last_writable_stream_id = selected->id;
               auto& write = selected->outbound.front();
               stream_id = selected->id;
               const auto remaining = write.data.size() - write.submitted;
               datav.base = remaining == 0 ? nullptr : write.data.data() + write.submitted;
               datav.len = remaining;
               datavcnt = (remaining > 0 || write.fin) ? 1 : 0;
               if (write.fin && remaining == 0) {
                  flags |= NGTCP2_WRITE_STREAM_FLAG_FIN;
               }
            }

            nwrite = ngtcp2_conn_writev_stream(conn, &ps.path, &pi, packet.data(), packet.size(), &data_len, flags,
                                               stream_id, datavcnt == 0 ? nullptr : &datav, datavcnt, packet_ts);

            if (nwrite == NGTCP2_ERR_STREAM_DATA_BLOCKED) {
               continue;
            }
            if (nwrite == NGTCP2_ERR_STREAM_SHUT_WR || nwrite == NGTCP2_ERR_STREAM_NOT_FOUND) {
               reject_unwritable_stream(selected, nwrite);
               continue;
            }
            if (nwrite != NGTCP2_ERR_WRITE_MORE) {
               break;
            }

            mark_stream_data_submitted(selected, data_len);
         }

         if (nwrite < 0) {
            fail_all();
            throw_engine(engine_error_kind::internal_error,
                         std::string{"ngtcp2_conn_writev_stream failed: "} + ngtcp2_strerror(static_cast<int>(nwrite)));
         }
         if (nwrite == 0) {
            break;
         }
         const auto submitted_fin =
             selected && data_len >= 0 && !selected->outbound.empty() && selected->outbound.front().fin;
         if (selected && data_len >= 0) {
            if (data_len == 0 && !selected->outbound.empty()) {
               auto& write = selected->outbound.front();
               if (write.fin) {
                  write.base_offset = selected->send_next_offset;
                  write.base_offset_set = true;
                  write.submitted = write.data.size();
               }
            }
            mark_stream_data_submitted(selected, data_len);
            complete_submitted_writes(selected);
         }
         ngtcp2_conn_update_pkt_tx_time(conn, timestamp());
         enqueue_datagram({packet.data(), static_cast<std::size_t>(nwrite)}, ps.path);
         if (submitted_fin) {
            selected->fin_send_generation = udp_enqueued_generation;
            udp_send_changed.notify();
         }
         if (test_failpoint) {
            static_cast<void>(test_failpoint("drain_after_udp_enqueue"));
         }
         ++packets_this_drain;
         if (packets_this_drain >= max_packets_per_drain) {
            drain_requested = true;
            co_await asio::post(strand, asio::use_awaitable);
            if (closing || canceled || conn == nullptr) {
               drain_requested = false;
            }
            break;
         }
      }
      complete_drain_requests(completed_generation);
   } while (drain_requested && !closing && !canceled && conn != nullptr);

   clear.reset();
   if (!closing && !canceled && conn != nullptr) {
      schedule_expiry();
      schedule_post_ngtcp2_work();
   }
}

boost::asio::awaitable<void>
engine_connection::impl::handle_packet(std::vector<std::uint8_t> packet,
                                       forge::net::transport::datagram_io::received route) {
   assert(strand.running_in_this_thread());
   co_await asio::dispatch(strand, asio::use_awaitable);
   start_cancel_request_worker();
   if (closing || canceled) {
      co_return;
   }
   const auto packet_size = packet.size();
   if (inbound_packets.size() >= limits.max_inbound_queued_packets || packet_size > limits.max_inbound_queued_bytes ||
       queued_inbound_packet_bytes > limits.max_inbound_queued_bytes - packet_size) {
      metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
      fail_all();
      throw_engine(engine_error_kind::backpressure_rejected, "QUIC inbound packet queue exceeds limit");
   }
   queued_inbound_packet_bytes += packet_size;
   inbound_packets.push_back(queued_packet{.bytes = std::move(packet), .route = std::move(route)});
   if (!drain_active && !packet_processing_active) {
      co_await process_queued_packets();
   }
}

boost::asio::awaitable<void> engine_connection::impl::process_queued_packets() {
   assert(strand.running_in_this_thread());
   co_await asio::dispatch(strand, asio::use_awaitable);
   if (packet_processing_active || drain_active) {
      co_return;
   }
   packet_processing_active = true;
   auto clear = std::unique_ptr<void, void (*)(void*)>{
       this, [](void* ptr) { static_cast<impl*>(ptr)->packet_processing_active = false; }};
   while (!inbound_packets.empty() && !closing && !canceled) {
      auto queued = std::move(inbound_packets.front());
      inbound_packets.pop_front();
      if (queued_inbound_packet_bytes >= queued.bytes.size()) {
         queued_inbound_packet_bytes -= queued.bytes.size();
      } else {
         queued_inbound_packet_bytes = 0;
      }
      auto path = make_path(queued.route.local, queued.route.remote);
      auto pi = ngtcp2_pkt_info{};
      const auto rv =
          ngtcp2_conn_read_pkt(conn, &path.path, &pi, queued.bytes.data(), queued.bytes.size(), timestamp());
      if (rv == NGTCP2_ERR_DRAINING) {
         // This native result, not canceled/closing flags, identifies peer close.
         native_remote_close_received = true;
         close_transport(!server_side);
         co_return;
      }
      if (rv == NGTCP2_ERR_CLOSING) {
         fail_all();
         co_return;
      }
      if (rv == NGTCP2_ERR_RETRY || rv == NGTCP2_ERR_DROP_CONN) {
         report_accept_failure = false;
         fail_all();
         co_return;
      }
      if (rv != 0) {
         auto message = ngtcp2_read_error_message(conn, rv);
         message += "; packet_size=";
         message += std::to_string(queued.bytes.size());
         fail_all();
         throw_engine(engine_error_kind::internal_error, std::move(message));
      }
      metrics.packets_received.fetch_add(1, std::memory_order_relaxed);
      metrics.bytes_received.fetch_add(queued.bytes.size(), std::memory_order_relaxed);
      co_await drain_send();
   }

   clear.reset();
   schedule_post_ngtcp2_work();
}

void engine_connection::impl::start_client_receive_loop() {
   if (receive_loop_started) {
      return;
   }
   if (self.expired()) {
      return;
   }
   receive_loop_started = true;
   spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
      try {
         while (!value->closing && !value->canceled) {
            auto packet = std::vector<std::uint8_t>(65536);
            boost::system::error_code ec;
            const auto nread = co_await value->socket->async_receive(asio::buffer(packet),
                                                                     asio::redirect_error(asio::use_awaitable, ec));
            if (ec) {
               if (ec != asio::error::operation_aborted && !value->closing) {
                  value->fail_udp(ec);
               }
               co_return;
            }
            packet.resize(nread);
            co_await value->handle_packet(
                std::move(packet),
                {.size = nread, .remote = value->remote_endpoint, .local = value->local_endpoint_value});
         }
      } catch (...) {
         if (!value->closing && !value->canceled) {
            value->fail_all();
         }
         co_return;
      }
   });
}

void engine_connection::impl::start_cancel_request_worker() {
   if (cancel_request_worker_started || self.expired()) {
      return;
   }
   cancel_request_worker_started = true;
   spawn_background([](const std::shared_ptr<impl>& value) -> asio::awaitable<void> {
      auto observed = value->termination_changed.epoch();
      while (!value->terminal_signaled.load(std::memory_order_acquire)) {
         if (value->cancellation_requested.load(std::memory_order_acquire)) {
            if (!value->closing && !value->canceled) {
               value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
               value->fail_all();
            }
            co_return;
         }
         if (value->owner_released.load(std::memory_order_acquire)) {
            if (value->metrics.queued_bytes.load(std::memory_order_relaxed) == 0 || value->owner_drain_timer_expired) {
               if (!value->closing && !value->canceled) {
                  value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                  value->fail_all();
               }
               co_return;
            }
            if (!value->owner_drain_timer_started) {
               const auto released_at = value->owner_released_at.load(std::memory_order_acquire);
               const auto now = timestamp();
               const auto elapsed = now >= released_at ? now - released_at : ngtcp2_tstamp{0};
               const auto timeout = static_cast<ngtcp2_tstamp>(
                   std::chrono::duration_cast<std::chrono::nanoseconds>(detached_write_drain_timeout).count());
               if (elapsed >= timeout) {
                  value->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                  value->fail_all();
                  co_return;
               }
               value->owner_drain_timer_started = true;
               value->owner_drain_timer.expires_after(
                   std::chrono::nanoseconds{static_cast<std::chrono::nanoseconds::rep>(timeout - elapsed)});
               value->spawn_background([](const std::shared_ptr<impl>& connection) -> asio::awaitable<void> {
                  auto error = boost::system::error_code{};
                  co_await connection->owner_drain_timer.async_wait(asio::redirect_error(asio::use_awaitable, error));
                  connection->owner_drain_timer_started = false;
                  connection->owner_drain_timer_expired = !error;
                  connection->termination_changed.notify();
               });
            }
         }
         observed = co_await value->termination_changed.async_wait(observed);
      }
   });
}
} // namespace forge::net::quic::detail
