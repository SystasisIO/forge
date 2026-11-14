#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.registry;
import forge.api.http.binding;
import forge.api.http.openapi;
import forge.api.http.parameters;
import forge.api.http.proxy;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.http.client;
import forge.net.http.body;
import forge.net.http.exceptions;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.stream;
import forge.net.http.types;

namespace forge::tests::http_parameters {

struct input : forge::net::http::endpoint_request {
   forge::api::http::query<unsigned> limit;
   forge::api::http::header<std::string> token;
   forge::api::http::cookie<std::string> session;
};

struct output {
   unsigned limit = 0;
   std::string token;
   std::string session;
};

BOOST_DESCRIBE_STRUCT(input, (), (limit, token, session))
BOOST_DESCRIBE_STRUCT(output, (), (limit, token, session))

class api
    : public forge::api::core::contract<api, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<output> read(input request) = 0;
   virtual boost::asio::awaitable<output> write(input request) = 0;
   virtual boost::asio::awaitable<forge::net::http::streaming_response> content(input request) = 0;
};

} // namespace forge::tests::http_parameters

FORGE_API(::forge::tests::http_parameters::api, FORGE_API_CONTRACT("http-parameters-test", 1, 0),
          FORGE_API_METHOD_TYPED(read, ::forge::tests::http_parameters::input, ::forge::tests::http_parameters::output),
          FORGE_API_METHOD_TYPED(write, ::forge::tests::http_parameters::input, ::forge::tests::http_parameters::output),
          FORGE_API_METHOD_TYPED(content, ::forge::tests::http_parameters::input,
                                 ::forge::net::http::streaming_response))
FORGE_HTTP_API(::forge::tests::http_parameters::api, FORGE_HTTP_GET(read, "/items"),
               FORGE_HTTP_POST(write, "/items", created, FORGE_HTTP_ALTERNATE_SUCCESS_STATUS(ok)),
               FORGE_HTTP_GET(content, "/content", FORGE_HTTP_RESPONSE_STREAM,
                              FORGE_HTTP_ALTERNATE_SUCCESS_STATUS(partial_content)))

namespace forge::tests::http_parameters {

class implementation final : public api {
 public:
   boost::asio::awaitable<output> read(input request) override {
      ++calls;
      if (request.token.value == "alternate") {
         request.response().result(forge::net::http::status::created);
      }
      co_return output{request.limit.value, std::move(request.token.value), std::move(request.session.value)};
   }

   boost::asio::awaitable<output> write(input request) override {
      if (request.token.value == "alternate") {
         request.response().result(forge::net::http::status::ok);
      }
      request.response().set(forge::net::http::field::content_length, "999");
      co_return output{.token = std::move(request.token.value)};
   }

   boost::asio::awaitable<forge::net::http::streaming_response> content(input request) override {
      auto head = forge::net::http::response{forge::net::http::status::partial_content, 11};
      head.set(forge::net::http::field::content_length, "1");
      if (request.token.value == "valid") {
         head.set(forge::net::http::field::content_range, "bytes 1-1/3");
      }
      request.response().set(forge::net::http::field::content_range, "bytes 0-0/3");
      auto body = forge::net::http::body_reader::from_callback(
          [sent = false]() mutable -> boost::asio::awaitable<std::optional<forge::net::http::body_chunk>> {
             if (std::exchange(sent, true)) {
                co_return std::nullopt;
             }
             co_return forge::net::http::body_chunk{.bytes = {std::byte{'x'}}};
          });
      co_return forge::net::http::streaming_response::from_body(std::move(head), std::move(body));
   }

   unsigned calls = 0;
};

BOOST_AUTO_TEST_CASE(query_header_cookie_dto_does_not_require_a_whole_body_codec) {
   auto runtime = forge::asio::runtime{};
   auto apis = forge::api::core::registry{};
   auto service = std::make_shared<implementation>();
   apis.install<api>(api::describe(), service);
   auto router = forge::net::http::router{};
   router.mount(forge::api::http::binding().use(forge::api::core::binding().serve(apis).build()).bind<api>().build());

   auto request = forge::net::http::request{forge::net::http::method::get, "/items?limit=7", 11};
   request.set("token", "header-value");
   request.set("Cookie", "session=cookie-value");
   auto context = forge::net::http::make_route_context(request);
   context.runtime = &runtime;
   const auto response = forge::asio::blocking::run(runtime, router.handle(context));
   BOOST_TEST(response.result_int() == 200U);
   BOOST_TEST(response.body().find("\"limit\":7") != std::string::npos);
   BOOST_TEST(response.body().find("header-value") != std::string::npos);
   BOOST_TEST(response.body().find("cookie-value") != std::string::npos);
   BOOST_TEST(service->calls == 1U);

   request.target("/items?limit=invalid");
   auto invalid = forge::net::http::make_route_context(request);
   invalid.runtime = &runtime;
   const auto failure = forge::asio::blocking::run(runtime, router.handle(invalid));
   BOOST_TEST(failure.result_int() == 422U);
   BOOST_TEST(service->calls == 1U);
}

BOOST_AUTO_TEST_CASE(explicit_alternate_success_status_keeps_json_codec_and_strict_default) {
   auto runtime = forge::asio::runtime{};
   auto apis = forge::api::core::registry{};
   apis.install<api>(api::describe(), std::make_shared<implementation>());
   auto router = forge::net::http::router{};
   router.mount(forge::api::http::binding().use(forge::api::core::binding().serve(apis).build()).bind<api>().build());

   auto request = forge::net::http::request{forge::net::http::method::post, "/items", 11};
   auto invoke = [&]() {
      auto context = forge::net::http::make_route_context(request);
      context.runtime = &runtime;
      return forge::asio::blocking::run(runtime, router.handle(context));
   };

   const auto created = invoke();
   BOOST_TEST(created.result_int() == 201U);
   request.set("token", "alternate");
   const auto existing = invoke();
   BOOST_TEST(existing.result_int() == 200U);
   BOOST_TEST(existing.body().find("alternate") != std::string::npos);
   BOOST_TEST(existing[forge::net::http::field::content_length] == std::to_string(existing.body().size()));

   request.method(forge::net::http::method::get);
   BOOST_TEST(invoke().result_int() == 500U);

   const auto document = forge::api::http::openapi<api>();
   const auto& success = document["paths"]["/items"]["post"]["responses"];
   BOOST_CHECK(success["200"]["content"] == success["201"]["content"]);
   const auto& content = document["paths"]["/content"]["get"]["responses"];
   BOOST_CHECK(content["200"]["content"] == content["206"]["content"]);
}

BOOST_AUTO_TEST_CASE(alternate_partial_stream_requires_its_own_range_header) {
   auto runtime = forge::asio::runtime{};
   auto apis = forge::api::core::registry{};
   apis.install<api>(api::describe(), std::make_shared<implementation>());
   auto router = forge::net::http::router{};
   router.mount(forge::api::http::binding().use(forge::api::core::binding().serve(apis).build()).bind<api>().build());

   auto request = forge::net::http::request{forge::net::http::method::get, "/content", 11};
   request.set("token", "valid");
   auto context = forge::net::http::make_route_context(request);
   context.runtime = &runtime;
   auto stream = forge::net::http::stream_request{context, {}};
   auto response = forge::asio::blocking::run(runtime, router.handle_stream(stream));
   BOOST_TEST(response.head.result_int() == 206U);
   BOOST_TEST(response.head[forge::net::http::field::content_range] == "bytes 1-1/3");
   BOOST_TEST(response.head[forge::net::http::field::content_length] == "1");
   BOOST_REQUIRE(response.body);
   const auto chunk = forge::asio::blocking::run(runtime, response.body());
   BOOST_REQUIRE(chunk);
   BOOST_REQUIRE_EQUAL(chunk->bytes.size(), 1U);
   BOOST_CHECK(chunk->bytes.front() == std::byte{'x'});
   BOOST_CHECK(!forge::asio::blocking::run(runtime, response.body()));

   request.erase("token");
   auto invalid = forge::net::http::make_route_context(request);
   invalid.runtime = &runtime;
   auto invalid_stream = forge::net::http::stream_request{invalid, {}};
   const auto rejected = forge::asio::blocking::run(runtime, router.handle_stream(invalid_stream));
   BOOST_TEST(rejected.head.result_int() == 500U);
   BOOST_TEST(rejected.head[forge::net::http::field::content_range].empty());
}

BOOST_AUTO_TEST_CASE(non_success_status_cannot_be_declared_as_an_alternate) {
   auto router = forge::net::http::router{};
   auto binding = forge::api::http::binding()
                      .route<&api::read, input, output>(
                          forge::api::http::route_builder{
                              forge::net::http::method::get, "read", "/invalid", forge::net::http::status::ok}
                              .alternate_success_status(forge::net::http::status::not_found)
                              .build())
                      .build();
   BOOST_CHECK_THROW(router.mount(binding), forge::net::http::exceptions::bad_request);
}

} // namespace forge::tests::http_parameters
