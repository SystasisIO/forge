#include "details/quic_engine_support.hxx"
#include "details/engine_stream_impl.hxx"
#include "details/engine_connection_impl.hxx"
#include "details/engine_connector_impl.hxx"
#include "details/engine_listener_impl.hxx"

namespace forge::net::quic::detail {
engine_listener::engine_listener(boost::asio::io_context& context, engine_endpoint bind_endpoint,
                                 engine_server_options options)
    : impl_(std::make_shared<impl>(context, std::move(bind_endpoint), std::move(options))) {
   impl_->self = impl_;
   if (impl_->bind_endpoint.host.empty()) {
      impl_->bind_endpoint.host = "127.0.0.1";
   }
   const auto address = literal_address(impl_->bind_endpoint);
   auto endpoint = udp::endpoint{address, impl_->bind_endpoint.port};
   impl_->server_socket->open_and_bind(endpoint);
   const auto local = impl_->server_socket->local_endpoint();
   impl_->bind_endpoint = from_udp_endpoint(local);
   impl_->start();
}

engine_listener::~engine_listener() {
   stop();
}

engine_endpoint engine_listener::local_endpoint() const {
   return impl_ ? impl_->bind_endpoint : engine_endpoint{};
}

boost::asio::awaitable<std::shared_ptr<engine_connection>> engine_listener::async_accept() {
   if (!impl_) {
      throw_engine(engine_error_kind::connection_closed, "invalid QUIC listener");
   }
   auto state = impl_;
   co_return co_await asio::co_spawn(
       state->strand,
       [state]() -> asio::awaitable<std::shared_ptr<engine_connection>> {
          if (state->shutdown_started) {
             throw_engine(engine_error_kind::connection_closed, "QUIC listener shutdown admission is closed");
          }
          ++state->active_operations;
          const auto finish = [state](engine_listener::impl*) noexcept { state->finish_operation(); };
          auto guard = std::unique_ptr<engine_listener::impl, decltype(finish)>{state.get(), finish};
          while (state->accepted.empty() && !state->stopped &&
                 (!state->pending_accept_error || state->connection_count() != 0)) {
             auto timer = std::make_shared<asio::steady_timer>(state->strand);
             timer->expires_after(std::chrono::minutes{10});
             state->accept_waiters.emplace_back(timer);
             boost::system::error_code ec;
             co_await timer->async_wait(asio::redirect_error(asio::use_awaitable, ec));
          }
          if (state->accepted.empty() && state->pending_accept_error) {
             const auto kind = *state->pending_accept_error;
             auto message = std::move(state->pending_accept_failure_text);
             state->pending_accept_error.reset();
             state->pending_accept_failure_text.clear();
             throw_engine(kind, message.empty() ? "QUIC listener accept failed" : message);
          }
          if (state->accepted.empty()) {
             throw_engine(engine_error_kind::connection_closed, "QUIC listener stopped before accept");
          }
          state->pending_accept_error.reset();
          state->pending_accept_failure_text.clear();
          auto connection = std::move(state->accepted.front());
          state->accepted.pop_front();
          co_return connection;
       },
       asio::use_awaitable);
}

void engine_listener::stop() {
   if (!impl_) {
      return;
   }
   impl_->stop_requested.store(true, std::memory_order_release);
   asio::post(impl_->strand, [impl = impl_] { impl->stop(); });
}

asio::awaitable<std::size_t> engine_listener::async_punch(engine_endpoint local, engine_endpoint remote,
                                                          std::chrono::milliseconds timeout, std::size_t max_packets,
                                                          std::shared_ptr<void> lifetime) {
   auto state = impl_;
   if (timeout.count() <= 0 || timeout > std::chrono::seconds{30} || max_packets == 0 || max_packets > 256) {
      throw_engine(engine_error_kind::invalid_options, "QUIC punch requires 1..256 packets and a 0..30s timeout");
   }
   const auto local_endpoint = udp::endpoint{literal_address(local), local.port};
   const auto remote_endpoint = udp::endpoint{literal_address(remote), remote.port};
   const auto bound = state->server_socket->local_endpoint();
   if (!state->server_socket->owns_local_endpoint(local_endpoint) || remote_endpoint.address().is_unspecified() ||
       remote_endpoint.address().is_multicast() || remote.port == 0 || remote_endpoint.protocol() != bound.protocol()) {
      throw_engine(engine_error_kind::invalid_endpoint, "QUIC punch endpoints do not match listener socket");
   }
   co_return co_await asio::co_spawn(
       state->strand,
       [state, local_endpoint, remote_endpoint, timeout, max_packets,
        lifetime = std::move(lifetime)]() mutable -> asio::awaitable<std::size_t> {
          if (state->shutdown_started || state->stop_requested.load(std::memory_order_acquire)) {
             throw_engine(engine_error_kind::connection_closed, "QUIC punch source listener is closed");
          }
          if (state->active_punches >= state->options.limits.max_connections) {
             throw_engine(engine_error_kind::backpressure_rejected, "QUIC punch operation limit exceeded");
          }
          ++state->active_punches;
          ++state->active_operations;
          const auto release = [state, &lifetime](engine_listener::impl*) noexcept {
             lifetime.reset();
             --state->active_punches;
             state->finish_operation();
          };
          auto operation = std::unique_ptr<engine_listener::impl, decltype(release)>{state.get(), release};
          state->start();
          const auto expires = std::chrono::steady_clock::now() + timeout;
          auto delay = std::make_shared<asio::steady_timer>(state->strand);
          std::erase_if(state->punch_waiters, [](const auto& value) { return value.expired(); });
          state->punch_waiters.push_back(delay);
          auto deadline = asio::steady_timer{state->strand};
          deadline.expires_at(expires);
          for (auto sent = std::size_t{0}; sent < max_packets; ++sent) {
             if (state->stop_requested.load(std::memory_order_acquire)) {
                throw_engine(engine_error_kind::connection_closed, "QUIC punch source listener closed");
             }
             if (std::chrono::steady_clock::now() >= expires) {
                throw_engine(engine_error_kind::connect_timeout, "QUIC punch deadline expired");
             }
             auto packet = server_udp_socket::packet{.bytes = std::vector<std::uint8_t>(64),
                                                     .route = {.remote = remote_endpoint, .local = local_endpoint}};
             if (!fill_random(packet.bytes)) {
                throw_engine(engine_error_kind::internal_error, "QUIC punch randomness unavailable");
             }
             // Non-QUIC random traffic opens the NAT mapping; it never initiates a client handshake.
             packet.bytes.front() &= 0x3f;
             const auto interval = std::chrono::milliseconds{10 + packet.bytes.back() % 191};
             using namespace asio::experimental::awaitable_operators;
             auto result = co_await (state->server_socket->async_send(std::move(packet)) ||
                                     deadline.async_wait(asio::use_awaitable));
             if (result.index() != 0) {
                throw_engine(engine_error_kind::connect_timeout, "QUIC punch deadline expired during send");
             }
             if (const auto error = std::get<0>(result); error) {
                throw_engine(error == asio::error::operation_aborted ? engine_error_kind::canceled
                                                                     : engine_error_kind::internal_error,
                             "QUIC punch send failed: " + error.message());
             }
             if (sent + 1 == max_packets) {
                if (state->stop_requested.load(std::memory_order_acquire)) {
                   throw_engine(engine_error_kind::connection_closed, "QUIC punch source listener closed");
                }
                co_return max_packets;
             }
             delay->expires_at(std::min(expires, std::chrono::steady_clock::now() + interval));
             auto error = boost::system::error_code{};
             co_await delay->async_wait(asio::redirect_error(asio::use_awaitable, error));
             if (error && !state->stop_requested.load(std::memory_order_acquire)) {
                throw_engine(engine_error_kind::canceled, "QUIC punch canceled");
             }
          }
          co_return max_packets;
       },
       asio::use_awaitable);
}

boost::asio::awaitable<void> engine_listener::async_stop() {
   if (!impl_) {
      co_return;
   }
   auto state = impl_;
   co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
   co_await asio::co_spawn(state->strand, state->async_shutdown(), asio::use_awaitable);
}

} // namespace forge::net::quic::detail
