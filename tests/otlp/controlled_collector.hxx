#pragma once

#include <boost/asio/awaitable.hpp>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

import forge.asio.runtime;
import forge.net.http.route_context;
import forge.net.http.server;
import forge.net.http.types;

namespace otlp_concurrency_fixtures {

class controlled_collector {
 public:
   explicit controlled_collector(forge::asio::runtime& runtime,
                                  forge::net::http::status status = forge::net::http::status::ok);
   ~controlled_collector();

   [[nodiscard]] std::string endpoint() const;
   [[nodiscard]] bool wait_for_requests(std::size_t count, std::chrono::milliseconds timeout = std::chrono::seconds{3}) const;
   [[nodiscard]] std::vector<std::string> bodies() const;
   void release() noexcept;

 private:
   struct state;
   static boost::asio::awaitable<forge::net::http::response> handle(
       std::shared_ptr<state> owner, forge::net::http::route_context& context);

   std::shared_ptr<state> state_;
   forge::net::http::server server_;
};

} // namespace otlp_concurrency_fixtures
