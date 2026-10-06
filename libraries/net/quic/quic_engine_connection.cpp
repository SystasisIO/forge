#include "details/quic_engine_support.hxx"
#include "details/engine_stream_impl.hxx"
#include "details/engine_connection_impl.hxx"

namespace forge::net::quic::detail {
engine_connection::engine_connection(std::shared_ptr<impl> impl_value) : impl_(std::move(impl_value)) {}

engine_connection::~engine_connection() {
   if (impl_) {
      impl_->owner_released_at.store(timestamp(), std::memory_order_release);
      impl_->owner_released.store(true, std::memory_order_release);
      impl_->termination_changed.notify();
   }
}

engine_connection_metrics engine_connection::metrics() const {
   return impl_ ? impl_->metrics.snapshot() : engine_connection_metrics{};
}

engine_endpoint engine_connection::local_endpoint() const {
   return impl_ ? from_udp_endpoint(impl_->local_endpoint()) : engine_endpoint{};
}

engine_endpoint engine_connection::remote_endpoint() const {
   return impl_ ? from_udp_endpoint(impl_->remote_endpoint) : engine_endpoint{};
}

std::optional<engine_peer_certificate> engine_connection::peer_certificate() const {
   return impl_ ? impl_->peer_certificate_value : std::nullopt;
}

std::shared_ptr<void> engine_connection::take_inbound_admission() noexcept {
   if (!impl_) {
      return {};
   }
   auto lock = std::scoped_lock{impl_->inbound_admission_mutex};
   return std::move(impl_->inbound_admission);
}

boost::asio::awaitable<std::shared_ptr<engine_stream>> engine_connection::async_open_stream() {
   if (!impl_) {
      throw_engine(engine_error_kind::connection_closed, "invalid QUIC connection");
   }
   co_return co_await asio::co_spawn(
       impl_->strand,
       [connection = impl_]() -> asio::awaitable<std::shared_ptr<engine_stream>> {
          auto cancellation = co_await asio::this_coro::cancellation_state;
          if (connection->close_started || connection->closing || connection->canceled) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection is closed");
          }
          if (connection->active_stream_count() >= connection->limits.max_streams_per_connection) {
             connection->metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
             throw_engine(engine_error_kind::backpressure_rejected, "QUIC max streams exceeded");
          }
          auto stream = std::make_shared<engine_stream::impl>(-1);
          stream->connection = connection;
          auto stream_id = std::int64_t{-1};
          while (true) {
             if (connection->close_started || connection->closing || connection->canceled) {
                throw_engine(engine_error_kind::connection_closed, "QUIC stream open admission is closed");
             }
             if (cancellation.cancelled() != asio::cancellation_type::none) {
                connection->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                throw_engine(engine_error_kind::canceled, "QUIC stream open canceled");
             }
             const auto rv = ngtcp2_conn_open_bidi_stream(connection->conn, &stream_id, stream.get());
             if (rv == 0) {
                break;
             }
             if (rv != NGTCP2_ERR_STREAM_ID_BLOCKED) {
                throw_engine(engine_error_kind::backpressure_rejected,
                             std::string{"ngtcp2_conn_open_bidi_stream failed: "} + ngtcp2_strerror(rv));
             }
             auto timer = std::make_shared<asio::steady_timer>(connection->strand);
             timer->expires_after(std::chrono::minutes{10});
             std::erase_if(connection->open_stream_waiters, [](const auto& weak) { return weak.expired(); });
             connection->open_stream_waiters.emplace_back(timer);
             auto error = boost::system::error_code{};
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, error));
             remove_waiter(connection->open_stream_waiters, timer);
             if (cancellation.cancelled() != asio::cancellation_type::none) {
                connection->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
                throw_engine(engine_error_kind::canceled, "QUIC stream open canceled while waiting for credit");
             }
             if (connection->close_started || connection->closing || connection->canceled) {
                throw_engine(engine_error_kind::connection_closed,
                             "QUIC connection closed while waiting for stream credit");
             }
          }

          // Once ngtcp2 allocates the stream ID, completion owns that stream. Make
          // cancellation completion-wins so no unreturned native stream can leak.
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
          stream->id = stream_id;
          connection->streams.emplace(stream_id, stream);
          connection->update_active_stream_metrics();
          connection->metrics.streams_opened.fetch_add(1, std::memory_order_relaxed);
          auto cancel_worker_failure = std::exception_ptr{};
          try {
             connection->start_stream_cancel_worker(stream);
          } catch (...) {
             cancel_worker_failure = std::current_exception();
          }
          if (cancel_worker_failure) {
             co_await connection->async_reset_stream_after_close_failure(stream);
             std::rethrow_exception(cancel_worker_failure);
          }
          co_await connection->drain_send();
          if (connection->close_started || connection->closing || connection->canceled) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection closed while opening stream");
          }
          co_return std::shared_ptr<engine_stream>{new engine_stream{std::move(stream)}};
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::shared_ptr<engine_stream>> engine_connection::async_accept_stream() {
   if (!impl_) {
      throw_engine(engine_error_kind::connection_closed, "invalid QUIC connection");
   }
   co_return co_await asio::co_spawn(
       impl_->strand,
       [connection = impl_]() -> asio::awaitable<std::shared_ptr<engine_stream>> {
          while (connection->accepted_streams.empty() && !connection->close_started && !connection->closing &&
                 !connection->canceled) {
             auto timer = std::make_shared<asio::steady_timer>(connection->strand);
             timer->expires_after(std::chrono::minutes{10});
             connection->accept_stream_waiters.emplace_back(timer);
             boost::system::error_code ec;
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
          }
          if (connection->close_started || connection->accepted_streams.empty()) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection closed before accepting stream");
          }
          auto stream = std::move(connection->accepted_streams.front());
          connection->accepted_streams.pop_front();
          auto cancel_worker_failure = std::exception_ptr{};
          try {
             connection->start_stream_cancel_worker(stream);
          } catch (...) {
             cancel_worker_failure = std::current_exception();
          }
          if (cancel_worker_failure) {
             co_await connection->async_reset_stream_after_close_failure(stream);
             std::rethrow_exception(cancel_worker_failure);
          }
          co_return std::shared_ptr<engine_stream>{new engine_stream{std::move(stream)}};
       },
       asio::use_awaitable);
}

boost::asio::awaitable<void> engine_connection::async_close() {
   if (!impl_) {
      co_return;
   }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   const auto deadline = std::chrono::steady_clock::now() + detached_write_drain_timeout;
   co_await asio::co_spawn(
       impl_->strand,
       [connection = impl_, deadline]() -> asio::awaitable<void> {
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
          if (connection->close_started) {
             co_await connection->wait_close_cleanup();
             co_return;
          }
          // A prior host cancellation/terminal transition admits cleanup, not
          // a new graceful send operation. Cancellation after this latch fails it.
          const auto cleanup_only = connection->cancellation_requested.load(std::memory_order_acquire) ||
                                    connection->canceled || connection->closing ||
                                    connection->terminal_cleanup_complete;
          connection->close_started = true;
          connection->udp_send_changed.notify();
          wake(connection->open_stream_waiters);
          wake(connection->accept_stream_waiters);
          auto primary_error = std::exception_ptr{};
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
                      const auto written =
                          ngtcp2_conn_write_connection_close(connection->conn, &path.path, &packet_info, packet.data(),
                                                             packet.size(), &close_error, timestamp());
                      if (written > 0) {
                         const auto packet_size = static_cast<std::size_t>(written);
                         const auto send_error = co_await connection->send_packet(
                             {.bytes = {packet.begin(), packet.begin() + written}, .route = copy_route(path.path)},
                             true, deadline);
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
                if (error.code() != asio::error::operation_aborted || !connection->native_remote_close_received ||
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
          try {
             co_await connection->wait_background_idle();
          } catch (...) {
             if (!primary_error) {
                primary_error = std::current_exception();
             }
             connection->fail_all();
          }
          // Joining the sender can expose a real fault after a remote-close
          // discard. Never let the earlier cleanup marker hide that fault.
          if (!primary_error && !cleanup_only) {
             try {
                if (connection->udp_failed_generation != 0 && connection->udp_failed_generation <= prefix &&
                    !connection->udp_send_discarded) {
                   co_await connection->wait_udp_send_prefix(prefix, deadline);
                }
                if (connection->cancellation_requested.load(std::memory_order_acquire) || connection->canceled) {
                   throw_engine(engine_error_kind::canceled, "QUIC graceful close canceled before cleanup joined");
                }
             } catch (...) {
                primary_error = std::current_exception();
             }
          }
          connection->complete_close(primary_error);
          if (primary_error) {
             std::rethrow_exception(primary_error);
          }
       },
       asio::use_awaitable);
}

void engine_connection::cancel() {
   if (!impl_) {
      return;
   }
   asio::post(impl_->strand, [impl = impl_] {
      impl->metrics.cancellations.fetch_add(1, std::memory_order_relaxed);
      impl->fail_all();
   });
}

void engine_connection::request_cancel() noexcept {
   if (impl_) {
      impl_->cancellation_requested.store(true, std::memory_order_release);
      impl_->termination_changed.notify();
   }
}

} // namespace forge::net::quic::detail
