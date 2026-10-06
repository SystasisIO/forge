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
namespace {

namespace asio = boost::asio;

[[noreturn]] void throw_invalid_options(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_options, std::move(message));
}

[[noreturn]] void throw_handshake_failed(std::string message, const boost::system::error_code& error) {
   if (error == boost::asio::error::operation_aborted) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "stcp handshake canceled",
                            forge::exceptions::ctx("reason", error.message()));
   }
   if (error == boost::asio::error::eof || error == boost::asio::error::connection_reset ||
       error == boost::asio::error::broken_pipe || error == boost::asio::ssl::error::stream_truncated) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "stcp peer closed during handshake",
                            forge::exceptions::ctx("reason", error.message()));
   }
   FORGE_THROW_EXCEPTION(exceptions::handshake_failed, std::move(message),
                         forge::exceptions::ctx("reason", error.message()));
}

[[noreturn]] void throw_handshake_timeout(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::timeout, std::move(message));
}

[[noreturn]] void throw_verification_failed(std::string message) {
   FORGE_THROW_EXCEPTION(exceptions::verification_failed, std::move(message));
}

void validate_common(std::size_t read_chunk_size) {
   if (read_chunk_size == 0) {
      throw_invalid_options("stcp read_chunk_size must be greater than zero");
   }
}

[[nodiscard]] tls::context_options make_tls_options(const client_options& options) {
   auto out = tls::context_options{};
   out.role = tls::endpoint_role::client;
   out.protocols = options.tls13_only ? tls::protocol_policy::tls13_only : tls::protocol_policy::system_default;
   out.verification = options.security.verify_peer ? tls::peer_verification::verify_peer : tls::peer_verification::none;
   out.certificate_chain_pem = options.certificate_pem;
   out.private_key_pem = options.private_key_pem;
   out.alpn_protocols = options.alpn_protocols;
   if (!options.security.trusted_ca_pem.empty()) {
      out.trust_anchors_pem.push_back(options.security.trusted_ca_pem);
      out.use_default_verify_paths = false;
   }
   return out;
}

[[nodiscard]] tls::context_options make_tls_options(const server_options& options) {
   auto out = tls::context_options{};
   out.role = tls::endpoint_role::server;
   out.protocols = options.tls13_only ? tls::protocol_policy::tls13_only : tls::protocol_policy::system_default;
   if (options.security.verify_peer) {
      out.verification = tls::peer_verification::require_peer_certificate;
   } else if (options.security.require_peer_certificate) {
      out.verification = tls::peer_verification::require_peer_certificate_for_application_verification;
   } else {
      out.verification = tls::peer_verification::none;
   }
   out.certificate_chain_pem = options.certificate_pem;
   out.private_key_pem = options.private_key_pem;
   out.alpn_protocols = options.alpn_protocols;
   out.use_default_verify_paths = options.security.verify_peer;
   if (!options.security.trusted_ca_pem.empty()) {
      out.trust_anchors_pem.push_back(options.security.trusted_ca_pem);
      out.use_default_verify_paths = false;
   }
   return out;
}

[[nodiscard]] tls::context_snapshot_ptr make_client_context(const client_options& options) {
   validate_common(options.read_chunk_size);
   try {
      return tls::make_context(make_tls_options(options));
   } catch (const forge::exceptions::base& error) {
      throw_invalid_options("invalid stcp TLS client options: " + error.message());
   }
}

[[nodiscard]] tls::context_snapshot_ptr make_server_context(const server_options& options) {
   validate_common(options.read_chunk_size);
   try {
      return tls::make_context(make_tls_options(options));
   } catch (const forge::exceptions::base& error) {
      throw_invalid_options("invalid stcp TLS server options: " + error.message());
   }
}

void configure_tls_client_stream(SSL* native_handle, const client_options& options, std::string_view remote_host,
                                 const tls::context_snapshot& context) {
   try {
      tls::configure_client_stream(
          native_handle, context,
          {.sni = options.sni, .endpoint_host = std::string{remote_host}, .server_name = options.server_name});
   } catch (const forge::exceptions::base& error) {
      if (tls::exceptions::code_of(error)) {
         throw_invalid_options("invalid stcp TLS client stream options: " + error.message());
      }
      throw;
   }
}

void classify_tls_handshake_failure(SSL* native_handle, const tls::context_snapshot& context) {
   try {
      tls::classify_handshake_failure(native_handle, context);
   } catch (const forge::exceptions::base& error) {
      if (tls::exceptions::code_of(error)) {
         throw_verification_failed("stcp TLS peer verification failed: " + error.message());
      }
      throw;
   }
}

void validate_tls_peer(SSL* native_handle, const tls::context_snapshot& context, const security_options& security,
                       std::string_view expected_host) {
   try {
      tls::validate_peer(native_handle, context,
                         {.expected_host = security.verify_peer ? std::string{expected_host} : std::string{},
                          .expected_sha256_fingerprint = security.expected_sha256_fingerprint,
                          .verifier = security.verifier});
   } catch (const forge::exceptions::base& error) {
      if (tls::exceptions::code_of(error)) {
         throw_verification_failed("stcp TLS peer verification failed: " + error.message());
      }
      throw;
   }
}

void validate_handshake_timeout(std::chrono::milliseconds timeout) {
   if (timeout.count() <= 0) {
      throw_invalid_options("stcp handshake timeout must be greater than zero");
   }
}

enum class handshake_cancellation_state : std::uint8_t {
   active,
   canceled,
   terminal,
};

boost::asio::awaitable<void> async_handshake(std::shared_ptr<detail::stream_backend> stream,
                                             asio::ssl::stream_base::handshake_type type,
                                             std::optional<std::chrono::milliseconds> timeout, std::stop_token stop,
                                             std::shared_ptr<detail::connection_test_hooks> hooks = {}) {
   auto strand = asio::make_strand(stream->get_executor());
   co_await asio::co_spawn(
       strand,
       [stream = std::move(stream), strand, type, timeout, stop, hooks = std::move(hooks)]() -> asio::awaitable<void> {
          if (stop.stop_requested()) {
             FORGE_THROW_EXCEPTION(exceptions::canceled, "stcp handshake canceled");
          }

          auto cancellation =
              std::make_shared<std::atomic<handshake_cancellation_state>>(handshake_cancellation_state::active);
          auto cancel_requested = std::make_shared<forge::asio::notification>();
          auto cancel_completed = std::make_shared<forge::asio::notification>();
          auto cancel_worker_error = std::make_shared<std::exception_ptr>();
          auto timer_completed = std::make_shared<forge::asio::notification>();
          asio::co_spawn(
              strand,
              [stream, cancellation, cancel_requested]() -> asio::awaitable<void> {
                 static_cast<void>(co_await cancel_requested->async_wait(0));
                 if (cancellation->load(std::memory_order_acquire) == handshake_cancellation_state::canceled) {
                    stream->request_cancel();
                 }
              },
              [cancel_completed, cancel_worker_error](std::exception_ptr error) noexcept {
                 *cancel_worker_error = std::move(error);
                 cancel_completed->notify();
              });
          auto request_cancel = [cancellation, cancel_requested]() noexcept {
             auto expected = handshake_cancellation_state::active;
             if (cancellation->compare_exchange_strong(expected, handshake_cancellation_state::canceled,
                                                       std::memory_order_acq_rel, std::memory_order_acquire)) {
                cancel_requested->notify();
             }
          };
          using stop_callback_type = std::stop_callback<decltype(request_cancel)>;
          auto cancel_on_stop = std::optional<stop_callback_type>{};
          auto timer = std::shared_ptr<asio::steady_timer>{};
          auto terminal = std::shared_ptr<detail::handshake_deadline_state>{};
          auto error = boost::system::error_code{};
          auto primary_error = std::exception_ptr{};
          auto timer_started = false;
          try {
             cancel_on_stop.emplace(stop, std::move(request_cancel));
             if (timeout) {
                validate_handshake_timeout(*timeout);
                timer = std::make_shared<asio::steady_timer>(co_await asio::this_coro::executor);
                terminal = std::make_shared<detail::handshake_deadline_state>();
                timer->expires_after(*timeout);
                timer->async_wait([stream, terminal, timer_completed, hooks](
                                     const boost::system::error_code& timer_error) noexcept {
                   if (hooks && hooks->timer_callback) {
                      hooks->timer_callback(hooks->state.get(), timer_error);
                   }
                   if (!timer_error && terminal->try_timeout()) {
                      stream->request_cancel();
                   }
                   timer_completed->notify();
                });
                timer_started = true;
             }

             error = co_await stream->async_handshake(type);
          } catch (...) {
             primary_error = std::current_exception();
          }

          auto expected = handshake_cancellation_state::active;
          const auto completed = cancellation->compare_exchange_strong(
              expected, handshake_cancellation_state::terminal, std::memory_order_acq_rel, std::memory_order_acquire);
          const auto canceled = !completed && expected == handshake_cancellation_state::canceled;
          cancel_on_stop.reset();
          const auto completed_before_timeout = !terminal || terminal->try_complete();

          // Parent cancellation must not interrupt terminal cleanup. If waiter
          // setup itself fails, published callbacks still retain the real
          // backend and its token; there is no allocation-retry loop.
          co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
          cancel_requested->notify();
          try {
             if (timer_started) {
                timer->cancel();
             }
             if (cancel_completed->epoch() == 0) {
                static_cast<void>(co_await cancel_completed->async_wait(0));
             }
             // Canceling a timer only queues its callback. Join that actual
             // callback before verification or connection publication.
             if (timer_started && timer_completed->epoch() == 0) {
                static_cast<void>(co_await timer_completed->async_wait(0));
             }
          } catch (...) {
             if (!primary_error) {
                primary_error = std::current_exception();
             }
          }
          if (primary_error || *cancel_worker_error) {
             if (primary_error) {
                std::rethrow_exception(primary_error);
             }
             std::rethrow_exception(*cancel_worker_error);
          }
          if (!completed_before_timeout) {
             throw_handshake_timeout(type == asio::ssl::stream_base::client ? "stcp client handshake timed out"
                                                                            : "stcp server handshake timed out");
          }
          if (canceled) {
             FORGE_THROW_EXCEPTION(exceptions::canceled, "stcp handshake canceled");
          }
          if (error) {
             throw_handshake_failed(type == asio::ssl::stream_base::client ? "stcp client handshake failed"
                                                                           : "stcp server handshake failed",
                                    error);
          }
       },
       asio::use_awaitable);
}

} // namespace

connection::connection() = default;
connection::connection(backend_token, std::shared_ptr<detail::stream_backend> stream, tls::context_snapshot_ptr context,
                       std::size_t read_chunk_size, transport::endpoint local, transport::endpoint remote,
                       std::shared_ptr<void> lifetime, std::shared_ptr<detail::connection_test_hooks> hooks) {
   try {
      if (hooks && hooks->startup) {
         hooks->startup(hooks->state.get(), detail::startup_stage::owner_allocation);
      }
      impl_ = std::make_shared<impl>(stream, std::move(context), read_chunk_size, std::move(local),
                                     std::move(remote), lifetime);
      if (hooks && hooks->startup) {
         hooks->startup(hooks->state.get(), detail::startup_stage::terminal_launch);
      }
      impl_->start_terminal_worker();
   } catch (...) {
      if (impl_) {
         impl_->cancel();
      }
      stream->request_cancel();
      throw;
   }
}
connection::~connection() {
   if (impl_) {
      impl_->cancel();
   }
}
connection::connection(connection&&) noexcept = default;
connection& connection::operator=(connection&& other) noexcept {
   if (this != &other) {
      if (impl_) {
         impl_->cancel();
      }
      impl_ = std::move(other.impl_);
   }
   return *this;
}

bool connection::valid() const noexcept {
   return impl_ && impl_->valid();
}

transport::endpoint connection::local_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return impl_->local_endpoint();
}

transport::endpoint connection::remote_endpoint() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return impl_->remote_endpoint();
}

std::optional<peer_certificate> connection::peer_certificate() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return impl_->certificate_value;
}

certificate_chain connection::peer_certificate_chain() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return impl_->chain_value;
}

std::string connection::selected_alpn() const {
   if (!valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return impl_->alpn_value;
}

boost::asio::awaitable<void> connection::async_write(std::span<const std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   auto state = impl_;
   co_await state->async_write(bytes);
}

boost::asio::awaitable<std::size_t> connection::async_read_some(std::span<std::uint8_t> bytes) {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   auto state = impl_;
   co_return co_await state->async_read_some(bytes);
}

boost::asio::awaitable<std::vector<std::uint8_t>> connection::async_read() {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
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
   if (impl_) {
      impl_->cancel();
   }
}

transport::stream_connection connection::into_transport_stream() && {
   if (!impl_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid stcp connection");
   }
   return impl_->into_transport_stream();
}

boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::optional<std::chrono::milliseconds> timeout,
                                                        std::stop_token stop);
boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::optional<std::chrono::milliseconds> timeout,
                                                        std::stop_token stop);

boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options) {
   co_return co_await async_upgrade_client(std::move(source), std::move(options), std::nullopt, {});
}

boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::chrono::milliseconds timeout) {
   co_return co_await async_upgrade_client(std::move(source), std::move(options), std::optional{timeout}, {});
}

boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::stop_token stop) {
   co_return co_await async_upgrade_client(std::move(source), std::move(options), std::nullopt, stop);
}

boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::chrono::milliseconds timeout, std::stop_token stop) {
   co_return co_await async_upgrade_client(std::move(source), std::move(options), std::optional{timeout}, stop);
}

boost::asio::awaitable<connection> async_upgrade_client(tcp::connection source, client_options options,
                                                        std::optional<std::chrono::milliseconds> timeout,
                                                        std::stop_token stop) {
   co_return co_await connection::async_upgrade_native(std::move(source), std::move(options), timeout, stop);
}

boost::asio::awaitable<connection> connection::async_upgrade_native(
    tcp::connection source, client_options options, std::optional<std::chrono::milliseconds> timeout,
    std::stop_token stop, std::shared_ptr<detail::connection_test_hooks> hooks) {
   if (!source.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid source tcp connection");
   }
   const auto local = source.local_endpoint();
   const auto remote = source.remote_endpoint();
   auto context = make_client_context(options);
   auto lifetime = std::shared_ptr<void>{};
   auto socket = std::move(source).release_socket(lifetime);
   auto native = std::shared_ptr<tls::asio_tls_stream>{};
   auto stream = std::shared_ptr<detail::stream_backend>{};
   auto result = connection{};
   auto failure = std::exception_ptr{};
   try {
      native = tls::make_asio_stream(context, std::move(socket));
      stream = detail::make_native_stream_backend(native, lifetime, hooks);
      configure_tls_client_stream(stream->native_handle(), options, remote.host, *context);
      try {
         co_await async_handshake(stream, asio::ssl::stream_base::client, timeout, stop, hooks);
      } catch (const exceptions::handshake_failed&) {
         classify_tls_handshake_failure(stream->native_handle(), *context);
         throw;
      }
      const auto expected_host = options.server_name.empty() ? remote.host : options.server_name;
      validate_tls_peer(stream->native_handle(), *context, options.security, expected_host);
      result = connection{backend_token{}, stream, context, options.read_chunk_size, local, remote, lifetime, hooks};
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure) {
      // No further TLS I/O is published. Close the actual lower before any
      // startup token can unwind; canceled timer callbacks retain the backend.
      if (stream) {
         stream->request_cancel();
      } else {
         detail::close_native_socket(native ? native->next_layer() : socket, hooks);
      }
      std::rethrow_exception(failure);
   }
   co_return result;
}

boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options) {
   co_return co_await async_upgrade_server(std::move(source), std::move(options), std::nullopt, {});
}

boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::chrono::milliseconds timeout) {
   co_return co_await async_upgrade_server(std::move(source), std::move(options), std::optional{timeout}, {});
}

boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::stop_token stop) {
   co_return co_await async_upgrade_server(std::move(source), std::move(options), std::nullopt, stop);
}

boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::chrono::milliseconds timeout, std::stop_token stop) {
   co_return co_await async_upgrade_server(std::move(source), std::move(options), std::optional{timeout}, stop);
}

boost::asio::awaitable<connection> async_upgrade_server(tcp::connection source, server_options options,
                                                        std::optional<std::chrono::milliseconds> timeout,
                                                        std::stop_token stop) {
   co_return co_await connection::async_upgrade_native(std::move(source), std::move(options), timeout, stop);
}

boost::asio::awaitable<connection> connection::async_upgrade_native(
    tcp::connection source, server_options options, std::optional<std::chrono::milliseconds> timeout,
    std::stop_token stop) {
   if (!source.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid source tcp connection");
   }
   const auto local = source.local_endpoint();
   const auto remote = source.remote_endpoint();
   auto context = make_server_context(options);
   auto lifetime = std::shared_ptr<void>{};
   auto socket = std::move(source).release_socket(lifetime);
   auto native = std::shared_ptr<tls::asio_tls_stream>{};
   auto stream = std::shared_ptr<detail::stream_backend>{};
   auto result = connection{};
   auto failure = std::exception_ptr{};
   try {
      native = tls::make_asio_stream(context, std::move(socket));
      stream = detail::make_native_stream_backend(native, lifetime);
      co_await async_handshake(stream, asio::ssl::stream_base::server, timeout, stop);
      validate_tls_peer(stream->native_handle(), *context, options.security, {});
      result = connection{backend_token{}, stream, context, options.read_chunk_size, local, remote, lifetime};
   } catch (...) {
      failure = std::current_exception();
   }
   if (failure) {
      if (stream) {
         stream->request_cancel();
      } else {
         detail::close_native_socket(native ? native->next_layer() : socket);
      }
      std::rethrow_exception(failure);
   }
   co_return result;
}

boost::asio::awaitable<connection>
async_upgrade_client(transport::stream_connection source, client_options options,
                     std::optional<std::chrono::milliseconds> timeout, std::stop_token stop) {
   if (!source.stream.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid source transport stream");
   }
   const auto local = source.local_endpoint;
   const auto remote = source.remote_endpoint;
   const auto executor = co_await asio::this_coro::executor;
   auto context = make_client_context(options);
   auto stream = detail::make_transport_stream_backend(executor, std::move(source.stream), context);
   configure_tls_client_stream(stream->native_handle(), options, remote.host, *context);

   try {
      co_await async_handshake(stream, asio::ssl::stream_base::client, timeout, stop);
   } catch (const exceptions::handshake_failed&) {
      classify_tls_handshake_failure(stream->native_handle(), *context);
      throw;
   }
   const auto expected_host = options.server_name.empty() ? remote.host : options.server_name;
   validate_tls_peer(stream->native_handle(), *context, options.security, expected_host);
   co_return connection{connection::backend_token{}, std::move(stream), std::move(context), options.read_chunk_size,
                        local, remote, {}};
}

boost::asio::awaitable<connection>
async_upgrade_server(transport::stream_connection source, server_options options,
                     std::optional<std::chrono::milliseconds> timeout, std::stop_token stop) {
   if (!source.stream.valid()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "invalid source transport stream");
   }
   const auto local = source.local_endpoint;
   const auto remote = source.remote_endpoint;
   const auto executor = co_await asio::this_coro::executor;
   auto context = make_server_context(options);
   auto stream = detail::make_transport_stream_backend(executor, std::move(source.stream), context);
   co_await async_handshake(stream, asio::ssl::stream_base::server, timeout, stop);
   validate_tls_peer(stream->native_handle(), *context, options.security, {});
   co_return connection{connection::backend_token{}, std::move(stream), std::move(context), options.read_chunk_size,
                        local, remote, {}};
}

} // namespace forge::net::stcp
