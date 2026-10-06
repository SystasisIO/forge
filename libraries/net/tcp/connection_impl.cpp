module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/system/error_code.hpp>
#include "details/connection_test_hooks.hxx"

module forge.net.tcp.connection;

import forge.asio.notification;
import forge.net.transport.stream;

#include "details/connection_impl.hxx"

namespace forge::net::tcp {
namespace asio = boost::asio;
using asio_tcp = boost::asio::ip::tcp;
using detail::socket_state;
using detail::cancel_socket;
using detail::throw_io_error;
using detail::throw_read_write_error;
using detail::prepare_stream;

namespace {

[[nodiscard]] std::int64_t next_stream_id() noexcept {
   static auto next = std::atomic<std::int64_t>{1};
   return next.fetch_add(1, std::memory_order_relaxed);
}

[[noreturn]] void throw_invalid_options(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

void validate_options(const options& value) {
   if (value.read_chunk_size == 0) {
      throw_invalid_options("tcp read_chunk_size must be greater than zero");
   }
}

[[nodiscard]] transport::endpoint from_asio_endpoint(const asio_tcp::endpoint& endpoint) {
   return transport::endpoint::from_address(endpoint.address(), endpoint.port(),
                                            transport::endpoint::protocol_kind::tcp);
}

} // namespace

connection::impl::impl(std::shared_ptr<asio_tcp::socket> socket_value, options tcp_options_value,
                       std::shared_ptr<void> lifetime_value, std::shared_ptr<detail::connection_test_hooks> hooks)
    : socket(std::move(socket_value)), tcp_options(tcp_options_value),
      strand(asio::make_strand(socket->get_executor())), id(next_stream_id()),
      terminal_state(std::make_shared<std::atomic<socket_state>>(socket_state::active)),
      terminal_requested(std::make_shared<forge::asio::notification>()),
      terminal_completed(std::make_shared<forge::asio::notification>()), lifetime(std::move(lifetime_value)),
      test_hooks(std::move(hooks)) {
   validate_options(tcp_options);
   auto error = boost::system::error_code{};
   local = from_asio_endpoint(socket->local_endpoint(error));
   if (error) {
      throw_io_error("failed to read tcp local endpoint", error);
   }
   remote = from_asio_endpoint(socket->remote_endpoint(error));
   if (error) {
      throw_io_error("failed to read tcp remote endpoint", error);
   }
}

connection::impl::~impl() {
   request_cancel();
}

void connection::impl::start_terminal_worker() {
   // The composed operation owns the socket and terminal state independently
   // of connection::impl. handed_off is terminal for this owner but never closes
   // the socket transferred to the stream or native caller.
   auto current = socket;
   auto state = terminal_state;
   auto requested = terminal_requested;
   auto completed = terminal_completed;
   auto lifetime_value = lifetime;
   auto hooks = test_hooks;
   if (hooks && hooks->startup) {
      hooks->startup(hooks->state.get(), detail::startup_stage::terminal_launch);
   }
   asio::co_spawn(
       strand,
       [current = std::move(current), state = std::move(state), requested = std::move(requested), completed,
        lifetime_value, hooks]() mutable -> asio::awaitable<void> {
          static_cast<void>(lifetime_value);
          try {
             static_cast<void>(co_await requested->async_wait(0));
          } catch (...) {
             auto expected = socket_state::active;
             state->compare_exchange_strong(expected, socket_state::cancel_requested, std::memory_order_acq_rel,
                                            std::memory_order_acquire);
          }
          close_on_owner(*current, *state, hooks);
          completed->notify();
       },
       [current = socket, state = terminal_state, completed = terminal_completed,
        lifetime_value, hooks](std::exception_ptr error) noexcept {
          static_cast<void>(lifetime_value);
          if (error) {
             auto expected = socket_state::active;
             state->compare_exchange_strong(expected, socket_state::cancel_requested, std::memory_order_acq_rel);
             close_on_owner(*current, *state, hooks);
          }
          completed->notify();
       });
}

[[nodiscard]] bool connection::impl::valid() const noexcept {
   const auto lock = std::scoped_lock{state_mutex};
   return !stream_handed_off && terminal_state->load(std::memory_order_acquire) == socket_state::active;
}

[[nodiscard]] transport::endpoint connection::impl::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return local;
}

[[nodiscard]] transport::endpoint connection::impl::remote_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return remote;
}

boost::asio::awaitable<void> connection::impl::async_write(std::span<const std::uint8_t> bytes) {
   auto self = shared_from_this();
   co_await asio::co_spawn(
       strand,
       [self = std::move(self), bytes]() -> asio::awaitable<void> {
          self->claim_operation();
          auto error = boost::system::error_code{};
          try {
             co_await asio::async_write(*self->socket, asio::buffer(bytes),
                                        asio::redirect_error(asio::use_awaitable, error));
          } catch (...) {
             self->release_operation();
             throw;
          }
          self->release_operation();
          if (error) {
             throw_read_write_error(error);
          }
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::size_t> connection::impl::async_read_some(std::span<std::uint8_t> bytes) {
   auto self = shared_from_this();
   co_return co_await asio::co_spawn(
       strand,
       [self = std::move(self), bytes]() -> asio::awaitable<std::size_t> {
          self->claim_operation();
          auto error = boost::system::error_code{};
          auto size = std::size_t{};
          try {
             size = co_await self->socket->async_read_some(asio::buffer(bytes),
                                                           asio::redirect_error(asio::use_awaitable, error));
          } catch (...) {
             self->release_operation();
             throw;
          }
          self->release_operation();
          if (error) {
             throw_read_write_error(error);
          }
          co_return size;
       },
       asio::use_awaitable);
}

boost::asio::awaitable<std::vector<std::uint8_t>> connection::impl::async_read() {
   auto out = std::vector<std::uint8_t>(tcp_options.read_chunk_size);
   auto self = shared_from_this();
   const auto size = co_await asio::co_spawn(
       strand,
       [self = std::move(self), writable = std::span<std::uint8_t>{out}]() -> asio::awaitable<std::size_t> {
          self->claim_operation();
          auto error = boost::system::error_code{};
          auto size = std::size_t{};
          try {
             size = co_await self->socket->async_read_some(asio::buffer(writable),
                                                           asio::redirect_error(asio::use_awaitable, error));
          } catch (...) {
             self->release_operation();
             throw;
          }
          self->release_operation();
          if (error) {
             throw_read_write_error(error);
          }
          co_return size;
       },
       asio::use_awaitable);
   out.resize(size);
   co_return out;
}

boost::asio::awaitable<void> connection::impl::async_close() {
   if (request_terminal(socket_state::close_requested) == terminal_request_result::stream_handed_off) {
      co_return;
   }
   terminal_requested->notify();
   static_cast<void>(co_await terminal_completed->async_wait(0));
}

void connection::impl::cancel() {
   request_cancel();
}

void connection::impl::request_cancel() noexcept {
   if (request_terminal(socket_state::cancel_requested) == terminal_request_result::stream_handed_off) {
      return;
   }
   terminal_requested->notify();
}

[[nodiscard]] transport::stream_connection connection::impl::into_transport_stream() {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   auto [model, stream] = prepare_stream(socket, strand, tcp_options, id, terminal_state, terminal_requested,
                                         terminal_completed, std::move(lifetime));
   auto out =
       transport::stream_connection{.local_endpoint = local, .remote_endpoint = remote, .stream = std::move(stream)};
   commit_stream_handoff();
   model->activate();
   return out;
}

[[nodiscard]] asio_tcp::socket connection::impl::release_socket(std::shared_ptr<void>* lifetime_out) {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   if (!lifetime_out && lifetime) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "tcp socket handoff with an owner lifetime requires a lifetime destination");
   }
   auto current = detach_socket();
   if (lifetime_out) {
      *lifetime_out = std::move(lifetime);
   }
   // A handoff is terminal for this owner. Wake the worker so it can drop
   // its moved-from socket without retaining a descriptor reservation.
   terminal_requested->notify();
   auto out = std::move(*current);
   return out;
}

[[nodiscard]] connection::impl::terminal_request_result connection::impl::request_terminal(socket_state requested) noexcept {
   const auto lock = std::scoped_lock{state_mutex};
   if (stream_handed_off) {
      return terminal_request_result::stream_handed_off;
   }
   auto expected = socket_state::active;
   if (terminal_state->compare_exchange_strong(expected, requested, std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
      return terminal_request_result::requested;
   }
   return terminal_request_result::already_terminal;
}

void connection::impl::close_on_owner(asio_tcp::socket& current, std::atomic<socket_state>& state,
                              const std::shared_ptr<detail::connection_test_hooks>& hooks) noexcept {
   auto observed = state.load(std::memory_order_acquire);
   while (observed == socket_state::cancel_requested || observed == socket_state::close_requested) {
      if (state.compare_exchange_weak(observed, socket_state::closed, std::memory_order_acq_rel,
                                      std::memory_order_acquire)) {
         cancel_socket(current, hooks);
         return;
      }
   }
}

[[nodiscard]] std::shared_ptr<asio_tcp::socket> connection::impl::detach_socket() {
   const auto lock = std::scoped_lock{state_mutex};
   if (terminal_state->load(std::memory_order_acquire) != socket_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "tcp connection cannot hand off a terminal socket");
   }
   if (active_operations != 0) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "tcp connection cannot hand off while I/O is active");
   }
   auto expected = socket_state::active;
   if (!terminal_state->compare_exchange_strong(expected, socket_state::handed_off, std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "tcp connection cannot hand off a terminal socket");
   }
   return std::move(socket);
}

void connection::impl::commit_stream_handoff() {
   const auto lock = std::scoped_lock{state_mutex};
   if (terminal_state->load(std::memory_order_acquire) != socket_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "tcp connection cannot hand off a terminal socket");
   }
   if (active_operations != 0) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "tcp connection cannot hand off while I/O is active");
   }
   stream_handed_off = true;
}

void connection::impl::claim_operation() {
   const auto lock = std::scoped_lock{state_mutex};
   if (stream_handed_off || terminal_state->load(std::memory_order_acquire) != socket_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   ++active_operations;
}

void connection::impl::release_operation() noexcept {
   const auto lock = std::scoped_lock{state_mutex};
   --active_operations;
}

} // namespace forge::net::tcp
