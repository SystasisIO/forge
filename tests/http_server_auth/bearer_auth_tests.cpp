#include <boost/asio/awaitable.hpp>
#include <boost/test/unit_test.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.http.types;
import forge.plugins.net.http.server.bearer_auth;
import forge.plugins.net.http.server.exceptions;

namespace {

namespace http = forge::plugins::net::http::server;

http::middleware_response invoke(const http::middleware_descriptor& middleware, std::string authorization,
                                 bool& called) {
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 1U}};
   auto headers = std::vector<http::header_entry>{};
   if (!authorization.empty()) {
      headers.push_back({.name = "Authorization", .value = std::move(authorization)});
   }
   auto request = http::middleware_request{.method = "GET", .path = "/v1/chain/admin", .headers = std::move(headers)};
   return forge::asio::blocking::run(
       runtime, middleware.handler(request, [&called]() -> boost::asio::awaitable<http::middleware_response> {
          called = true;
          co_return http::middleware_response::text(forge::net::http::status::ok, "accepted");
       }));
}

} // namespace

BOOST_AUTO_TEST_CASE(bearer_auth_accepts_only_configured_token_hashes) {
   const auto middleware = http::bearer_auth({
       .path_prefix = "/v1/chain/admin",
       .token_hashes = {http::hash_bearer_token("correct-horse-battery-staple")},
   });
   BOOST_TEST(static_cast<int>(middleware.phase) == static_cast<int>(http::middleware_phase::security));

   auto called = false;
   auto response = invoke(middleware, "Bearer correct-horse-battery-staple", called);
   BOOST_TEST(called);
   BOOST_TEST(static_cast<int>(response.status()) == static_cast<int>(forge::net::http::status::ok));

   called = false;
   response = invoke(middleware, "bearer correct-horse-battery-staple", called);
   BOOST_TEST(called);
   BOOST_TEST(static_cast<int>(response.status()) == static_cast<int>(forge::net::http::status::ok));

   for (const auto& authorization : {std::string{}, std::string{"Basic abc"}, std::string{"Bearer wrong"},
                                     std::string{"Bearer "}, std::string{"Bearer bad token\t"}}) {
      called = false;
      response = invoke(middleware, authorization, called);
      BOOST_TEST(!called);
      BOOST_TEST(static_cast<int>(response.status()) == static_cast<int>(forge::net::http::status::unauthorized));
      BOOST_REQUIRE_EQUAL(response.headers().size(), 1U);
      BOOST_TEST(response.headers().front().name == "WWW-Authenticate");
      BOOST_TEST(response.headers().front().value == "Bearer");
   }
}

BOOST_AUTO_TEST_CASE(bearer_auth_rejects_incomplete_configuration) {
   BOOST_CHECK_THROW(static_cast<void>(http::bearer_auth({})), http::exceptions::invalid_config);
   BOOST_CHECK_THROW(static_cast<void>(http::bearer_auth({.token_hashes = {forge::crypto::digest::sha256{}}})),
                     http::exceptions::invalid_config);
}
