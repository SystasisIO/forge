module;

#include <chrono>
#include <memory>
#include <cstdint>
#include <optional>
#include <string>

#include <boost/asio/awaitable.hpp>

export module forge.net.http.connection;

import forge.asio.runtime;
import forge.net.http.base_url;
import forge.net.http.body;
import forge.net.http.types;
import forge.net.tls.context;

export namespace forge::net::http {

struct request_options {
   std::chrono::milliseconds timeout{30'000};
   bool retry_idempotent = false;
   std::uint32_t max_retries = 0;
   std::chrono::milliseconds retry_backoff{50};
   // If set, transport uses the earlier of this absolute deadline and timeout.
   std::optional<std::chrono::steady_clock::time_point> deadline;
   // Bounds response payload bytes for buffered and streamed reads; zero permits only an empty body.
   std::uint64_t max_response_body_bytes = 8ULL * 1024 * 1024;
};

struct connection_metrics {
   std::uint64_t queued_requests = 0;
   std::uint64_t started_requests = 0;
   std::uint64_t completed_requests = 0;
   std::uint64_t failed_requests = 0;
   std::uint64_t retry_attempts = 0;
   std::uint64_t reconnects = 0;
   std::uint64_t timeouts = 0;
   std::uint64_t cancellations = 0;
   std::uint64_t status_1xx = 0;
   std::uint64_t status_2xx = 0;
   std::uint64_t status_3xx = 0;
   std::uint64_t status_4xx = 0;
   std::uint64_t status_5xx = 0;
   std::size_t queue_depth = 0;
};

struct response_stream {
   response head;
   body_reader body;
};

struct connection_options {
   std::shared_ptr<tls::context_provider> tls_context_provider;
   tls::client_stream_options tls_stream_options;
   tls::peer_validation tls_peer_validation;
};

class connection {
 public:
   connection(forge::asio::runtime& runtime, base_url endpoint);
   connection(forge::asio::runtime& runtime, base_url endpoint, connection_options options);
   ~connection();

   connection(const connection&) = delete;
   connection& operator=(const connection&) = delete;

   boost::asio::awaitable<response> async_request(forge::net::http::request request_value,
                                                  request_options options = {});
   boost::asio::awaitable<response> async_streaming_request(forge::net::http::request request_value, body_reader body,
                                                            request_options options = {});
   boost::asio::awaitable<response_stream> async_stream_request(forge::net::http::request request_value,
                                                                request_options options = {});
   boost::asio::awaitable<response_stream> async_stream_request(forge::net::http::request request_value,
                                                                body_reader body, request_options options = {});
   [[nodiscard]] connection_metrics metrics() const;

 private:
   struct impl;

   std::shared_ptr<impl> impl_;
};

} // namespace forge::net::http
