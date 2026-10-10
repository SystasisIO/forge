#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <memory>
#include <string>
#include <utility>

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.registry;
import forge.api.http.binding;
import forge.api.http.parameters;
import forge.api.http.proxy;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.http.client;
import forge.net.http.route_context;
import forge.net.http.router;
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

class api : public forge::api::core::contract<api, forge::api::core::surface::local |
                                                   forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<output> read(input request) = 0;
};

} // namespace forge::tests::http_parameters

FORGE_API(::forge::tests::http_parameters::api,
          FORGE_API_CONTRACT("http-parameters-test", 1, 0),
          FORGE_API_METHOD_TYPED(read, ::forge::tests::http_parameters::input,
                                 ::forge::tests::http_parameters::output))
FORGE_HTTP_API(::forge::tests::http_parameters::api, FORGE_HTTP_GET(read, "/items"))

namespace forge::tests::http_parameters {

class implementation final : public api {
 public:
   boost::asio::awaitable<output> read(input request) override
   {
      ++calls;
      co_return output{request.limit.value, std::move(request.token.value), std::move(request.session.value)};
   }

   unsigned calls = 0;
};

BOOST_AUTO_TEST_CASE(query_header_cookie_dto_does_not_require_a_whole_body_codec)
{
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

} // namespace forge::tests::http_parameters
