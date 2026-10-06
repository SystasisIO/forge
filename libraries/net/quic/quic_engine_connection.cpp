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
          if (!connection->close_cleanup_complete) {
             auto error = boost::system::error_code{};
             co_await connection->async_wait_close(
                 [deadline](const std::shared_ptr<impl>& owner) {
                    if (owner->close_started) {
                       return;
                    }
                    // Admission follows native wait registration, before any
                    // fallible close-worker launch or allocating background join.
                    const auto cleanup_only = owner->cancellation_requested.load(std::memory_order_acquire) ||
                                              owner->canceled || owner->closing || owner->terminal_cleanup_complete;
                    owner->close_started = true;
                    owner->udp_send_changed.notify();
                    wake(owner->open_stream_waiters);
                    wake(owner->accept_stream_waiters);
                    owner->close_work_active = true;
                    try {
                       asio::co_spawn(owner->strand, owner->async_close_on_owner(deadline, cleanup_only),
                                      [owner](std::exception_ptr failure) noexcept {
                                         owner->close_work_active = false;
                                         if (failure) {
                                            owner->fail_all();
                                         }
                                         owner->complete_close(std::move(failure));
                                      });
                    } catch (...) {
                       owner->close_work_active = false;
                       throw;
                    }
                 },
                 asio::redirect_error(asio::use_awaitable, error));
             if (error != asio::error::operation_aborted) {
                throw boost::system::system_error{error ? error : asio::error::fault, "QUIC terminal wait failed"};
             }
          }
          if (connection->close_error) {
             std::rethrow_exception(connection->close_error);
          }
       },
       asio::use_awaitable);
}

void engine_connection::cancel() {
   if (!impl_) {
      return;
   }
   request_cancel();
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
