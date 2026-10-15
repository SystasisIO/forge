module;

#include <chrono>
#include <algorithm>
#include <atomic>
#include <coroutine>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <mutex>
#include <utility>
#include <variant>
#include <vector>

#include <forge/exceptions/macros.hpp>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/connect.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/experimental/awaitable_operators.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/version.hpp>

module forge.net.http.connection;

import forge.asio.runtime;
import forge.asio.exceptions;
import forge.net.http.body;
import forge.net.http.exceptions;
import forge.net.tls.context;

namespace forge::net::http {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace beast_http = boost::beast::http;
using tcp = asio::ip::tcp;
using asio::awaitable;
using asio::use_awaitable;
using transport_deadline = std::chrono::steady_clock::time_point;

[[nodiscard]] tls::context_options make_https_client_context_options() {
   auto options = tls::context_options{};
   options.protocols = tls::protocol_policy::system_default;
   return options;
}

[[nodiscard]] std::shared_ptr<tls::context_provider> make_https_client_context_provider() {
   return std::make_shared<tls::context_provider>(make_https_client_context_options());
}

transport_deadline make_deadline(const request_options& options) noexcept {
   const auto now = std::chrono::steady_clock::now();
   const auto maximum = transport_deadline::max();
   const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(maximum - now);
   const auto relative = options.timeout <= std::chrono::milliseconds::zero() ? now
                         : options.timeout >= remaining ? maximum : now + options.timeout;
   return std::min(relative, options.deadline.value_or(maximum));
}

bool deadline_expired(transport_deadline deadline) noexcept {
   return std::chrono::steady_clock::now() >= deadline;
}

template <typename Stream> void expire_at(Stream& stream, transport_deadline deadline) {
   beast::get_lowest_layer(stream).expires_at(deadline);
}

[[noreturn]] void raise_deadline() {
   throw exceptions::gateway_timeout{"HTTP request exceeded its transport deadline"};
}

void require_tls_peer_verification(const tls::context_snapshot_ptr& snapshot) {
   if (!snapshot || snapshot->verification() != tls::peer_verification::verify_peer) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTPS client requires TLS peer verification");
   }
}

void require_deadline(transport_deadline deadline) {
   if (deadline_expired(deadline)) {
      raise_deadline();
   }
}

bool connection_reset_error(const boost::system::error_code& error) {
   return error == asio::error::broken_pipe || error == asio::error::connection_reset || error == asio::error::eof ||
          error == boost::beast::http::error::end_of_stream;
}

bool timeout_error(const boost::system::error_code& error) {
   return error == beast::error::timeout || error == asio::error::timed_out;
}

bool cancellation_error(const boost::system::error_code& error) {
   return error == asio::error::operation_aborted;
}

[[noreturn]] void raise_transport_error(const boost::system::system_error& error) {
   if (timeout_error(error.code())) {
      throw exceptions::gateway_timeout{"HTTP request exceeded its transport deadline"};
   }
   if (cancellation_error(error.code())) {
      throw forge::asio::exceptions::canceled{"HTTP request was canceled"};
   }
   throw exceptions::unavailable{"HTTP transport request failed"};
}

[[noreturn]] void raise_transport_implementation_error(const std::exception& error) {
   FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP transport implementation failed",
                         forge::exceptions::ctx("reason", error.what()));
}

[[noreturn]] void raise_transport_implementation_error() {
   FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP transport implementation failed");
}

void ensure_host_header(request& request_value, const base_url& endpoint) {
   if (request_value.find(field::host) == request_value.end()) {
      request_value.set(field::host, endpoint.host);
   }
}

beast_http::verb to_beast_method(method value) noexcept {
   switch (value) {
   case method::delete_:
      return beast_http::verb::delete_;
   case method::get:
      return beast_http::verb::get;
   case method::head:
      return beast_http::verb::head;
   case method::options:
      return beast_http::verb::options;
   case method::patch:
      return beast_http::verb::patch;
   case method::post:
      return beast_http::verb::post;
   case method::put:
      return beast_http::verb::put;
   case method::unknown:
      return beast_http::verb::unknown;
   }
   return beast_http::verb::unknown;
}

status to_http_status(beast_http::status value) noexcept {
   return static_cast<status>(static_cast<unsigned>(value));
}

beast_http::request<beast_http::string_body> to_beast_request(const request& source) {
   auto target = beast_http::request<beast_http::string_body>{to_beast_method(source.method()),
                                                              std::string{source.target()}, source.version()};
   for (const auto& header : source.headers()) {
      target.insert(header.name, header.text);
   }
   target.keep_alive(source.keep_alive());
   target.body() = source.body();
   target.prepare_payload();
   return target;
}

response to_http_response(const beast_http::response<beast_http::string_body>& source) {
   auto target = response{to_http_status(source.result()), source.version()};
   target.keep_alive(source.keep_alive());
   for (const auto& header : source) {
      target.insert(header.name_string(), header.value());
   }
   target.body() = source.body();
   return target;
}

response make_header_response(const beast_http::response_parser<beast_http::buffer_body>& parser) {
   const auto& source = parser.get();
   auto output = response{to_http_status(source.result()), source.version()};
   output.keep_alive(source.keep_alive());
   for (const auto& field_value : source) {
      output.insert(field_value.name_string(), field_value.value());
   }
   return output;
}

template <typename Stream>
class beast_response_body_source final : public body_reader::source,
                                         public std::enable_shared_from_this<beast_response_body_source<Stream>> {
 public:
   beast_response_body_source(Stream stream, beast::flat_buffer buffer,
                              std::shared_ptr<beast_http::response_parser<beast_http::buffer_body>> parser,
                              transport_deadline deadline, tls::context_snapshot_ptr tls_context_snapshot = {})
       : stream_(std::move(stream)), buffer_(std::move(buffer)), parser_(std::move(parser)), deadline_(deadline),
         tls_context_snapshot_(std::move(tls_context_snapshot)) {}

   ~beast_response_body_source() override {
      auto ignored = boost::system::error_code{};
      beast::get_lowest_layer(stream_).socket().shutdown(tcp::socket::shutdown_both, ignored);
      beast::get_lowest_layer(stream_).socket().close(ignored);
   }

   awaitable<std::optional<body_chunk>> async_read() override try {
      if (parser_->is_done()) {
         co_return std::nullopt;
      }

      for (;;) {
         auto storage = std::vector<std::byte>(64U * 1024U);
         auto& body = parser_->get().body();
         body.data = storage.data();
         body.size = storage.size();

         require_deadline(deadline_);
         expire_at(stream_, deadline_);
         auto [read_error, bytes] =
             co_await beast_http::async_read_some(stream_, buffer_, *parser_, asio::as_tuple(use_awaitable));
         static_cast<void>(bytes);

         const auto produced = storage.size() - body.size;
         if (read_error == beast_http::error::need_buffer) {
            read_error = {};
         }
         if (read_error) {
            if (cancelled_.load(std::memory_order_acquire)) {
               throw forge::asio::exceptions::canceled{"HTTP response body read was canceled"};
            }
            if (cancellation_error(read_error)) {
               if (deadline_expired(deadline_)) {
                  throw exceptions::gateway_timeout{"HTTP response body exceeded its transport deadline"};
               }
               const auto cancellation = co_await asio::this_coro::cancellation_state;
               if (cancellation.cancelled() == asio::cancellation_type::none) {
                  throw exceptions::gateway_timeout{"HTTP response body exceeded its transport deadline"};
               }
            }
            throw boost::system::system_error{read_error};
         }

         bytes_read_ += produced;
         if (produced != 0U) {
            storage.resize(produced);
            co_return body_chunk{.bytes = std::move(storage)};
         }
         if (parser_->is_done()) {
            co_return std::nullopt;
         }
      }
   } catch (const forge::exceptions::base&) {
      cancel();
      throw;
   } catch (const boost::system::system_error& error) {
      cancel();
      raise_transport_error(error);
   } catch (const std::exception& error) {
      cancel();
      raise_transport_implementation_error(error);
   } catch (...) {
      cancel();
      raise_transport_implementation_error();
   }

   [[nodiscard]] std::uint64_t bytes_read() const noexcept override {
      return bytes_read_;
   }

   void cancel() noexcept override {
      cancelled_.store(true, std::memory_order_release);
      try {
         asio::dispatch(stream_.get_executor(), [self = this->shared_from_this()] {
            auto ignored = boost::system::error_code{};
            auto& socket = beast::get_lowest_layer(self->stream_).socket();
            socket.cancel(ignored);
            socket.set_option(asio::socket_base::linger{true, 0}, ignored);
            socket.close(ignored);
         });
      } catch (...) {
         // Cancellation is a best-effort no-throw operation.
      }
   }

 private:
   Stream stream_;
   beast::flat_buffer buffer_;
   std::shared_ptr<beast_http::response_parser<beast_http::buffer_body>> parser_;
   transport_deadline deadline_;
   tls::context_snapshot_ptr tls_context_snapshot_;
   std::uint64_t bytes_read_ = 0;
   std::atomic_bool cancelled_ = false;
};

awaitable<bool> cancel_body_at_deadline(body_reader& body, transport_deadline deadline) {
   auto timer = asio::steady_timer{co_await asio::this_coro::executor, deadline};
   auto [error] = co_await timer.async_wait(asio::as_tuple(use_awaitable));
   if (!error) {
      body.cancel();
      co_return true;
   }
   if (error == asio::error::operation_aborted) {
      co_return false;
   }
   throw boost::system::system_error{error};
}

awaitable<std::optional<body_chunk>> read_body_until(body_reader& body, transport_deadline deadline) {
   require_deadline(deadline);
   const auto executor = co_await asio::this_coro::executor;
   const auto inherited_cancellation = co_await asio::this_coro::cancellation_state;
   auto parent_cancellation_slot = inherited_cancellation.slot();
   auto body_cancellation = asio::cancellation_state{parent_cancellation_slot,
                                                     [&body](asio::cancellation_type type) noexcept {
                                                        if (type != asio::cancellation_type::none) {
                                                           body.cancel();
                                                        }
                                                        return type;
                                                     },
                                                     asio::enable_total_cancellation{}};
   const auto clear_cancellation = [](asio::cancellation_slot* slot) noexcept {
      try {
         slot->clear();
      } catch (...) {
         // Cancellation cleanup cannot escape the body-read boundary.
      }
   };
   auto cancellation_cleanup = std::unique_ptr<asio::cancellation_slot, decltype(clear_cancellation)>{
       &parent_cancellation_slot, clear_cancellation};
   if (inherited_cancellation.cancelled() != asio::cancellation_type::none) {
      body.cancel();
      throw forge::asio::exceptions::canceled{"HTTP request body read was canceled"};
   }
   using namespace asio::experimental::awaitable_operators;
   auto outcome = co_await asio::co_spawn(executor,
                                          asio::co_spawn(executor, body.async_read(), asio::as_tuple(use_awaitable)) ||
                                              cancel_body_at_deadline(body, deadline),
                                          asio::bind_cancellation_slot(body_cancellation.slot(), use_awaitable));
   if (body_cancellation.cancelled() != asio::cancellation_type::none) {
      throw forge::asio::exceptions::canceled{"HTTP request body read was canceled"};
   }
   if (outcome.index() != 0U) {
      if (std::get<1>(outcome)) {
         raise_deadline();
      }
      throw forge::asio::exceptions::canceled{"HTTP request body read was canceled"};
   }
   auto [error, chunk] = std::get<0>(std::move(outcome));
   if (error) {
      if (deadline_expired(deadline)) {
         raise_deadline();
      }
      std::rethrow_exception(error);
   }
   require_deadline(deadline);
   co_return std::move(chunk);
}

template <typename Stream>
awaitable<void> write_streaming_request(Stream& stream, const request& request_value, body_reader& body,
                                        transport_deadline deadline) {
   auto message = beast_http::request<beast_http::buffer_body>{
       to_beast_method(request_value.method()), std::string{request_value.target()}, request_value.version()};
   for (const auto& field_value : request_value.headers()) {
      message.set(field_value.name, field_value.text);
   }
   message.keep_alive(request_value.keep_alive());
   message.chunked(true);

   auto serializer = beast_http::request_serializer<beast_http::buffer_body>{message};
   serializer.split(true);
   require_deadline(deadline);
   expire_at(stream, deadline);
   co_await beast_http::async_write_header(stream, serializer, use_awaitable);

   while (auto chunk = co_await read_body_until(body, deadline)) {
      require_deadline(deadline);
      auto& body_value = message.body();
      body_value.data = chunk->bytes.data();
      body_value.size = chunk->bytes.size();
      body_value.more = true;
      expire_at(stream, deadline);
      auto [body_error, body_bytes] =
          co_await beast_http::async_write(stream, serializer, asio::as_tuple(use_awaitable));
      static_cast<void>(body_bytes);
      if (body_error && body_error != beast_http::error::need_buffer) {
         throw boost::system::system_error{body_error};
      }
   }

   auto& body_value = message.body();
   body_value.data = nullptr;
   body_value.size = 0;
   body_value.more = false;
   require_deadline(deadline);
   expire_at(stream, deadline);
   auto [final_error, final_bytes] =
       co_await beast_http::async_write(stream, serializer, asio::as_tuple(use_awaitable));
   static_cast<void>(final_bytes);
   if (final_error && final_error != beast_http::error::need_buffer) {
      throw boost::system::system_error{final_error};
   }
}

} // namespace

struct connection::impl : std::enable_shared_from_this<connection::impl> {
   struct queued_request {
      explicit queued_request(asio::io_context& context)
          : completion_timer(context, (std::chrono::steady_clock::time_point::max)()) {}

      forge::net::http::request request_value;
      request_options options;
      transport_deadline deadline = transport_deadline::max();
      boost::asio::steady_timer completion_timer;
      mutable std::mutex completion_mutex;
      std::optional<response> result;
      std::exception_ptr error;
      asio::cancellation_signal transport_cancellation;
      bool completed = false;

      bool complete_response(response response_value) {
         {
            const auto lock = std::scoped_lock{completion_mutex};
            if (completed) {
               return false;
            }
            result = std::move(response_value);
            completed = true;
         }
         completion_timer.cancel();
         return true;
      }

      bool complete_error(std::exception_ptr error_value) {
         {
            const auto lock = std::scoped_lock{completion_mutex};
            if (completed) {
               return false;
            }
            error = std::move(error_value);
            completed = true;
         }
         completion_timer.cancel();
         return true;
      }

      bool is_completed() const {
         const auto lock = std::scoped_lock{completion_mutex};
         return completed;
      }

      response take_result() {
         auto result_value = std::optional<response>{};
         auto error_value = std::exception_ptr{};
         {
            const auto lock = std::scoped_lock{completion_mutex};
            result_value = std::move(result);
            error_value = error;
         }
         if (error_value) {
            try {
               std::rethrow_exception(error_value);
            } catch (const forge::exceptions::base&) {
               throw;
            } catch (const std::exception& error) {
               FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP transport request failed",
                                     forge::exceptions::ctx("reason", error.what()));
            } catch (...) {
               FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP transport request failed");
            }
         }
         if (!result_value.has_value()) {
            FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP request completed without response");
         }
         return std::move(*result_value);
      }
   };

   struct caller_cancellation_filter {
      std::weak_ptr<impl> owner;
      std::weak_ptr<queued_request> operation;

      asio::cancellation_type_t operator()(asio::cancellation_type_t type) const noexcept {
         if (type != asio::cancellation_type::none) {
            if (auto owner_value = owner.lock()) {
               if (auto operation_value = operation.lock()) {
                  owner_value->cancel_request(operation_value);
               }
            }
         }
         return asio::cancellation_type::none;
      }
   };

   explicit impl(forge::asio::runtime& runtime_value, base_url endpoint_value,
                 std::shared_ptr<tls::context_provider> tls_context_provider_value,
                 tls::client_stream_options tls_stream_options_value, tls::peer_validation tls_peer_validation_value)
       : runtime(runtime_value), endpoint(std::move(endpoint_value)), strand(asio::make_strand(runtime.context())),
         tls_context_provider_ptr(std::move(tls_context_provider_value)),
         tls_stream_options(std::move(tls_stream_options_value)),
         tls_peer_validation(std::move(tls_peer_validation_value)) {
      if (!tls_context_provider_ptr) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP TLS context provider must not be empty");
      }
   }

   awaitable<tcp::resolver::results_type> resolve(transport_deadline deadline) {
      require_deadline(deadline);
      auto request_resolver = tcp::resolver{strand};
      auto timer = asio::steady_timer{strand, deadline};
      using namespace asio::experimental::awaitable_operators;
      auto outcome =
          co_await (request_resolver.async_resolve(endpoint.host, endpoint.port, asio::as_tuple(use_awaitable)) ||
                    timer.async_wait(asio::as_tuple(use_awaitable)));
      if (outcome.index() != 0U) {
         const auto [timer_error] = std::get<1>(outcome);
         if (!timer_error) {
            raise_deadline();
         }
         throw boost::system::system_error{timer_error};
      }
      auto [error, results] = std::get<0>(std::move(outcome));
      if (error) {
         if (deadline_expired(deadline)) {
            raise_deadline();
         }
         throw boost::system::system_error{error};
      }
      require_deadline(deadline);
      co_return results;
   }

   awaitable<void> ensure_plain_connected(transport_deadline deadline) {
      if (plain_stream && plain_connected) {
         co_return;
      }

      auto results = co_await resolve(deadline);
      auto stream = beast::tcp_stream{strand};
      co_await stream.async_connect(results, use_awaitable);
      plain_stream = std::make_unique<beast::tcp_stream>(std::move(stream));
      if (plain_connected_once) {
         record_reconnect();
      }
      plain_connected_once = true;
      plain_connected = true;
   }

   void configure_tls_client_stream(beast::ssl_stream<beast::tcp_stream>& stream,
                                    const tls::context_snapshot& snapshot) const {
      auto options = tls_stream_options;
      if (options.endpoint_host.empty()) {
         options.endpoint_host = endpoint.host;
      }
      tls::configure_client_stream(stream.native_handle(), snapshot, std::move(options));
   }

   void validate_tls_peer(beast::ssl_stream<beast::tcp_stream>& stream, const tls::context_snapshot& snapshot) const {
      auto validation = tls_peer_validation;
      if (validation.expected_host.empty()) {
         validation.expected_host = endpoint.host;
      }
      tls::validate_peer(stream.native_handle(), snapshot, validation);
   }

   awaitable<void> ensure_tls_connected(transport_deadline deadline) {
      if (tls_stream && tls_connected) {
         co_return;
      }

      auto results = co_await resolve(deadline);

      auto& tls_context_provider = *tls_context_provider_ptr;
      auto context_snapshot = tls_context_provider.snapshot();
      require_tls_peer_verification(context_snapshot);
      auto stream = tls::make_beast_stream(context_snapshot, beast::tcp_stream{strand});
      configure_tls_client_stream(*stream, *context_snapshot);

      co_await beast::get_lowest_layer(*stream).async_connect(results, use_awaitable);
      require_deadline(deadline);
      try {
         co_await stream->async_handshake(asio::ssl::stream_base::client, use_awaitable);
      } catch (const boost::system::system_error&) {
         tls::classify_handshake_failure(stream->native_handle(), *context_snapshot);
         throw;
      }
      validate_tls_peer(*stream, *context_snapshot);
      tls_stream = std::move(stream);
      if (tls_connected_once) {
         record_reconnect();
      }
      tls_connected_once = true;
      tls_connected = true;
   }

   awaitable<response> do_plain_request(forge::net::http::request request_value, transport_deadline deadline,
                                        std::uint64_t max_response_body_bytes) {
      co_await ensure_plain_connected(deadline);

      ensure_host_header(request_value, endpoint);
      auto beast_request = to_beast_request(request_value);
      require_deadline(deadline);
      co_await boost::beast::http::async_write(*plain_stream, beast_request, use_awaitable);

      buffer.consume(buffer.size());
      auto parser = beast_http::response_parser<beast_http::string_body>{};
      parser.body_limit(max_response_body_bytes);
      if (request_value.method() == method::head) {
         parser.skip(true);
      }
      co_await beast_http::async_read(*plain_stream, buffer, parser, use_awaitable);
      auto response_value = to_http_response(parser.release());

      if (!response_value.keep_alive()) {
         close_plain();
      }
      record_status(response_value);
      co_return response_value;
   }

   awaitable<response> do_tls_request(forge::net::http::request request_value, transport_deadline deadline,
                                      std::uint64_t max_response_body_bytes) {
      co_await ensure_tls_connected(deadline);

      ensure_host_header(request_value, endpoint);
      auto beast_request = to_beast_request(request_value);
      require_deadline(deadline);
      co_await boost::beast::http::async_write(*tls_stream, beast_request, use_awaitable);

      buffer.consume(buffer.size());
      auto parser = beast_http::response_parser<beast_http::string_body>{};
      parser.body_limit(max_response_body_bytes);
      if (request_value.method() == method::head) {
         parser.skip(true);
      }
      co_await beast_http::async_read(*tls_stream, buffer, parser, use_awaitable);
      auto response_value = to_http_response(parser.release());

      if (!response_value.keep_alive()) {
         close_tls();
      }
      record_status(response_value);
      co_return response_value;
   }

   awaitable<response> do_plain_streaming_request(forge::net::http::request request_value, body_reader body,
                                                  transport_deadline deadline, std::uint64_t max_response_body_bytes) {
      auto results = co_await resolve(deadline);
      auto stream = beast::tcp_stream{strand};
      stream.expires_at(deadline);
      co_await stream.async_connect(results, use_awaitable);

      ensure_host_header(request_value, endpoint);
      co_await write_streaming_request(stream, request_value, body, deadline);

      auto stream_buffer = beast::flat_buffer{};
      auto parser = beast_http::response_parser<beast_http::string_body>{};
      parser.body_limit(max_response_body_bytes);
      if (request_value.method() == method::head) {
         parser.skip(true);
      }
      require_deadline(deadline);
      stream.expires_at(deadline);
      co_await beast_http::async_read(stream, stream_buffer, parser, use_awaitable);
      auto response_value = to_http_response(parser.release());
      record_status(response_value);
      co_return response_value;
   }

   awaitable<response> do_tls_streaming_request(forge::net::http::request request_value, body_reader body,
                                                transport_deadline deadline, std::uint64_t max_response_body_bytes) {
      auto results = co_await resolve(deadline);
      auto& tls_context_provider = *tls_context_provider_ptr;
      auto context_snapshot = tls_context_provider.snapshot();
      require_tls_peer_verification(context_snapshot);
      auto stream = tls::make_beast_stream(context_snapshot, beast::tcp_stream{strand});
      configure_tls_client_stream(*stream, *context_snapshot);

      expire_at(*stream, deadline);
      co_await beast::get_lowest_layer(*stream).async_connect(results, use_awaitable);
      require_deadline(deadline);
      expire_at(*stream, deadline);
      try {
         co_await stream->async_handshake(asio::ssl::stream_base::client, use_awaitable);
      } catch (const boost::system::system_error&) {
         tls::classify_handshake_failure(stream->native_handle(), *context_snapshot);
         throw;
      }
      validate_tls_peer(*stream, *context_snapshot);

      ensure_host_header(request_value, endpoint);
      co_await write_streaming_request(*stream, request_value, body, deadline);

      auto stream_buffer = beast::flat_buffer{};
      auto parser = beast_http::response_parser<beast_http::string_body>{};
      parser.body_limit(max_response_body_bytes);
      if (request_value.method() == method::head) {
         parser.skip(true);
      }
      require_deadline(deadline);
      expire_at(*stream, deadline);
      co_await beast_http::async_read(*stream, stream_buffer, parser, use_awaitable);
      auto response_value = to_http_response(parser.release());
      record_status(response_value);
      co_return response_value;
   }

   awaitable<response_stream> do_plain_stream_request(forge::net::http::request request_value,
                                                      std::optional<body_reader> body, transport_deadline deadline,
                                                      std::uint64_t max_response_body_bytes) {
      auto results = co_await resolve(deadline);
      auto stream = beast::tcp_stream{strand};
      stream.expires_at(deadline);
      co_await stream.async_connect(results, use_awaitable);

      ensure_host_header(request_value, endpoint);
      if (body.has_value()) {
         co_await write_streaming_request(stream, request_value, *body, deadline);
      } else {
         auto beast_request = to_beast_request(request_value);
         require_deadline(deadline);
         stream.expires_at(deadline);
         co_await beast_http::async_write(stream, beast_request, use_awaitable);
      }

      auto stream_buffer = beast::flat_buffer{};
      auto parser = std::make_shared<beast_http::response_parser<beast_http::buffer_body>>();
      parser->body_limit(max_response_body_bytes);
      if (request_value.method() == method::head) {
         parser->skip(true);
      }
      require_deadline(deadline);
      stream.expires_at(deadline);
      co_await beast_http::async_read_header(stream, stream_buffer, *parser, use_awaitable);
      auto head = make_header_response(*parser);
      record_status(head);
      auto source = std::make_shared<beast_response_body_source<beast::tcp_stream>>(
          std::move(stream), std::move(stream_buffer), std::move(parser), deadline);
      co_return response_stream{.head = std::move(head), .body = body_reader{std::move(source)}};
   }

   awaitable<response_stream> do_tls_stream_request(forge::net::http::request request_value,
                                                    std::optional<body_reader> body, transport_deadline deadline,
                                                    std::uint64_t max_response_body_bytes) {
      auto results = co_await resolve(deadline);
      auto& tls_context_provider = *tls_context_provider_ptr;
      auto context_snapshot = tls_context_provider.snapshot();
      require_tls_peer_verification(context_snapshot);
      auto stream = tls::make_beast_stream(context_snapshot, beast::tcp_stream{strand});
      configure_tls_client_stream(*stream, *context_snapshot);

      expire_at(*stream, deadline);
      co_await beast::get_lowest_layer(*stream).async_connect(results, use_awaitable);
      require_deadline(deadline);
      expire_at(*stream, deadline);
      try {
         co_await stream->async_handshake(asio::ssl::stream_base::client, use_awaitable);
      } catch (const boost::system::system_error&) {
         tls::classify_handshake_failure(stream->native_handle(), *context_snapshot);
         throw;
      }
      validate_tls_peer(*stream, *context_snapshot);

      ensure_host_header(request_value, endpoint);
      if (body.has_value()) {
         co_await write_streaming_request(*stream, request_value, *body, deadline);
      } else {
         auto beast_request = to_beast_request(request_value);
         require_deadline(deadline);
         expire_at(*stream, deadline);
         co_await beast_http::async_write(*stream, beast_request, use_awaitable);
      }

      auto stream_buffer = beast::flat_buffer{};
      auto parser = std::make_shared<beast_http::response_parser<beast_http::buffer_body>>();
      parser->body_limit(max_response_body_bytes);
      if (request_value.method() == method::head) {
         parser->skip(true);
      }
      require_deadline(deadline);
      expire_at(*stream, deadline);
      co_await beast_http::async_read_header(*stream, stream_buffer, *parser, use_awaitable);
      auto head = make_header_response(*parser);
      record_status(head);
      auto source = std::make_shared<beast_response_body_source<beast::ssl_stream<beast::tcp_stream>>>(
          std::move(*stream), std::move(stream_buffer), std::move(parser), deadline, std::move(context_snapshot));
      co_return response_stream{.head = std::move(head), .body = body_reader{std::move(source)}};
   }

   awaitable<response> streaming_request(forge::net::http::request request_value, body_reader body,
                                         transport_deadline deadline, std::uint64_t max_response_body_bytes) {
      record_started();
      try {
         if (endpoint.secure()) {
            auto result = co_await do_tls_streaming_request(std::move(request_value), std::move(body), deadline,
                                                            max_response_body_bytes);
            record_completed();
            co_return result;
         }
         auto result = co_await do_plain_streaming_request(std::move(request_value), std::move(body), deadline,
                                                           max_response_body_bytes);
         record_completed();
         co_return result;
      } catch (const boost::system::system_error& error) {
         record_system_error(error.code());
         throw;
      } catch (const exceptions::gateway_timeout&) {
         record_system_error(asio::error::timed_out);
         throw;
      } catch (...) {
         record_failed();
         throw;
      }
   }

   awaitable<response_stream> stream_request(forge::net::http::request request_value, std::optional<body_reader> body,
                                             transport_deadline deadline, std::uint64_t max_response_body_bytes) {
      record_started();
      try {
         if (endpoint.secure()) {
            auto result = co_await do_tls_stream_request(std::move(request_value), std::move(body), deadline,
                                                         max_response_body_bytes);
            record_completed();
            co_return result;
         }
         auto result = co_await do_plain_stream_request(std::move(request_value), std::move(body), deadline,
                                                        max_response_body_bytes);
         record_completed();
         co_return result;
      } catch (const boost::system::system_error& error) {
         record_system_error(error.code());
         throw;
      } catch (const exceptions::gateway_timeout&) {
         record_system_error(asio::error::timed_out);
         throw;
      } catch (...) {
         record_failed();
         throw;
      }
   }

   awaitable<response_stream> retrying_stream_request(forge::net::http::request request_value,
                                                      transport_deadline deadline, request_options options) {
      const auto may_retry = options.retry_idempotent && is_idempotent(request_value.method());
      auto attempt = std::uint32_t{0};

      for (;;) {
         try {
            auto result =
                co_await stream_request(request_value, std::nullopt, deadline, options.max_response_body_bytes);
            if (attempt > 0) {
               record_reconnect();
            }
            co_return result;
         } catch (const boost::system::system_error& error) {
            if (!may_retry || attempt >= options.max_retries || !connection_reset_error(error.code())) {
               throw;
            }
         }
         ++attempt;
         record_retry();
         try {
            co_await sleep_for(options.retry_backoff, deadline);
         } catch (const exceptions::gateway_timeout&) {
            record_system_error(asio::error::timed_out);
            throw;
         } catch (const boost::system::system_error& error) {
            record_system_error(error.code());
            throw;
         }
      }
   }

   awaitable<void> process_request(std::shared_ptr<queued_request> operation) {
      try {
         const auto original = operation->request_value;
         const auto options = operation->options;
         const auto deadline = operation->deadline;
         const auto may_retry = options.retry_idempotent && is_idempotent(original.method());
         auto attempt = std::uint32_t{0};

         for (;;) {
            auto should_retry = false;
            try {
               require_deadline(deadline);
               record_started();
               if (endpoint.secure()) {
                  if (operation->complete_response(
                          co_await do_tls_request(original, deadline, options.max_response_body_bytes))) {
                     record_completed();
                  }
               } else {
                  if (operation->complete_response(
                          co_await do_plain_request(original, deadline, options.max_response_body_bytes))) {
                     record_completed();
                  }
               }
               break;
            } catch (const boost::system::system_error& error) {
               if (operation->is_completed()) {
                  close_all();
                  break;
               }

               const auto reset_connection = connection_reset_error(error.code()) || timeout_error(error.code()) ||
                                             cancellation_error(error.code()) ||
                                             error.code() == beast_http::error::body_limit;
               if (reset_connection) {
                  close_all();
               }
               record_system_error(error.code());
               if (may_retry && attempt < options.max_retries && connection_reset_error(error.code())) {
                  should_retry = true;
               } else if (timeout_error(error.code())) {
                  static_cast<void>(operation->complete_error(std::make_exception_ptr(
                      exceptions::gateway_timeout{"HTTP request exceeded its transport deadline"})));
               } else if (cancellation_error(error.code())) {
                  static_cast<void>(operation->complete_error(
                      std::make_exception_ptr(forge::asio::exceptions::canceled{"HTTP request was canceled"})));
               } else {
                  static_cast<void>(operation->complete_error(
                      std::make_exception_ptr(exceptions::unavailable{"HTTP transport request failed"})));
               }
            } catch (...) {
               close_all();
               record_failed();
               static_cast<void>(operation->complete_error(std::current_exception()));
               break;
            }

            if (should_retry) {
               ++attempt;
               record_retry();
               co_await sleep_for(options.retry_backoff, deadline);
               continue;
            }
            break;
         }
      } catch (const boost::system::system_error& error) {
         close_all();
         if (!operation->is_completed()) {
            record_system_error(error.code());
            if (timeout_error(error.code())) {
               static_cast<void>(operation->complete_error(std::make_exception_ptr(
                   exceptions::gateway_timeout{"HTTP request exceeded its transport deadline"})));
            } else if (cancellation_error(error.code())) {
               static_cast<void>(operation->complete_error(
                   std::make_exception_ptr(forge::asio::exceptions::canceled{"HTTP request was canceled"})));
            } else {
               static_cast<void>(operation->complete_error(
                   std::make_exception_ptr(exceptions::unavailable{"HTTP transport request failed"})));
            }
         }
      } catch (...) {
         close_all();
         if (!operation->is_completed()) {
            record_failed();
            static_cast<void>(operation->complete_error(std::current_exception()));
         }
      }

      active_request.reset();
      processing = false;
      start_next();
   }

   void enqueue(std::shared_ptr<queued_request> operation) {
      if (operation->is_completed()) {
         return;
      }
      requests.push_back(std::move(operation));
      record_queued();
      start_next();
   }

   void start_next() {
      if (processing || requests.empty()) {
         return;
      }

      processing = true;
      auto operation = requests.front();
      requests.pop_front();
      active_request = operation;

      asio::co_spawn(strand, process_request(operation),
                     asio::bind_cancellation_slot(operation->transport_cancellation.slot(),
                                                  [self = shared_from_this()](std::exception_ptr error) {
                                                     static_cast<void>(self);
                                                     if (error) {
                                                        try {
                                                           std::rethrow_exception(error);
                                                        } catch (const std::exception&) {
                                                        }
                                                     }
                                                  }));
   }

   void cancel_request(const std::shared_ptr<queued_request>& operation) {
      asio::dispatch(strand, [self = shared_from_this(), operation] { self->cancel_request_on_strand(operation); });
   }

   void expire_request(const std::shared_ptr<queued_request>& operation) {
      asio::dispatch(strand, [self = shared_from_this(), operation] { self->expire_request_on_strand(operation); });
   }

   void expire_request_on_strand(const std::shared_ptr<queued_request>& operation) {
      if (operation->is_completed()) {
         return;
      }
      const auto timeout =
          std::make_exception_ptr(exceptions::gateway_timeout{"HTTP request exceeded its transport deadline"});
      const auto queued = std::find(requests.begin(), requests.end(), operation);
      if (queued != requests.end()) {
         requests.erase(queued);
         static_cast<void>(operation->complete_error(timeout));
         record_system_error(asio::error::timed_out);
         return;
      }
      if (active_request == operation) {
         static_cast<void>(operation->complete_error(timeout));
         record_system_error(asio::error::timed_out);
         operation->transport_cancellation.emit(asio::cancellation_type::all);
         interrupt_active_io();
         return;
      }
      if (operation->complete_error(timeout)) {
         record_system_error(asio::error::timed_out);
      }
   }

   void cancel_request_on_strand(const std::shared_ptr<queued_request>& operation) {
      if (operation->is_completed()) {
         return;
      }
      const auto canceled = std::make_exception_ptr(forge::asio::exceptions::canceled{"HTTP request was canceled"});
      const auto queued = std::find(requests.begin(), requests.end(), operation);
      if (queued != requests.end()) {
         requests.erase(queued);
         static_cast<void>(operation->complete_error(canceled));
         record_canceled();
         return;
      }
      if (active_request == operation) {
         static_cast<void>(operation->complete_error(canceled));
         record_canceled();
         operation->transport_cancellation.emit(asio::cancellation_type::all);
         interrupt_active_io();
         return;
      }
      if (operation->complete_error(canceled)) {
         record_canceled();
      }
   }

   void shutdown() {
      asio::dispatch(strand, [self = shared_from_this()] { self->shutdown_on_strand(); });
   }

   void shutdown_on_strand() {
      const auto canceled = std::make_exception_ptr(forge::asio::exceptions::canceled{"HTTP connection was closed"});
      for (const auto& operation : requests) {
         if (operation->complete_error(canceled)) {
            record_canceled();
         }
      }
      requests.clear();
      if (active_request) {
         if (active_request->complete_error(canceled)) {
            record_canceled();
         }
         active_request->transport_cancellation.emit(asio::cancellation_type::all);
         interrupt_active_io();
      } else {
         close_all();
      }
   }

   void interrupt_active_io() {
      if (plain_stream) {
         auto ignored = boost::system::error_code{};
         beast::get_lowest_layer(*plain_stream).socket().cancel(ignored);
         beast::get_lowest_layer(*plain_stream).socket().set_option(asio::socket_base::linger{true, 0}, ignored);
         beast::get_lowest_layer(*plain_stream).socket().close(ignored);
         plain_connected = false;
      }
      if (tls_stream) {
         auto ignored = boost::system::error_code{};
         beast::get_lowest_layer(*tls_stream).socket().cancel(ignored);
         beast::get_lowest_layer(*tls_stream).socket().set_option(asio::socket_base::linger{true, 0}, ignored);
         beast::get_lowest_layer(*tls_stream).socket().close(ignored);
         tls_connected = false;
      }
   }

   void close_plain() {
      if (!plain_stream) {
         return;
      }

      auto ignored = boost::system::error_code{};
      beast::get_lowest_layer(*plain_stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
      beast::get_lowest_layer(*plain_stream).socket().close(ignored);
      plain_stream.reset();
      plain_connected = false;
   }

   void close_tls() {
      if (!tls_stream) {
         return;
      }

      auto ignored = boost::system::error_code{};
      beast::get_lowest_layer(*tls_stream).socket().shutdown(tcp::socket::shutdown_both, ignored);
      beast::get_lowest_layer(*tls_stream).socket().close(ignored);
      tls_stream.reset();
      tls_connected = false;
   }

   void close_all() {
      close_plain();
      close_tls();
   }

   awaitable<void> sleep_for(std::chrono::milliseconds delay, transport_deadline deadline) {
      if (delay.count() <= 0) {
         co_return;
      }
      require_deadline(deadline);
      auto timer = asio::steady_timer{strand};
      timer.expires_at(std::min(deadline, std::chrono::steady_clock::now() + delay));
      co_await timer.async_wait(use_awaitable);
      require_deadline(deadline);
   }

   void record_queued() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.queued_requests;
      current_metrics.queue_depth = requests.size() + (processing ? 1U : 0U);
   }

   void record_started() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.started_requests;
      current_metrics.queue_depth = requests.size() + 1U;
   }

   void record_completed() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.completed_requests;
      current_metrics.queue_depth = requests.size();
   }

   void record_failed() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.failed_requests;
      current_metrics.queue_depth = requests.size();
   }

   void record_retry() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.retry_attempts;
   }

   void record_reconnect() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.reconnects;
   }

   void record_system_error(const boost::system::error_code& error) {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.failed_requests;
      if (timeout_error(error)) {
         ++current_metrics.timeouts;
      }
      if (cancellation_error(error)) {
         ++current_metrics.cancellations;
      }
      current_metrics.queue_depth = requests.size();
   }

   void record_canceled() {
      const auto lock = std::scoped_lock{metrics_mutex};
      ++current_metrics.failed_requests;
      ++current_metrics.cancellations;
      current_metrics.queue_depth = requests.size() + (processing ? 1U : 0U);
   }

   void record_status(const response& response_value) {
      const auto value = response_value.result_int();
      const auto lock = std::scoped_lock{metrics_mutex};
      if (value < 200) {
         ++current_metrics.status_1xx;
      } else if (value < 300) {
         ++current_metrics.status_2xx;
      } else if (value < 400) {
         ++current_metrics.status_3xx;
      } else if (value < 500) {
         ++current_metrics.status_4xx;
      } else {
         ++current_metrics.status_5xx;
      }
   }

   [[nodiscard]] connection_metrics metrics() const {
      const auto lock = std::scoped_lock{metrics_mutex};
      auto snapshot = current_metrics;
      snapshot.queue_depth = requests.size() + (processing ? 1U : 0U);
      return snapshot;
   }

   forge::asio::runtime& runtime;
   base_url endpoint;
   asio::strand<asio::io_context::executor_type> strand;
   beast::flat_buffer buffer;
   std::shared_ptr<tls::context_provider> tls_context_provider_ptr;
   tls::client_stream_options tls_stream_options;
   tls::peer_validation tls_peer_validation;
   std::unique_ptr<beast::tcp_stream> plain_stream;
   std::shared_ptr<beast::ssl_stream<beast::tcp_stream>> tls_stream;
   bool plain_connected = false;
   bool tls_connected = false;
   bool plain_connected_once = false;
   bool tls_connected_once = false;
   bool processing = false;
   std::shared_ptr<queued_request> active_request;
   std::deque<std::shared_ptr<queued_request>> requests;
   mutable std::mutex metrics_mutex;
   connection_metrics current_metrics{};
};

connection::connection(forge::asio::runtime& runtime, base_url endpoint) try
    : connection(runtime, std::move(endpoint), {.tls_context_provider = make_https_client_context_provider()}) {
} catch (const forge::exceptions::base&) {
   throw;
} catch (const boost::system::system_error& error) {
   raise_transport_implementation_error(error);
} catch (const std::exception& error) {
   raise_transport_implementation_error(error);
} catch (...) {
   raise_transport_implementation_error();
}

connection::connection(forge::asio::runtime& runtime, base_url endpoint, connection_options options) try
    : impl_(std::make_shared<impl>(runtime, std::move(endpoint),
                                   options.tls_context_provider ? std::move(options.tls_context_provider)
                                                                : make_https_client_context_provider(),
                                   std::move(options.tls_stream_options), std::move(options.tls_peer_validation))) {
} catch (const forge::exceptions::base&) {
   throw;
} catch (const boost::system::system_error& error) {
   raise_transport_implementation_error(error);
} catch (const std::exception& error) {
   raise_transport_implementation_error(error);
} catch (...) {
   raise_transport_implementation_error();
}

connection::~connection() {
   if (impl_) {
      impl_->shutdown();
   }
}

boost::asio::awaitable<response> connection::async_request(forge::net::http::request request_value,
                                                           request_options options) {
   auto implementation = impl_;
   auto operation = std::shared_ptr<impl::queued_request>{};
   try {
      operation = std::make_shared<impl::queued_request>(implementation->runtime.context());
      operation->request_value = std::move(request_value);
      operation->options = options;
      operation->deadline = make_deadline(options);
      operation->completion_timer.expires_at(operation->deadline);

      co_await asio::this_coro::reset_cancellation_state(
          asio::enable_total_cancellation{},
          impl::caller_cancellation_filter{.owner = implementation, .operation = operation});
      const auto cancellation = co_await asio::this_coro::cancellation_state;

      asio::post(implementation->strand,
                 [implementation, operation]() mutable { implementation->enqueue(std::move(operation)); });

      if (cancellation.cancelled() != asio::cancellation_type::none) {
         implementation->cancel_request(operation);
      }

      while (!operation->is_completed()) {
         auto error = boost::system::error_code{};
         co_await operation->completion_timer.async_wait(boost::asio::redirect_error(use_awaitable, error));
         if (!error) {
            implementation->expire_request(operation);
            operation->completion_timer.expires_at(transport_deadline::max());
            continue;
         }
         if (error && error != asio::error::operation_aborted) {
            throw boost::system::system_error{error};
         }
      }
      co_return operation->take_result();
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const boost::system::system_error& error) {
      if (operation && !operation->is_completed()) {
         implementation->cancel_request(operation);
      }
      raise_transport_error(error);
   } catch (const std::exception& error) {
      if (operation && !operation->is_completed()) {
         implementation->cancel_request(operation);
      }
      raise_transport_implementation_error(error);
   } catch (...) {
      if (operation && !operation->is_completed()) {
         implementation->cancel_request(operation);
      }
      raise_transport_implementation_error();
   }
}

boost::asio::awaitable<response> connection::async_streaming_request(forge::net::http::request request_value,
                                                                     body_reader body, request_options options) try {
   auto implementation = impl_;
   const auto deadline = make_deadline(options);
   co_await asio::dispatch(implementation->strand, use_awaitable);
   co_return co_await implementation->streaming_request(std::move(request_value), std::move(body), deadline,
                                                        options.max_response_body_bytes);
} catch (const forge::exceptions::base&) {
   throw;
} catch (const boost::system::system_error& error) {
   raise_transport_error(error);
} catch (const std::exception& error) {
   raise_transport_implementation_error(error);
} catch (...) {
   raise_transport_implementation_error();
}

boost::asio::awaitable<response_stream> connection::async_stream_request(forge::net::http::request request_value,
                                                                         request_options options) try {
   auto implementation = impl_;
   const auto deadline = make_deadline(options);
   co_await asio::dispatch(implementation->strand, use_awaitable);
   co_return co_await implementation->retrying_stream_request(std::move(request_value), deadline, options);
} catch (const forge::exceptions::base&) {
   throw;
} catch (const boost::system::system_error& error) {
   raise_transport_error(error);
} catch (const std::exception& error) {
   raise_transport_implementation_error(error);
} catch (...) {
   raise_transport_implementation_error();
}

boost::asio::awaitable<response_stream> connection::async_stream_request(forge::net::http::request request_value,
                                                                         body_reader body,
                                                                         request_options options) try {
   auto implementation = impl_;
   const auto deadline = make_deadline(options);
   co_await asio::dispatch(implementation->strand, use_awaitable);
   co_return co_await implementation->stream_request(std::move(request_value), std::move(body), deadline,
                                                     options.max_response_body_bytes);
} catch (const forge::exceptions::base&) {
   throw;
} catch (const boost::system::system_error& error) {
   raise_transport_error(error);
} catch (const std::exception& error) {
   raise_transport_implementation_error(error);
} catch (...) {
   raise_transport_implementation_error();
}

connection_metrics connection::metrics() const {
   return impl_->metrics();
}

} // namespace forge::net::http
