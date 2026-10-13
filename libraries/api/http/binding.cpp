module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

module forge.api.http.binding;

import forge.api.core.binding;
import forge.api.core.exceptions;
import forge.api.core.stream_reader;
import forge.api.core.types;
import forge.api.auth.authenticated_subject;
import forge.api.http.client_response;
import forge.net.http.body;
import forge.net.http.client;
import forge.net.http.connection;
import forge.net.http.exceptions;
import forge.net.http.negotiation;
import forge.net.http.stream;
import forge.net.http.types;
import forge.raw.raw;

#include "details/stream_frame_decoder.hxx"
#include "details/body_stream_endpoint.hxx"
#include "details/endpoint_body_source.hxx"
#include "details/server_stream_state.hxx"
#include "details/invocation_body_source.hxx"

namespace forge::api::http::detail {
namespace {

constexpr auto stream_content_type = std::string_view{"application/vnd.forge.api-stream; version=2"};
constexpr auto stream_media_type = std::string_view{"application/vnd.forge.api-stream"};
constexpr auto stream_limits = forge::api::core::session_limits{};

[[nodiscard]] bool request_content_type_matches(const forge::net::http::request& request) {
   const auto found = request.find(forge::net::http::field::content_type);
   if (found == request.end()) {
      return false;
   }
   const auto accepted = std::array{
      forge::net::http::media_type_match{.type = stream_media_type, .structured_suffix = {}},
   };
   return forge::net::http::media_type_matches(found->value(), accepted);
}

[[nodiscard]] bool response_content_type_matches(const forge::net::http::response& response) {
   const auto found = response.find(forge::net::http::field::content_type);
   if (found == response.end()) {
      return false;
   }
   const auto accepted = std::array{
      forge::net::http::media_type_match{.type = stream_media_type, .structured_suffix = {}},
   };
   return forge::net::http::media_type_matches(found->value(), accepted);
}

[[nodiscard]] forge::api::core::response
make_core_response(const forge::api::core::request& request,
                   forge::api::core::frame terminal) {
   auto output = forge::api::core::response{
      .api = request.api,
      .method = request.method,
      .codec = request.codec,
   };
   if (terminal.id.value != 1 ||
       (terminal.kind != forge::api::core::frame_kind::response &&
        terminal.kind != forge::api::core::frame_kind::error)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "HTTP API stream returned an invalid terminal frame");
   }
   if (terminal.kind == forge::api::core::frame_kind::error) {
      output.error = forge::raw::unpack_exact<forge::api::core::error_payload>(terminal.payload);
   } else {
      output.body = std::move(terminal.payload);
   }
   return output;
}

[[nodiscard]] bool response_accepts_stream(const forge::net::http::request& request) {
   auto value = std::string{};
   for (const auto& header : request.headers()) {
      if (forge::net::http::header_name_equal(
             header.name, forge::net::http::field_name(forge::net::http::field::accept))) {
         if (!value.empty()) {
            value += ", ";
         }
         value += header.text;
      }
   }
   if (value.empty()) {
      return true;
   }
   const auto accepted = std::array{
      forge::net::http::media_type_match{.type = stream_media_type, .structured_suffix = {}},
   };
   return forge::net::http::accept_allows(value, accepted);
}

} // namespace

forge::api::core::trusted_invocation trusted_invocation_for(const forge::net::http::route_context& context) {
   if (!context.client_certificate_fingerprint) {
      return {};
   }
   return forge::api::core::trusted_invocation_builder{}
       .set(forge::api::auth::authenticated_caller{
           forge::api::auth::caller_source::tls_certificate,
           *context.client_certificate_fingerprint,
       })
       .build();
}

boost::asio::awaitable<forge::api::core::invocation_context>
provide_invocation(invocation_provider provider, const route_context& context,
                   const forge::api::core::method_descriptor& method) {
   auto admitted = provider ? co_await provider(context, method) : forge::api::core::invocation_context{};
   const auto cancellation = co_await boost::asio::this_coro::cancellation_state;
   if (cancellation.cancelled() != boost::asio::cancellation_type::none) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::cancelled, "HTTP admission was cancelled");
   }
   admitted.trusted = forge::api::core::trusted_invocation_builder{}
                         .merge(trusted_invocation_for(context)).merge(admitted.trusted).build();
   if (const auto* subject = admitted.trusted.find<forge::api::auth::authenticated_subject>()) {
      if (subject->token() == nullptr) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "HTTP subject is unavailable");
      }
      const auto until = subject->token()->valid_until();
      if (!admitted.lifetime.expires_at || until < *admitted.lifetime.expires_at) {
         admitted.lifetime.expires_at = until;
      }
   }
   admitted.lifetime.bind_expiry();
   co_return admitted;
}

void validate_live_stream_headers(const forge::net::http::request& request,
                                  forge::api::core::method_kind kind) {
   if (kind == forge::api::core::method_kind::bidirectional_stream) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                            "HTTP/1.1 does not support bidirectional API streams");
   }
   if (!response_accepts_stream(request)) {
      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::not_acceptable,
                            "HTTP API live stream requires an acceptable v2 stream media type");
   }
   if (kind == forge::api::core::method_kind::client_stream &&
       !request_content_type_matches(request)) {
      FORGE_THROW_EXCEPTION(forge::net::http::exceptions::unsupported_media_type,
                            "HTTP API client stream requires the v2 stream media type");
   }
}

boost::asio::awaitable<forge::net::http::stream_response>
make_live_server_stream_response(forge::api::core::pinned_binding_plan plan, forge::api::core::frame request,
                                 forge::net::http::stream_request& http_request,
                                 forge::net::http::status success_status,
                                 forge::api::core::trusted_invocation trusted,
                                 forge::api::core::invocation_lifetime lifetime) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto state = std::make_shared<server_stream_state>(
       executor, std::move(plan), std::move(request), std::move(trusted), stream_limits.max_frame_bytes,
       stream_limits.max_item_bytes, stream_limits.initial_window_items, stream_limits.initial_window_bytes,
       std::move(lifetime));
   auto body_lifetime = std::shared_ptr<void>{
      state.get(),
      [state](void*) noexcept { state->cancel(); }};

   auto head = forge::net::http::response{success_status, http_request.context.request.version()};
   head.keep_alive(http_request.context.request.keep_alive());
   head.set(forge::net::http::field::content_type, std::string{stream_content_type});
   co_return forge::net::http::stream_response{
      .head = std::move(head),
      .body = http_request.response_body(
         [lifetime = std::move(body_lifetime)]() -> boost::asio::awaitable<
            std::optional<forge::net::http::body_chunk>> {
            auto* state = static_cast<server_stream_state*>(lifetime.get());
            co_return co_await state->async_next();
         }),
      .send_guard = [state](boost::asio::awaitable<void> operation, std::function<void()>)
         -> boost::asio::awaitable<void> {
         auto error = std::exception_ptr{};
         try {
            state->start();
            co_await std::move(operation);
         } catch (...) {
            error = std::current_exception();
         }
         // Disable cancellation before the co_await: Asio can reject it before async_drain starts.
         co_await boost::asio::this_coro::reset_cancellation_state(boost::asio::disable_cancellation{});
         co_await state->async_drain();
         if (error) {
            std::rethrow_exception(error);
         }
      },
   };
}

boost::asio::awaitable<forge::api::core::frame>
dispatch_live_client_stream(forge::api::core::pinned_binding_plan plan, forge::api::core::frame request,
                            forge::net::http::body_reader body,
                            std::function<void(const forge::api::core::bytes&, forge::raw::unpack_limits)> decoder,
                            forge::api::core::trusted_invocation trusted,
                            forge::api::core::invocation_lifetime lifetime) {
   auto input = std::make_shared<body_stream_endpoint>(
      std::move(body), forge::api::core::stream_direction::input, std::move(decoder),
      stream_limits.max_frame_bytes, stream_limits.max_item_bytes, false);
   auto admitted = forge::api::core::invocation_context{trusted, std::move(lifetime)};
   co_return co_await forge::api::core::enforce_invocation(
      plan.dispatch_stream_contextual(std::move(request), input, {}, std::move(trusted)), std::move(admitted),
      [input](std::exception_ptr error) { input->fail(std::move(error)); });
}

stream_response protect_response_body(stream_response response, forge::api::core::invocation_context context,
                                       body_reader request_body) {
   context.lifetime.bind_expiry();
   if (context.lifetime.active()) {
      auto state = std::make_shared<invocation_body_source>(response.body, std::move(context),
         [body = std::move(request_body)]() mutable { body.cancel(); });
      if (response.body) {
         response.body = response.body.with_callback([state]()
            -> boost::asio::awaitable<std::optional<body_chunk>> { co_return co_await state->async_read(); });
      }
      response.send_guard = [state, inner = std::move(response.send_guard)](
         boost::asio::awaitable<void> operation, std::function<void()> abort) -> boost::asio::awaitable<void> {
         if (inner) {
            operation = inner(std::move(operation), abort);
         }
         co_await state->async_send(std::move(operation), std::move(abort));
      };
   }
   return response;
}

forge::net::http::response
make_live_terminal_response(const forge::net::http::request& request,
                            forge::net::http::status success_status,
                            const forge::api::core::frame& terminal) {
   if (terminal.kind != forge::api::core::frame_kind::response || terminal.id.value != 1) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "HTTP API client stream produced an invalid terminal response");
   }
   auto chunk = stream_frame_decoder::encode(terminal, stream_limits.max_frame_bytes);
   auto body = std::string{};
   body.resize(chunk.bytes.size());
   if (!chunk.bytes.empty()) {
      std::memcpy(body.data(), chunk.bytes.data(), chunk.bytes.size());
   }
   auto output = forge::net::http::response{success_status, request.version()};
   output.keep_alive(request.keep_alive());
   output.set(forge::net::http::field::content_type, std::string{stream_content_type});
   output.body() = std::move(body);
   output.prepare_payload();
   return output;
}

boost::asio::awaitable<forge::api::core::response>
invoke_live_http_stream(
   forge::net::http::client& client,
   const forge::api::core::descriptor& descriptor,
   const route& route,
   forge::api::core::request request,
   forge::net::http::request http_request,
   forge::net::http::request_options request_options,
   forge::api::core::method_kind kind,
   std::shared_ptr<forge::api::core::detail::stream_endpoint> input,
   std::shared_ptr<forge::api::core::detail::stream_endpoint> output) {
   if (kind == forge::api::core::method_kind::bidirectional_stream) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::incompatible_version,
                            "HTTP/1.1 does not support bidirectional API streams");
   }
   const auto* method = forge::api::core::find_method(descriptor, request.method);
   if (method == nullptr || method->kind != kind) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::method_not_found,
                            "HTTP API stream method is not declared");
   }

   http_request.set(forge::net::http::field::accept, std::string{stream_media_type});
   if (kind == forge::api::core::method_kind::server_stream) {
      if (!output) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "HTTP server stream requires an output endpoint");
      }
      auto response = co_await client.async_stream_request(std::move(http_request), request_options);
      if (response.head.result_int() < 200U || response.head.result_int() >= 300U) {
         response.head.body() = co_await read_bounded_error_body(response.body);
         auto error = decode_error_payload(response.head, route.error_body_codec);
         forge::api::core::raise_remote_error(error, method);
      }
      if (!response_content_type_matches(response.head)) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                               "HTTP API stream response has an incompatible media type");
      }

      auto inbound = std::make_shared<body_stream_endpoint>(
         std::move(response.body), forge::api::core::stream_direction::output,
         method->output_decoder, stream_limits.max_frame_bytes,
         stream_limits.max_item_bytes, true);
      output->set_failure_observer(
         [weak = std::weak_ptr<body_stream_endpoint>{inbound}] {
            if (auto locked = weak.lock()) {
               locked->fail(std::make_exception_ptr(
                  forge::api::core::exceptions::cancelled{
                     "HTTP API response stream was cancelled"}));
            }
         });
      while (auto item = co_await inbound->async_read()) {
         co_await output->async_write(std::move(*item));
      }
      output->close();
      co_return make_core_response(request, co_await inbound->async_finish());
   }

   if (!input) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "HTTP client stream requires an input endpoint");
   }
   http_request.set(forge::net::http::field::content_type, std::string{stream_content_type});
   auto source = std::make_shared<endpoint_body_source>(
      std::move(input), forge::api::core::stream_direction::input, request.api,
      request.method, request.codec, stream_limits.max_frame_bytes,
      stream_limits.max_item_bytes);
   auto response = co_await client.async_stream_request(
      std::move(http_request), forge::net::http::body_reader{std::move(source)}, request_options);
   if (response.head.result_int() < 200U || response.head.result_int() >= 300U) {
      response.head.body() = co_await read_bounded_error_body(response.body);
      auto error = decode_error_payload(response.head, route.error_body_codec);
      forge::api::core::raise_remote_error(error, method);
   }
   if (!response_content_type_matches(response.head)) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "HTTP API stream response has an incompatible media type");
   }

   auto decoder = stream_frame_decoder{std::move(response.body), stream_limits.max_frame_bytes};
   auto terminal = co_await decoder.async_read();
   if (!terminal) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "HTTP API client stream response has no terminal frame");
   }
   if (co_await decoder.async_read()) {
      FORGE_THROW_EXCEPTION(forge::api::core::exceptions::protocol_error,
                            "HTTP API client stream response has trailing frames");
   }
   co_return make_core_response(request, std::move(*terminal));
}

} // namespace forge::api::http::detail
