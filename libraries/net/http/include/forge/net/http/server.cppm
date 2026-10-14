module;

#include <cstdint>
#include <chrono>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/awaitable.hpp>

export module forge.net.http.server;

import forge.asio.runtime;
import forge.net.http.middleware;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.tls.context;
import forge.net.http.types;

export namespace forge::net::http {

struct server_config {
   std::string bind_address = "127.0.0.1";
   std::uint16_t port = 0;
   std::uint64_t max_request_body_bytes = 16 * 1024 * 1024;
   std::uint64_t max_header_bytes = 64 * 1024;
   std::chrono::milliseconds read_timeout{30'000};
   // Bounds socket I/O and keep-alive gaps; handlers and body producers own their deadlines.
   std::chrono::milliseconds idle_timeout{120'000};
   std::shared_ptr<forge::net::tls::context_provider> tls_context_provider;
   std::chrono::milliseconds handshake_timeout{10'000};
   std::uint64_t max_pending_tls_handshakes = 64;
   // Requires DEBUG on forge.net.http.server; never includes request/response content.
   bool access_log = false;
};

using server_handler = std::function<boost::asio::awaitable<response>(route_context&)>;

class server {
 public:
   server(forge::asio::runtime& runtime, server_config config, server_handler handler);
   server(forge::asio::runtime& runtime, server_config config, router router_value);
   ~server();

   server(const server&) = delete;
   server& operator=(const server&) = delete;

   void start();
   void stop();
   boost::asio::awaitable<void> async_start();
   boost::asio::awaitable<void> async_stop();

   [[nodiscard]] std::uint16_t port() const;

 private:
   struct impl;

   std::shared_ptr<impl> impl_;
};

} // namespace forge::net::http
