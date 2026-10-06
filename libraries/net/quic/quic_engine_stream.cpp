#include "details/quic_engine_support.hxx"
#include "details/engine_stream_impl.hxx"
#include "details/engine_connection_impl.hxx"

namespace forge::net::quic::detail {
engine_stream::engine_stream(std::shared_ptr<impl> impl_value) : impl_(std::move(impl_value)) {}

std::int64_t engine_stream::id() const noexcept {
   return impl_ ? impl_->id : -1;
}

boost::asio::awaitable<void> engine_stream::async_write(std::span<const std::uint8_t> bytes) {
   co_await async_write(bytes, {});
}

boost::asio::awaitable<void> engine_stream::async_write(std::span<const std::uint8_t> bytes,
                                                        std::shared_ptr<void> lifetime) {
   co_await async_write(std::vector<std::uint8_t>{bytes.begin(), bytes.end()}, std::move(lifetime));
}

boost::asio::awaitable<void> engine_stream::async_write(std::vector<std::uint8_t> bytes,
                                                        std::shared_ptr<void> lifetime) {
   if (!impl_) {
      throw_engine(engine_error_kind::stream_closed, "invalid QUIC stream");
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      throw_engine(engine_error_kind::connection_closed, "QUIC connection is closed");
   }
   co_await asio::co_spawn(
       connection->strand,
       [connection, stream = impl_, owned = std::move(bytes),
        lifetime = std::move(lifetime)]() mutable -> asio::awaitable<void> {
          if (connection->close_started || connection->closing || connection->canceled) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection write admission is closed");
          }
          if (stream->reset) {
             throw_engine(engine_error_kind::stream_reset, "QUIC stream is reset");
          }
          if (stream->fin_queued || stream->local_write_closed || stream->closed) {
             throw_engine(engine_error_kind::stream_closed, "QUIC stream write side is closed");
          }
          if (owned.empty()) {
             co_return;
          }
          const auto queued = connection->metrics.queued_bytes.load(std::memory_order_relaxed);
          if (owned.size() > connection->limits.max_queued_bytes ||
              queued > connection->limits.max_queued_bytes - owned.size()) {
             connection->metrics.backpressure_rejections.fetch_add(1, std::memory_order_relaxed);
             throw_engine(engine_error_kind::backpressure_rejected, "QUIC stream write queue exceeds max_queued_bytes");
          }
          const auto size = owned.size();
          stream->outbound.push_back(engine_stream::impl::pending_write{
              .data = std::move(owned),
              .lifetime = std::move(lifetime),
          });
          connection->metrics.queued_bytes.fetch_add(size, std::memory_order_relaxed);
          connection->metrics.frames_sent.fetch_add(1, std::memory_order_relaxed);
          connection->spawn_background(
              [](const std::shared_ptr<engine_connection::impl>& value) -> asio::awaitable<void> {
                 if (value->test_failpoint && value->test_failpoint("background_stream_drain_failure")) {
                    throw std::runtime_error{"QUIC test failpoint: background stream drain failure"};
                 }
                 try {
                    co_await value->drain_send();
                 } catch (const engine_failure&) {
                    value->fail_all();
                 }
              });
          co_await asio::post(connection->strand, asio::use_awaitable);
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::vector<std::uint8_t>> engine_stream::async_read() {
   if (!impl_) {
      throw_engine(engine_error_kind::stream_closed, "invalid QUIC stream");
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      throw_engine(engine_error_kind::connection_closed, "QUIC connection is closed");
   }
   co_return co_await asio::co_spawn(
       connection->strand,
       [connection, stream = impl_]() -> asio::awaitable<std::vector<std::uint8_t>> {
          while (stream->inbound_ready.empty() && !stream->remote_read_closed && !stream->remote_read_reset &&
                 !stream->reset && !stream->closed && !connection->closing && !connection->canceled) {
             auto timer = std::make_shared<asio::steady_timer>(connection->strand);
             timer->expires_after(std::chrono::minutes{10});
             stream->read_waiters.emplace_back(timer);
             boost::system::error_code ec;
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
          }
          if (!stream->inbound_ready.empty()) {
             auto out = std::move(stream->inbound_ready.front());
             stream->inbound_ready.pop_front();
             co_return out;
          }
          if (stream->reset || stream->remote_read_reset) {
             throw_engine(engine_error_kind::stream_reset, "QUIC stream was reset while reading");
          }
          if (connection->canceled) {
             throw_engine(engine_error_kind::canceled, "QUIC connection was canceled while reading");
          }
          if (connection->closing) {
             throw_engine(engine_error_kind::connection_closed, "QUIC connection closed while reading");
          }
          throw_engine(engine_error_kind::stream_closed, "QUIC stream read side is closed");
       },
       asio::use_awaitable);
}

boost::asio::awaitable<void> engine_stream::async_close() {
   if (!impl_) {
      co_return;
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      co_return;
   }
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   const auto deadline = std::chrono::steady_clock::now() + detached_write_drain_timeout;
   auto primary_error = std::exception_ptr{};
   try {
      if (connection->test_failpoint && connection->test_failpoint("stream_close_before_owner_spawn")) {
         throw std::bad_alloc{};
      }
      co_await asio::co_spawn(
          connection->strand,
          [connection, stream = impl_, deadline]() -> asio::awaitable<void> {
             auto error = std::exception_ptr{};
             try {
                if (connection->test_failpoint && connection->test_failpoint("stream_close_before_state_check")) {
                   throw std::bad_alloc{};
                }
                if (connection->close_started) {
                   co_await connection->wait_close_cleanup();
                   co_return;
                }
                if (connection->closing || connection->canceled) {
                   co_await connection->wait_udp_send_idle();
                   co_return;
                }
                if (!stream->local_write_closed && !stream->reset && !stream->closed) {
                   if (!stream->fin_queued) {
                      if (connection->test_failpoint && connection->test_failpoint("stream_close_before_fin")) {
                         throw std::bad_alloc{};
                      }
                      stream->outbound.push_back(engine_stream::impl::pending_write{.fin = true});
                      stream->fin_queued = true;
                   }
                   co_await connection->drain_send(deadline);
                   while (!stream->local_write_closed && !stream->reset && !stream->closed && !connection->closing &&
                          !connection->canceled) {
                      auto timer = std::make_shared<asio::steady_timer>(connection->strand);
                      if (std::chrono::steady_clock::now() >= deadline) {
                         throw boost::system::system_error{asio::error::timed_out, "QUIC FIN submission timed out"};
                      }
                      timer->expires_at(deadline);
                      stream->write_waiters.emplace_back(timer);
                      auto ec = boost::system::error_code{};
                      co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
                      remove_waiter(stream->write_waiters, timer);
                   }
                   connection->update_active_stream_metrics();
                }
                if (stream->fin_queued) {
                   if (stream->reset) {
                      throw_engine(engine_error_kind::stream_reset, "QUIC stream reset before FIN send completed");
                   }
                   if (connection->canceled) {
                      throw_engine(engine_error_kind::canceled, "QUIC FIN send canceled");
                   }
                   if (stream->fin_send_generation == 0) {
                      throw_engine(engine_error_kind::connection_closed,
                                   "QUIC connection closed before FIN was queued");
                   }
                   co_await connection->wait_udp_send_prefix(stream->fin_send_generation, deadline, stream);
                }
             } catch (...) {
                error = std::current_exception();
             }
             if (error) {
                co_await connection->async_reset_stream_after_close_failure(stream);
                if (connection->closing || connection->canceled) {
                   co_await connection->wait_udp_send_idle();
                }
                std::rethrow_exception(error);
             }
          },
          asio::use_awaitable);
   } catch (...) {
      primary_error = std::current_exception();
   }
   if (primary_error) {
      // The cancel worker is started before an engine stream is published.
      // This non-throwing handoff also covers a failure starting co_spawn.
      impl_->cancel_requested.notify();
      co_await wait_for_stream_terminal_cleanup(impl_);
      std::rethrow_exception(primary_error);
   }
   // A concurrent cancel worker may already own the native stream reset.
   // Success is observable only after that owner publishes its terminal drain.
   if (connection->test_failpoint) {
      static_cast<void>(connection->test_failpoint("stream_close_before_terminal_join"));
   }
   co_await wait_for_stream_terminal_cleanup(impl_);
   co_await asio::co_spawn(
       connection->strand,
       [stream = impl_]() -> asio::awaitable<void> {
          if (stream->terminal_cleanup_error) {
             std::rethrow_exception(stream->terminal_cleanup_error);
          }
          co_return;
       },
       asio::use_awaitable);
}

void engine_stream::cancel_write() {
   if (!impl_) {
      return;
   }
   auto connection = impl_->connection.lock();
   if (!connection) {
      return;
   }
   auto stream = impl_;
   asio::dispatch(connection->strand, [connection, stream] {
      if (stream->local_write_closed || stream->reset || stream->closed) {
         return;
      }
      auto cleanup = claim_stream_terminal_owner(stream);
      auto shutdown_result = 0;
      auto should_drain = false;
      if (connection->conn != nullptr && !connection->closing && !connection->canceled) {
         shutdown_result = ngtcp2_conn_shutdown_stream_write(connection->conn, 0, stream->id, 0);
         should_drain = shutdown_result == 0;
      }
      connection->release_queued_stream_writes(stream);
      stream->local_write_closed = true;
      publish_stream_terminal(stream);
      wake(stream->write_waiters);
      if (!std::exchange(stream->reset_counted, true)) {
         connection->metrics.streams_reset.fetch_add(1, std::memory_order_relaxed);
      }
      connection->update_active_stream_metrics();
      if (shutdown_result != 0) {
         connection->fail_all();
         return;
      }
      if (!should_drain) {
         return;
      }
      const auto deadline = std::chrono::steady_clock::now() + detached_write_drain_timeout;
      // The pair destroys the claim before its strong stream owner, including
      // when spawn_background drops the work before invoking the coroutine.
      connection->spawn_background(
          [owned = std::pair{stream, std::move(cleanup)},
           deadline](const std::shared_ptr<engine_connection::impl>& value) mutable -> asio::awaitable<void> {
             co_await value->drain_stream_reset(owned.first, deadline);
             owned.second.reset();
          });
   });
}

void engine_stream::cancel() {
   request_cancel();
}

void engine_stream::request_cancel() noexcept {
   if (!impl_) {
      return;
   }
   impl_->cancel_requested.notify();
}

} // namespace forge::net::quic::detail
