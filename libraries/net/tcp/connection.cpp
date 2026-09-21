module;

#include <forge/exceptions/macros.hpp>

#include <atomic>
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

module forge.net.tcp.connection;

import forge.asio.notification;
import forge.net.transport.stream;

namespace forge::net::tcp {
namespace {

namespace asio = boost::asio;
using asio_tcp = boost::asio::ip::tcp;

[[nodiscard]] std::int64_t next_stream_id() noexcept {
   static auto next = std::atomic<std::int64_t>{1};
   return next.fetch_add(1, std::memory_order_relaxed);
}

[[noreturn]] void throw_invalid_options(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

[[noreturn]] void throw_io_error(std::string message, const boost::system::error_code& error) {
   FORGE_THROW_EXCEPTION(exceptions::io_error, std::move(message), forge::exceptions::ctx("reason", error.message()));
}

[[noreturn]] void throw_read_write_error(const boost::system::error_code& error) {
   if (error == boost::asio::error::operation_aborted) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "tcp connection operation canceled",
                            forge::exceptions::ctx("reason", error.message()));
   }
   if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset ||
       error == boost::asio::error::broken_pipe) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "tcp connection closed",
                            forge::exceptions::ctx("reason", error.message()));
   }
   throw_io_error("tcp connection I/O failed", error);
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

void cancel_socket(asio_tcp::socket& socket) noexcept {
   auto ignored = boost::system::error_code{};
   socket.cancel(ignored);
   socket.shutdown(asio_tcp::socket::shutdown_both, ignored);
   socket.close(ignored);
}

enum class socket_state : std::uint8_t {
   active,
   cancel_requested,
   close_requested,
   handed_off,
   closed,
};

class socket_stream final : public transport::detail::stream_concept,
                            public std::enable_shared_from_this<socket_stream> {
 public:
   socket_stream(std::shared_ptr<asio_tcp::socket> socket, asio::strand<asio::any_io_executor> strand,
                 options tcp_options, std::int64_t id, std::shared_ptr<std::atomic<socket_state>> state,
                 std::shared_ptr<forge::asio::notification> terminal_requested,
                 std::shared_ptr<forge::asio::notification> terminal_completed, std::shared_ptr<void> lifetime)
       : socket_(std::move(socket)), strand_(std::move(strand)), options_(tcp_options), id_(id),
         state_(std::move(state)), terminal_requested_(std::move(terminal_requested)),
         terminal_completed_(std::move(terminal_completed)), lifetime_(std::move(lifetime)) {}

   ~socket_stream() override {
      request_cancel();
   }

   void activate() noexcept {
      ownership_active_.store(true, std::memory_order_release);
   }

   [[nodiscard]] bool valid() const noexcept override {
      return ownership_active_.load(std::memory_order_acquire) &&
             state_->load(std::memory_order_acquire) == socket_state::active;
   }

   [[nodiscard]] std::int64_t id() const noexcept override {
      return id_;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) override {
      auto self = shared_from_this();
      co_await asio::co_spawn(
          strand_,
          [self = std::move(self), bytes]() -> asio::awaitable<void> {
             if (!self->valid()) {
                FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp stream");
             }
             auto error = boost::system::error_code{};
             co_await asio::async_write(*self->socket_, asio::buffer(bytes),
                                        asio::redirect_error(asio::use_awaitable, error));
             if (error) {
                throw_read_write_error(error);
             }
          },
          asio::use_awaitable);
   }

   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() override {
      auto self = shared_from_this();
      auto out = std::vector<std::uint8_t>(options_.read_chunk_size);
      const auto size = co_await asio::co_spawn(
          strand_,
          [self = std::move(self), writable = std::span<std::uint8_t>{out}]() -> asio::awaitable<std::size_t> {
             if (!self->valid()) {
                FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp stream");
             }
             auto error = boost::system::error_code{};
             const auto size = co_await self->socket_->async_read_some(
                 asio::buffer(writable), asio::redirect_error(asio::use_awaitable, error));
             if (error) {
                throw_read_write_error(error);
             }
             co_return size;
          },
          asio::use_awaitable);
      out.resize(size);
      co_return out;
   }

   boost::asio::awaitable<transport::chunk> async_read_chunk() override {
      auto builder = pool_.acquire(options_.read_chunk_size);
      auto writable = builder.writable();
      auto self = shared_from_this();
      const auto size = co_await asio::co_spawn(
          strand_,
          [self = std::move(self), writable]() -> asio::awaitable<std::size_t> {
             if (!self->valid()) {
                FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp stream");
             }
             auto error = boost::system::error_code{};
             const auto size = co_await self->socket_->async_read_some(
                 asio::buffer(writable), asio::redirect_error(asio::use_awaitable, error));
             if (error) {
                throw_read_write_error(error);
             }
             co_return size;
          },
          asio::use_awaitable);
      co_return builder.commit(size);
   }

   boost::asio::awaitable<void> async_close() override {
      if (request_terminal(socket_state::close_requested)) {
         terminal_requested_->notify();
      }
      static_cast<void>(co_await terminal_completed_->async_wait(0));
   }

   void cancel() override {
      request_cancel();
   }

   void request_cancel() noexcept {
      if (request_terminal(socket_state::cancel_requested)) {
         terminal_requested_->notify();
      }
   }

 private:
   [[nodiscard]] bool request_terminal(socket_state requested) noexcept {
      if (!ownership_active_.load(std::memory_order_acquire)) {
         return false;
      }
      auto expected = socket_state::active;
      return state_->compare_exchange_strong(expected, requested, std::memory_order_acq_rel, std::memory_order_acquire);
   }

   std::shared_ptr<asio_tcp::socket> socket_;
   asio::strand<asio::any_io_executor> strand_;
   options options_;
   transport::buffer_pool pool_;
   std::int64_t id_ = -1;
   std::shared_ptr<std::atomic<socket_state>> state_;
   std::shared_ptr<forge::asio::notification> terminal_requested_;
   std::shared_ptr<forge::asio::notification> terminal_completed_;
   std::shared_ptr<void> lifetime_;
   std::atomic_bool ownership_active_ = false;
};

[[nodiscard]] std::pair<std::shared_ptr<socket_stream>, transport::stream>
prepare_stream(std::shared_ptr<asio_tcp::socket> socket, asio::strand<asio::any_io_executor> strand,
               options tcp_options, std::int64_t id, std::shared_ptr<std::atomic<socket_state>> state,
               std::shared_ptr<forge::asio::notification> terminal_requested,
               std::shared_ptr<forge::asio::notification> terminal_completed, std::shared_ptr<void> lifetime) {
   auto model = std::make_shared<socket_stream>(std::move(socket), std::move(strand), tcp_options, id, std::move(state),
                                                std::move(terminal_requested), std::move(terminal_completed),
                                                std::move(lifetime));
   auto weak = std::weak_ptr<socket_stream>{model};
   auto stream = transport::detail::stream_access::make_cancelable(model, [weak = std::move(weak)]() noexcept {
      if (auto value = weak.lock()) {
         value->request_cancel();
      }
   });
   return {std::move(model), std::move(stream)};
}

} // namespace

struct connection::impl final : std::enable_shared_from_this<connection::impl> {
   enum class terminal_request_result {
      requested,
      already_terminal,
      stream_handed_off,
   };

   impl(asio_tcp::socket socket_value, options tcp_options_value, std::shared_ptr<void> lifetime_value)
       : socket(std::make_shared<asio_tcp::socket>(std::move(socket_value))), tcp_options(tcp_options_value),
         strand(asio::make_strand(socket->get_executor())), id(next_stream_id()),
         terminal_state(std::make_shared<std::atomic<socket_state>>(socket_state::active)),
         terminal_requested(std::make_shared<forge::asio::notification>()),
         terminal_completed(std::make_shared<forge::asio::notification>()), lifetime(std::move(lifetime_value)) {
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

   ~impl() {
      request_cancel();
   }

   void start_terminal_worker() {
      // The composed operation owns the socket and terminal state independently
      // of connection::impl. handed_off is terminal for this owner but never closes
      // the socket transferred to the stream or native caller.
      auto current = socket;
      auto state = terminal_state;
      auto requested = terminal_requested;
      auto completed = terminal_completed;
      auto lifetime_value = lifetime;
      asio::co_spawn(
          strand,
          [current = std::move(current), state = std::move(state), requested = std::move(requested), completed,
           lifetime_value = std::move(lifetime_value)]() mutable -> asio::awaitable<void> {
             static_cast<void>(lifetime_value);
             try {
                static_cast<void>(co_await requested->async_wait(0));
             } catch (...) {
                auto expected = socket_state::active;
                state->compare_exchange_strong(expected, socket_state::cancel_requested, std::memory_order_acq_rel,
                                               std::memory_order_acquire);
             }
             close_on_owner(*current, *state);
             completed->notify();
          },
          [completed = std::move(completed)](std::exception_ptr) noexcept { completed->notify(); });
   }

   [[nodiscard]] bool valid() const noexcept {
      const auto lock = std::scoped_lock{state_mutex};
      return !stream_handed_off && terminal_state->load(std::memory_order_acquire) == socket_state::active;
   }

   [[nodiscard]] transport::endpoint local_endpoint() const {
      if (!valid()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
      }
      return local;
   }

   [[nodiscard]] transport::endpoint remote_endpoint() const {
      if (!valid()) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
      }
      return remote;
   }

   boost::asio::awaitable<void> async_write(std::span<const std::uint8_t> bytes) {
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

   boost::asio::awaitable<std::size_t> async_read_some(std::span<std::uint8_t> bytes) {
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

   boost::asio::awaitable<std::vector<std::uint8_t>> async_read() {
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

   boost::asio::awaitable<void> async_close() {
      if (request_terminal(socket_state::close_requested) == terminal_request_result::stream_handed_off) {
         co_return;
      }
      terminal_requested->notify();
      static_cast<void>(co_await terminal_completed->async_wait(0));
   }

   void cancel() {
      request_cancel();
   }

   void request_cancel() noexcept {
      if (request_terminal(socket_state::cancel_requested) == terminal_request_result::stream_handed_off) {
         return;
      }
      terminal_requested->notify();
   }

   [[nodiscard]] transport::stream_connection into_transport_stream() {
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

   [[nodiscard]] asio_tcp::socket release_socket(std::shared_ptr<void>* lifetime_out) {
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

   [[nodiscard]] terminal_request_result request_terminal(socket_state requested) noexcept {
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

   static void close_on_owner(asio_tcp::socket& current, std::atomic<socket_state>& state) noexcept {
      auto observed = state.load(std::memory_order_acquire);
      while (observed == socket_state::cancel_requested || observed == socket_state::close_requested) {
         if (state.compare_exchange_weak(observed, socket_state::closed, std::memory_order_acq_rel,
                                         std::memory_order_acquire)) {
            cancel_socket(current);
            return;
         }
      }
   }

   [[nodiscard]] std::shared_ptr<asio_tcp::socket> detach_socket() {
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

   void commit_stream_handoff() {
      const auto lock = std::scoped_lock{state_mutex};
      if (terminal_state->load(std::memory_order_acquire) != socket_state::active) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "tcp connection cannot hand off a terminal socket");
      }
      if (active_operations != 0) {
         FORGE_THROW_EXCEPTION(exceptions::io_error, "tcp connection cannot hand off while I/O is active");
      }
      stream_handed_off = true;
   }

   void claim_operation() {
      const auto lock = std::scoped_lock{state_mutex};
      if (stream_handed_off || terminal_state->load(std::memory_order_acquire) != socket_state::active) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
      }
      ++active_operations;
   }

   void release_operation() noexcept {
      const auto lock = std::scoped_lock{state_mutex};
      --active_operations;
   }

   std::shared_ptr<asio_tcp::socket> socket;
   options tcp_options;
   asio::strand<asio::any_io_executor> strand;
   transport::endpoint local;
   transport::endpoint remote;
   std::int64_t id = -1;
   mutable std::mutex state_mutex;
   std::shared_ptr<std::atomic<socket_state>> terminal_state;
   std::shared_ptr<forge::asio::notification> terminal_requested;
   std::shared_ptr<forge::asio::notification> terminal_completed;
   std::shared_ptr<void> lifetime;
   std::size_t active_operations = 0;
   bool stream_handed_off = false;
};

connection::connection() = default;
connection::connection(boost::asio::ip::tcp::socket socket, options tcp_options, std::shared_ptr<void> lifetime)
    : impl_(std::make_shared<impl>(std::move(socket), tcp_options, std::move(lifetime))) {
   impl_->start_terminal_worker();
}
connection::~connection() = default;
connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&&) noexcept = default;

bool connection::valid() const noexcept {
   return impl_ && impl_->valid();
}

transport::endpoint connection::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->local_endpoint();
}

transport::endpoint connection::remote_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->remote_endpoint();
}

boost::asio::awaitable<void> connection::async_write(std::span<const std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   auto state = impl_;
   co_await state->async_write(bytes);
}

boost::asio::awaitable<std::size_t> connection::async_read_some(std::span<std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   auto state = impl_;
   co_return co_await state->async_read_some(bytes);
}

boost::asio::awaitable<std::vector<std::uint8_t>> connection::async_read() {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   auto state = impl_;
   co_return co_await state->async_read();
}

boost::asio::awaitable<void> connection::async_close() {
   if (!impl_) {
      co_return;
   }
   auto state = impl_;
   co_await state->async_close();
}

void connection::cancel() {
   request_cancel();
}

void connection::request_cancel() noexcept {
   if (impl_) {
      impl_->request_cancel();
   }
}

transport::stream_connection connection::into_transport_stream() && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->into_transport_stream();
}

boost::asio::ip::tcp::socket connection::release_socket() && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->release_socket(nullptr);
}

boost::asio::ip::tcp::socket connection::release_socket(std::shared_ptr<void>& lifetime) && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid tcp connection");
   }
   return impl_->release_socket(&lifetime);
}

} // namespace forge::net::tcp
