module;

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/asio/awaitable.hpp>

export module forge.net.http.client;

import forge.asio.runtime;
import forge.net.http.base_url;
import forge.net.http.body;
import forge.net.http.connection;
import forge.net.http.types;
import forge.net.tls.context;

export namespace forge::net::http {

inline constexpr std::size_t max_client_provider_headers = 64U;
inline constexpr std::size_t max_client_provider_header_bytes = 64U * 1024U;

using request_header_provider = std::function<std::vector<header_entry>(const request&)>;

struct client_options {
   std::shared_ptr<tls::context_provider> tls_context_provider;
   std::string hostname;
   std::string server_name;
   std::optional<std::string> expected_sha256_fingerprint;
   // Invoked synchronously before a request is queued. Implementations must be bounded, nonblocking, and thread-safe.
   // The callback has no cancellation channel; request cancellation is observed after it returns.
   request_header_provider header_provider;
   std::size_t max_provider_headers = 16U;
   std::size_t max_provider_header_bytes = 8U * 1024U;
};

void validate_provider_headers(std::span<const header_entry> headers,
                               std::span<const header_entry> existing_headers = {}, std::size_t max_headers = 16U,
                               std::size_t max_header_bytes = 8U * 1024U);

class client {
 public:
   client(forge::asio::runtime& runtime, base_url endpoint, client_options options = {});
   ~client();

   client(const client&) = delete;
   client& operator=(const client&) = delete;

   boost::asio::awaitable<response> async_request(forge::net::http::request request_value,
                                                  request_options options = {});
   boost::asio::awaitable<response> async_streaming_request(forge::net::http::request request_value, body_reader body,
                                                            request_options options = {});
   boost::asio::awaitable<response_stream> async_stream_request(forge::net::http::request request_value,
                                                                request_options options = {});
   boost::asio::awaitable<response_stream> async_stream_request(forge::net::http::request request_value,
                                                                body_reader body, request_options options = {});
   boost::asio::awaitable<response> async_send(method verb, std::string_view path, std::string body = {},
                                               std::string_view content_type = "application/octet-stream",
                                               request_options options = {});
   boost::asio::awaitable<response> async_get(std::string_view path, request_options options = {});
   boost::asio::awaitable<response> async_post_json(std::string_view path, std::string body,
                                                    request_options options = {});
   [[nodiscard]] std::string make_target(std::string_view path) const;
   [[nodiscard]] connection_metrics metrics() const;

 private:
   [[nodiscard]] request apply_provider_headers(request request_value) const;

   base_url endpoint_;
   client_options options_;
   connection connection_;
};

} // namespace forge::net::http
