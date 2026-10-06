module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/associated_executor.hpp>
#include <boost/asio/associated_cancellation_slot.hpp>
#include <boost/asio/async_result.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <boost/compat/move_only_function.hpp>
#include <boost/system/error_code.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/ssl.h>
#include "details/handshake_deadline.hxx"
#include "details/connection_test_hooks.hxx"

module forge.net.stcp.connection;

import forge.asio.gate;
import forge.asio.notification;
import forge.net.tls.context;
import forge.net.tls.exceptions;
import forge.net.transport.stream;

#include "details/connection_impl.hxx"

namespace forge::net::stcp {
namespace asio = boost::asio;
using detail::io_gates;
using detail::io_stop_reason;
using detail::stream_model;
using detail::terminalize_io_error;
using detail::terminalize_closed;

namespace {

[[nodiscard]] std::int64_t next_stream_id() noexcept {
   static auto next = std::atomic<std::int64_t>{1};
   return next.fetch_add(1, std::memory_order_relaxed);
}

} // namespace

connection::impl::impl(std::shared_ptr<detail::stream_backend> stream_value, tls::context_snapshot_ptr context_value,
                       std::size_t read_chunk_size_value, transport::endpoint local, transport::endpoint remote,
                       std::shared_ptr<void> lifetime_value)
    : stream(std::move(stream_value)), context(std::move(context_value)),
      strand(asio::make_strand(stream->get_executor())), gates(std::make_shared<io_gates>()),
      terminal_completed(std::make_shared<forge::asio::notification>()),
      terminal_failure(std::make_shared<std::exception_ptr>()), read_chunk_size(read_chunk_size_value),
      id(next_stream_id()), local_value(std::move(local)), remote_value(std::move(remote)),
      lifetime(std::move(lifetime_value)) {
   chain_value = tls::extract_peer_certificate_chain(stream->native_handle());
   if (!chain_value.certificates.empty()) {
      certificate_value = chain_value.certificates.front();
   }
   alpn_value = tls::selected_alpn(stream->native_handle());
}

void connection::impl::start_terminal_worker() {
   // This operation owns the backend across a later transport handoff and
   // serializes terminal lower-stream cleanup on its owning executor.
   auto current = stream;
   auto current_gates = gates;
   auto completed = terminal_completed;
   auto failure = terminal_failure;
   auto lifetime_value = lifetime;
   asio::co_spawn(
       strand,
       [current = std::move(current), current_gates = std::move(current_gates),
        lifetime_value = std::move(lifetime_value)]() -> asio::awaitable<void> {
          static_cast<void>(lifetime_value);
          try {
             static_cast<void>(co_await current_gates->terminal_requested.async_wait(0));
          } catch (...) {
             current_gates->stop(io_stop_reason::canceled);
          }
          co_await current->async_terminal_close();
       },
       [current = stream, completed = std::move(completed), failure = std::move(failure),
        lifetime_value = lifetime](std::exception_ptr error) noexcept {
          static_cast<void>(lifetime_value);
          if (error) {
             // Native backend cancellation closes its actual lower socket;
             // a transport backend retains its independently owned lower.
             current->request_cancel();
             *failure = std::move(error);
          }
          completed->notify();
       });
}

[[nodiscard]] bool connection::impl::valid() const noexcept {
   const auto lock = std::scoped_lock{state_mutex};
   return state == connection_state::active;
}

[[nodiscard]] transport::endpoint connection::impl::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return local_value;
}

[[nodiscard]] transport::endpoint connection::impl::remote_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return remote_value;
}

boost::asio::awaitable<void> connection::impl::async_write(std::span<const std::uint8_t> bytes) {
   claim_operation();
   try {
      auto write_ticket = co_await gates->acquire(gates->write);
      auto self = shared_from_this();
      co_await asio::co_spawn(
          strand,
          [self = std::move(self), bytes]() -> asio::awaitable<void> {
             if (self->gates->stopped()) {
                self->gates->throw_stopped();
             }
             if (!self->stream || !self->stream->is_open()) {
                self->mark_closed_from_io();
                terminalize_closed(self->stream.get(), *self->gates, "invalid stcp connection");
             }
             const auto error = co_await self->stream->async_write(bytes);
             if (error) {
                self->mark_closed_from_io();
                terminalize_io_error(*self->stream, *self->gates, error);
             }
          },
          asio::use_awaitable);
   } catch (...) {
      release_operation();
      throw;
   }
   release_operation();
}

boost::asio::awaitable<std::size_t> connection::impl::async_read_some(std::span<std::uint8_t> bytes) {
   claim_operation();
   try {
      auto read_ticket = co_await gates->acquire(gates->read);
      auto self = shared_from_this();
      const auto size = co_await asio::co_spawn(
          strand,
          [self = std::move(self), bytes]() -> asio::awaitable<std::size_t> {
             if (self->gates->stopped()) {
                self->gates->throw_stopped();
             }
             if (!self->stream || !self->stream->is_open()) {
                self->mark_closed_from_io();
                terminalize_closed(self->stream.get(), *self->gates, "invalid stcp connection");
             }
             const auto [error, size] = co_await self->stream->async_read_some(bytes);
             if (error) {
                self->mark_closed_from_io();
                terminalize_io_error(*self->stream, *self->gates, error);
             }
             co_return size;
          },
          asio::use_awaitable);
      release_operation();
      co_return size;
   } catch (...) {
      release_operation();
      throw;
   }
}

boost::asio::awaitable<std::vector<std::uint8_t>> connection::impl::async_read() {
   claim_operation();
   try {
      auto read_ticket = co_await gates->acquire(gates->read);
      auto out = std::vector<std::uint8_t>(read_chunk_size);
      auto self = shared_from_this();
      const auto size = co_await asio::co_spawn(
          strand,
          [self = std::move(self), writable = std::span<std::uint8_t>{out}]() -> asio::awaitable<std::size_t> {
             if (self->gates->stopped()) {
                self->gates->throw_stopped();
             }
             if (!self->stream || !self->stream->is_open()) {
                self->mark_closed_from_io();
                terminalize_closed(self->stream.get(), *self->gates, "invalid stcp connection");
             }
             const auto [error, size] = co_await self->stream->async_read_some(writable);
             if (error) {
                self->mark_closed_from_io();
                terminalize_io_error(*self->stream, *self->gates, error);
             }
             co_return size;
          },
          asio::use_awaitable);
      out.resize(size);
      release_operation();
      co_return out;
   } catch (...) {
      release_operation();
      throw;
   }
}

boost::asio::awaitable<void> connection::impl::async_close() {
   if (request_terminal(connection_state::close_requested)) {
      gates->stop(io_stop_reason::closed);
   }
   static_cast<void>(co_await terminal_completed->async_wait(0));
   if (*terminal_failure) {
      std::rethrow_exception(*terminal_failure);
   }
}

void connection::impl::cancel() noexcept {
   if (request_terminal(connection_state::cancel_requested)) {
      gates->stop(io_stop_reason::canceled);
   }
}

[[nodiscard]] transport::stream_connection connection::impl::into_transport_stream() {
   static_assert(std::is_nothrow_move_constructible_v<transport::stream_connection>);
   auto model = std::make_shared<stream_model>(strand, gates, terminal_completed, terminal_failure,
                                              read_chunk_size, id, lifetime);
   auto weak = std::weak_ptr<stream_model>{model};
   auto result = transport::stream_connection{
       .local_endpoint = local_value,
       .remote_endpoint = remote_value,
       .stream = transport::detail::stream_access::make_cancelable(model,
                                                                   [weak = std::move(weak)]() noexcept {
                                                                      if (auto value = weak.lock()) {
                                                                         value->request_cancel();
                                                                      }
                                                                   }),
   };
   commit_handoff(model);
   return result;
}

[[nodiscard]] bool connection::impl::request_terminal(connection_state requested) noexcept {
   const auto lock = std::scoped_lock{state_mutex};
   if (state != connection_state::active) {
      return false;
   }
   state = requested;
   return true;
}

void connection::impl::mark_closed_from_io() noexcept {
   const auto lock = std::scoped_lock{state_mutex};
   if (state == connection_state::active || state == connection_state::cancel_requested ||
       state == connection_state::close_requested) {
      state = connection_state::closed;
   }
}

void connection::impl::commit_handoff(const std::shared_ptr<stream_model>& model) {
   const auto lock = std::scoped_lock{state_mutex};
   if (state != connection_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "stcp connection cannot hand off a terminal stream");
   }
   if (active_operations != 0) {
      FORGE_THROW_EXCEPTION(exceptions::io_error, "stcp connection cannot hand off while I/O is active");
   }
   // Model, cancel callback, and result endpoints are fully allocated. The
   // remaining shared_ptr move and state commit are non-throwing.
   model->attach(std::move(stream));
   state = connection_state::handed_off;
}

void connection::impl::claim_operation() {
   const auto lock = std::scoped_lock{state_mutex};
   if (state == connection_state::cancel_requested) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "stcp connection canceled");
   }
   if (state != connection_state::active) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   ++active_operations;
}

void connection::impl::release_operation() noexcept {
   const auto lock = std::scoped_lock{state_mutex};
   --active_operations;
}

} // namespace forge::net::stcp
