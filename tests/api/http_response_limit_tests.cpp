#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

import forge.api.core.binding;
import forge.api.core.registry;
import forge.api.http.binding;
import forge.api.http.parameters;
import forge.api.http.proxy;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.http.base_url;
import forge.net.http.body;
import forge.net.http.client;
import forge.net.http.exceptions;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.stream;
import forge.net.http.types;

namespace forge::tests::http_api_response_limit {

inline constexpr auto response_bound = std::uint64_t{10} << 20;

struct request
{
   std::uint64_t bytes = 0;
   bool chunked = false;
};

class api : public forge::api::core::contract<api, forge::api::core::surface::local |
                                                  forge::api::core::surface::remote>
{
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<forge::net::http::streaming_response> ordinary(request input) = 0;
   virtual boost::asio::awaitable<forge::net::http::streaming_response> large(request input) = 0;
   virtual boost::asio::awaitable<forge::net::http::streaming_response> positional(std::uint64_t bytes,
                                                                                 bool chunked) = 0;
};

BOOST_DESCRIBE_STRUCT(request, (), (bytes, chunked))

} // namespace forge::tests::http_api_response_limit

FORGE_API(::forge::tests::http_api_response_limit::api, FORGE_API_CONTRACT("http.response.limit.test", 1, 0),
          FORGE_API_METHOD_TYPED(ordinary, ::forge::tests::http_api_response_limit::request,
                                 ::forge::net::http::streaming_response),
          FORGE_API_METHOD_TYPED(large, ::forge::tests::http_api_response_limit::request,
                                 ::forge::net::http::streaming_response),
          FORGE_API_METHOD(positional, bytes, chunked))

FORGE_HTTP_API(::forge::tests::http_api_response_limit::api,
               FORGE_HTTP_GET(ordinary, "/ordinary?bytes={bytes}&chunked={chunked}", FORGE_HTTP_RESPONSE_STREAM),
               FORGE_HTTP_GET(large, "/large?bytes={bytes}&chunked={chunked}",
                               FORGE_HTTP_RESPONSE_STREAM,
                               FORGE_HTTP_RESPONSE_BODY_LIMIT(::forge::tests::http_api_response_limit::response_bound)),
               FORGE_HTTP_GET(positional, "/positional?bytes={bytes}&chunked={chunked}",
                               FORGE_HTTP_RESPONSE_STREAM,
                               FORGE_HTTP_RESPONSE_BODY_LIMIT(::forge::tests::http_api_response_limit::response_bound)))

namespace forge::tests::http_api_response_limit {

class generated_body final : public forge::net::http::body_reader::source
{
 public:
   explicit generated_body(std::uint64_t size)
     : _size(size)
   {
   }

   boost::asio::awaitable<std::optional<forge::net::http::body_chunk>> async_read() override
   {
      if (_offset == _size)
      {
         co_return std::nullopt;
      }

      const auto count = static_cast<std::size_t>((std::min)(_size - _offset, std::uint64_t{65536}));
      _offset += count;
      co_return forge::net::http::body_chunk{.bytes = std::vector<std::byte>(count, std::byte{'x'})};
   }

   std::uint64_t bytes_read() const noexcept override
   {
      return _offset;
   }

 private:
   std::uint64_t _size;
   std::uint64_t _offset = 0;
};

class implementation final : public api
{
 public:
   boost::asio::awaitable<forge::net::http::streaming_response> ordinary(request input) override
   {
      auto head = forge::net::http::response{forge::net::http::status::ok, 11};
      if (!input.chunked)
      {
         head.set(forge::net::http::field::content_length, std::to_string(input.bytes));
      }

      co_return forge::net::http::streaming_response::from_body(
          std::move(head), forge::net::http::body_reader{std::make_shared<generated_body>(input.bytes)});
   }

   boost::asio::awaitable<forge::net::http::streaming_response> large(request input) override
   {
      co_return co_await ordinary(input);
   }

   boost::asio::awaitable<forge::net::http::streaming_response> positional(std::uint64_t bytes, bool chunked) override
   {
      co_return co_await ordinary({bytes, chunked});
   }
};

class fixture
{
 public:
   fixture()
   {
      _apis.install<api>(api::describe(), std::make_shared<implementation>());
      auto router = forge::net::http::router{};
      router.mount(forge::api::http::binding()
                       .use(forge::api::core::binding().serve(_apis).build())
                       .bind<api>()
                       .build());
      _server = std::make_unique<forge::net::http::server>(runtime, forge::net::http::server_config{}, std::move(router));
      forge::asio::blocking::run(runtime, _server->async_start());
      _client = std::make_unique<forge::net::http::client>(
          runtime, forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(_server->port())));
   }

   ~fixture()
   {
      forge::asio::blocking::run(runtime, _server->async_stop());
   }

   boost::asio::awaitable<std::uint64_t> fetch(unsigned route, std::uint64_t bytes, bool chunked)
   {
      const auto remote = co_await forge::api::http::remote<api>(*_client);
      auto response = route == 0 ? co_await remote->ordinary(request{bytes, chunked})
                                : route == 1 ? co_await remote->large(request{bytes, chunked})
                                             : co_await remote->positional(bytes, chunked);
      auto count = std::uint64_t{};
      while (auto chunk = co_await response.body().async_read())
      {
         BOOST_TEST(std::ranges::all_of(chunk->bytes,
                                       [](std::byte value)
                                       {
                                          return value == std::byte{'x'};
                                       }));
         count += chunk->bytes.size();
      }

      co_return count;
   }

   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 2}};

 private:
   forge::api::core::registry _apis;
   std::unique_ptr<forge::net::http::server> _server;
   std::unique_ptr<forge::net::http::client> _client;
};

BOOST_AUTO_TEST_CASE(generated_clients_apply_explicit_bound_and_preserve_default_per_route)
{
   auto test = fixture{};
   for (const auto chunked : {false, true})
   {
      BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, test.fetch(0, 9ULL << 20, chunked)),
                        forge::net::http::exceptions::unavailable);
      for (const auto route : {1U, 2U})
      {
         BOOST_TEST(forge::asio::blocking::run(test.runtime, test.fetch(route, 9ULL << 20, chunked)) == (9ULL << 20));
         BOOST_TEST(forge::asio::blocking::run(test.runtime, test.fetch(route, response_bound, chunked)) == response_bound);
         BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, test.fetch(route, response_bound + 1, chunked)),
                           forge::net::http::exceptions::unavailable);
      }

      BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, test.fetch(0, (8ULL << 20) + 1, chunked)),
                        forge::net::http::exceptions::unavailable);
      BOOST_TEST(forge::asio::blocking::run(test.runtime, test.fetch(0, 8ULL << 20, chunked)) == (8ULL << 20));
   }
}

} // namespace forge::tests::http_api_response_limit
