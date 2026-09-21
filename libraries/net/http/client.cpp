module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <coroutine>
#include <exception>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/beast/http.hpp>

module forge.net.http.client;

import forge.net.http.exceptions;

namespace forge::net::http {
namespace {

[[nodiscard]] tls::context_options make_default_tls_options() {
   auto options = tls::context_options{};
   options.protocols = tls::protocol_policy::system_default;
   return options;
}

[[nodiscard]] std::shared_ptr<tls::context_provider> resolve_tls_context(const base_url& endpoint,
                                                                         const client_options& options) {
   auto provider = options.tls_context_provider;
   if (!provider) {
      provider = std::make_shared<tls::context_provider>(make_default_tls_options());
   }
   const auto snapshot = provider->snapshot();
   if (endpoint.secure() && (!snapshot || snapshot->verification() != tls::peer_verification::verify_peer)) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTPS client requires TLS peer verification");
   }
   return provider;
}

[[nodiscard]] tls::client_stream_options make_tls_stream_options(const base_url& endpoint,
                                                                 const client_options& options) {
   const auto hostname = options.hostname.empty() ? endpoint.host : options.hostname;
   if (hostname.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTPS client hostname must not be empty");
   }
   if (options.server_name.empty()) {
      return {.sni = tls::sni_policy::endpoint_host, .endpoint_host = hostname};
   }
   return {.sni = tls::sni_policy::explicit_name, .endpoint_host = hostname, .server_name = options.server_name};
}

[[nodiscard]] tls::peer_validation make_tls_peer_validation(const base_url& endpoint, const client_options& options) {
   const auto hostname = options.hostname.empty() ? endpoint.host : options.hostname;
   if (hostname.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTPS client hostname must not be empty");
   }
   return {.expected_host = hostname, .expected_sha256_fingerprint = options.expected_sha256_fingerprint};
}

[[nodiscard]] bool is_token_character(unsigned char value) noexcept {
   return std::isalnum(value) != 0 || value == '!' || value == '#' || value == '$' || value == '%' || value == '&' ||
          value == '\'' || value == '*' || value == '+' || value == '-' || value == '.' || value == '^' ||
          value == '_' || value == '`' || value == '|' || value == '~';
}

[[nodiscard]] bool is_protected_header(std::string_view name) noexcept {
   return header_name_equal(name, "Host") || header_name_equal(name, "Content-Length") ||
          header_name_equal(name, "Transfer-Encoding") || header_name_equal(name, "Trailer") ||
          header_name_equal(name, "Connection") || header_name_equal(name, "Keep-Alive") ||
          header_name_equal(name, "Proxy-Authenticate") || header_name_equal(name, "Proxy-Authorization") ||
          header_name_equal(name, "Proxy-Connection") || header_name_equal(name, "TE") ||
          header_name_equal(name, "Upgrade");
}

void validate_provider_header(const header_entry& header) {
   if (header.name.empty() ||
       !std::ranges::all_of(header.name, [](unsigned char value) { return is_token_character(value); })) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP header provider returned an invalid header name");
   }
   if (std::ranges::any_of(header.text, [](unsigned char value) { return value < 0x20U || value == 0x7fU; })) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP header provider returned an invalid header value");
   }
   if (is_protected_header(header.name)) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request,
                            "HTTP header provider cannot modify Host, framing, or hop-by-hop headers");
   }
}

[[nodiscard]] bool has_header_named(std::span<const header_entry> headers, std::string_view name) {
   return std::ranges::any_of(headers,
                              [name](const header_entry& header) { return header_name_equal(header.name, name); });
}

[[nodiscard]] request_options remaining_request_options(request_options options,
                                                        std::chrono::steady_clock::time_point started) {
   const auto elapsed =
       std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
   if (elapsed >= options.timeout) {
      throw exceptions::gateway_timeout{"HTTP header provider exceeded the request deadline"};
   }
   options.timeout -= elapsed;
   return options;
}

request make_request(method method_value, const base_url& endpoint, std::string_view path, std::string body = {},
                     std::string_view content_type = {}) {
   auto request_value = request{};
   request_value.method(method_value);
   request_value.target(endpoint.make_target(path));
   request_value.version(11);
   request_value.body() = std::move(body);
   if (!request_value.body().empty()) {
      request_value.set(field::content_type,
                        content_type.empty() ? std::string_view{"application/octet-stream"} : content_type);
      request_value.prepare_payload();
   }
   return request_value;
}

} // namespace

void validate_provider_headers(std::span<const header_entry> headers, std::span<const header_entry> existing_headers,
                               std::size_t max_headers, std::size_t max_header_bytes) {
   if (max_headers == 0U || max_headers > max_client_provider_headers || max_header_bytes == 0U ||
       max_header_bytes > max_client_provider_header_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP header provider limits are outside the supported bounds");
   }
   if (headers.size() > max_headers) {
      FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large,
                            "HTTP header provider exceeded its configured header count");
   }

   auto provider_header_names = std::vector<std::string_view>{};
   provider_header_names.reserve(headers.size());
   auto bytes = std::size_t{};
   for (const auto& header : headers) {
      validate_provider_header(header);
      if (has_header_named(existing_headers, header.name) ||
          std::ranges::any_of(provider_header_names,
                              [&header](std::string_view name) { return header_name_equal(name, header.name); })) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request,
                               "HTTP header provider returned a header that collides with an existing header");
      }
      if (header.name.size() > max_header_bytes - bytes ||
          header.text.size() > max_header_bytes - bytes - header.name.size()) {
         FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large,
                               "HTTP header provider exceeded its configured header byte limit");
      }
      bytes += header.name.size() + header.text.size();
      provider_header_names.push_back(header.name);
   }
}

client::client(forge::asio::runtime& runtime, base_url endpoint, client_options options) try
    : endpoint_(std::move(endpoint)), options_(std::move(options)),
      connection_(runtime, endpoint_,
                  {.tls_context_provider = resolve_tls_context(endpoint_, options_),
                   .tls_stream_options = make_tls_stream_options(endpoint_, options_),
                   .tls_peer_validation = make_tls_peer_validation(endpoint_, options_)}) {
   if (options_.max_provider_headers > max_client_provider_headers ||
       options_.max_provider_header_bytes > max_client_provider_header_bytes || options_.max_provider_headers == 0U ||
       options_.max_provider_header_bytes == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP header provider limits are outside the supported bounds");
   }
} catch (const forge::exceptions::base&) {
   throw;
} catch (const std::exception& error) {
   FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP client initialization failed",
                         forge::exceptions::ctx("reason", error.what()));
} catch (...) {
   FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP client initialization failed");
}

client::~client() = default;

boost::asio::awaitable<response> client::async_request(forge::net::http::request request_value,
                                                       request_options options) {
   const auto started = std::chrono::steady_clock::now();
   auto prepared = apply_provider_headers(std::move(request_value));
   if (options_.header_provider) {
      options = remaining_request_options(options, started);
   }
   co_return co_await connection_.async_request(std::move(prepared), options);
}

boost::asio::awaitable<response> client::async_streaming_request(forge::net::http::request request_value,
                                                                 body_reader body, request_options options) {
   const auto started = std::chrono::steady_clock::now();
   auto prepared = apply_provider_headers(std::move(request_value));
   if (options_.header_provider) {
      options = remaining_request_options(options, started);
   }
   co_return co_await connection_.async_streaming_request(std::move(prepared), std::move(body), options);
}

boost::asio::awaitable<response_stream> client::async_stream_request(forge::net::http::request request_value,
                                                                     request_options options) {
   const auto started = std::chrono::steady_clock::now();
   auto prepared = apply_provider_headers(std::move(request_value));
   if (options_.header_provider) {
      options = remaining_request_options(options, started);
   }
   co_return co_await connection_.async_stream_request(std::move(prepared), options);
}

boost::asio::awaitable<response_stream> client::async_stream_request(forge::net::http::request request_value,
                                                                     body_reader body, request_options options) {
   const auto started = std::chrono::steady_clock::now();
   auto prepared = apply_provider_headers(std::move(request_value));
   if (options_.header_provider) {
      options = remaining_request_options(options, started);
   }
   co_return co_await connection_.async_stream_request(std::move(prepared), std::move(body), options);
}

boost::asio::awaitable<response> client::async_send(method verb, std::string_view path, std::string body,
                                                    std::string_view content_type, request_options options) {
   co_return co_await async_request(make_request(verb, endpoint_, path, std::move(body), content_type), options);
}

boost::asio::awaitable<response> client::async_get(std::string_view path, request_options options) {
   co_return co_await async_request(make_request(method::get, endpoint_, path), options);
}

boost::asio::awaitable<response> client::async_post_json(std::string_view path, std::string body,
                                                         request_options options) {
   co_return co_await async_request(make_request(method::post, endpoint_, path, std::move(body), "application/json"),
                                    options);
}

std::string client::make_target(std::string_view path) const {
   return endpoint_.make_target(path);
}

connection_metrics client::metrics() const {
   return connection_.metrics();
}

request client::apply_provider_headers(request request_value) const {
   if (!options_.header_provider) {
      return request_value;
   }
   try {
      auto headers = options_.header_provider(request_value);
      const auto existing_headers = request_value.headers();
      validate_provider_headers(headers, existing_headers, options_.max_provider_headers,
                                options_.max_provider_header_bytes);
      for (const auto& header : headers) {
         request_value.insert(header.name, header.text);
      }
      return request_value;
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP header provider failed",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "HTTP header provider failed");
   }
}

} // namespace forge::net::http
