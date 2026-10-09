#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.core.types;
import forge.api.http.binding;
import forge.api.http.error_renderer;
import forge.api.http.proxy;
import forge.net.http.client;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.http.exceptions;
import forge.net.http.middleware;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.types;

namespace forge::tests::http_error {

struct input : forge::net::http::endpoint_request {
   int value = 0;
};

struct output {
   int value = 0;
};

BOOST_DESCRIBE_STRUCT(input, (), (value))
BOOST_DESCRIBE_STRUCT(output, (), (value))

class api : public forge::api::core::contract<api, forge::api::core::surface::local |
                                                   forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<output> submit(input request) = 0;
};

} // namespace forge::tests::http_error

FORGE_API(::forge::tests::http_error::api,
          FORGE_API_CONTRACT("http-error-test", 1, 0),
          FORGE_API_METHOD_TYPED(submit, ::forge::tests::http_error::input, ::forge::tests::http_error::output))
FORGE_HTTP_API(::forge::tests::http_error::api, FORGE_HTTP_POST(submit, "/submit", ok, FORGE_HTTP_CACHE(no_store)))

namespace forge::tests::http_error {

class implementation final : public api {
 public:
   boost::asio::awaitable<output> submit(input request) override
   {
      ++calls;
      request.response().set("X-Job-Id", "job-123");
      request.response().set("Retry-After", "2");
      if (request.value == 1) {
         FORGE_THROW_EXCEPTION(forge::api::core::exceptions::resource_exhausted, "queue full",
                               forge::exceptions::ctx("private", "hidden-diagnostic"));
      }
      if (request.value == 2) {
         FORGE_THROW_EXCEPTION(forge::net::http::exceptions::payload_too_large, "hidden-server-message");
      }
      if (request.value == 3) {
         throw std::runtime_error{"hidden-foreign-message"};
      }
      co_return output{request.value};
   }

   unsigned calls = 0;
};

struct fixture {
   fixture()
   {
      apis.install<api>(api::describe(), service);
   }

   void mount(forge::api::http::error_renderer renderer = {})
   {
      router.mount(forge::api::http::binding()
                      .use(forge::api::core::binding().serve(apis).build())
                      .errors(std::move(renderer))
                      .bind<api>()
                      .build());
   }

   forge::net::http::response invoke(std::string body, std::string content_type = "application/json")
   {
      auto request = forge::net::http::request{forge::net::http::method::post, "/submit", 11};
      request.keep_alive(true);
      request.set(forge::net::http::field::content_type, content_type);
      request.body() = std::move(body);
      request.prepare_payload();
      auto context = forge::net::http::make_route_context(request);
      context.runtime = &runtime;
      return forge::asio::blocking::run(runtime, router.handle(context));
   }

   forge::asio::runtime runtime;
   forge::api::core::registry apis;
   std::shared_ptr<implementation> service = std::make_shared<implementation>();
   forge::net::http::router router;
};

BOOST_AUTO_TEST_SUITE(http_error_renderer)

BOOST_AUTO_TEST_CASE(default_wire_format_is_unchanged)
{
   auto value = fixture{};
   value.mount();
   const auto error = value.invoke(R"({"value":1})");
   BOOST_TEST(error.result_int() == 429U);
   BOOST_TEST(error.body().find("resource_exhausted") != std::string::npos);
   BOOST_TEST(error.body().find("hidden-diagnostic") == std::string::npos);
   BOOST_TEST(error["X-Job-Id"] == "job-123");
   BOOST_TEST(error["Cache-Control"] == "no-store");
   const auto malformed = value.invoke("{");
   BOOST_TEST(malformed.result_int() == 422U);
   BOOST_TEST(malformed.body().find("validation_error") != std::string::npos);
   BOOST_TEST(value.service->calls == 1U);
}

BOOST_AUTO_TEST_CASE(renderer_receives_typed_cause_and_preserves_endpoint_headers)
{
   auto value = fixture{};
   unsigned rendered = 0;
   value.mount([&](const forge::api::http::error_context& context)
                   -> std::optional<forge::net::http::response>
   {
      ++rendered;
      BOOST_TEST(context.request.target() == "/submit");
      BOOST_TEST(static_cast<unsigned>(context.status) == 429U);
      BOOST_TEST(context.payload.message == "queue full");
      BOOST_CHECK(dynamic_cast<const forge::api::core::exceptions::resource_exhausted*>(context.cause) != nullptr);
      return forge::net::http::make_text_response(context.request, context.status,
                                                  R"({"error":{"message":"queue full"}})", "application/json");
   });
   const auto error = value.invoke(R"({"value":1})");
   BOOST_TEST(error.body() == R"({"error":{"message":"queue full"}})");
   BOOST_TEST(error.result_int() == 429U);
   BOOST_TEST(error.version() == 11U);
   BOOST_TEST(error.keep_alive());
   BOOST_TEST(error["X-Job-Id"] == "job-123");
   BOOST_TEST(error["Retry-After"] == "2");
   BOOST_TEST(error["Cache-Control"] == "no-store");
   BOOST_TEST(rendered == 1U);
   const auto success = value.invoke(R"({"value":0})");
   BOOST_TEST(success.result_int() == 200U);
   BOOST_TEST(rendered == 1U);
}

BOOST_AUTO_TEST_CASE(renderer_covers_parse_and_negotiation_errors_before_invocation)
{
   auto value = fixture{};
   unsigned rendered = 0;
   value.mount([&](const forge::api::http::error_context& context)
                   -> std::optional<forge::net::http::response>
   {
      ++rendered;
      BOOST_CHECK(context.cause != nullptr);
      const auto code = context.status == static_cast<forge::net::http::status>(422)
                           ? forge::net::http::status::bad_request : context.status;
      return forge::net::http::make_text_response(context.request, code, "external error");
   });
   const auto malformed = value.invoke("{");
   BOOST_TEST(malformed.result_int() == 400U);
   BOOST_TEST(malformed.body() == "external error");
   const auto media = value.invoke("abc", "text/plain");
   BOOST_TEST(media.result_int() == 415U);
   BOOST_TEST(media.body() == "external error");
   BOOST_TEST(rendered == 2U);
   BOOST_TEST(value.service->calls == 0U);
}

BOOST_AUTO_TEST_CASE(nullopt_and_throwing_renderer_retain_safe_projection)
{
   for (const auto should_throw : {false, true}) {
      auto value = fixture{};
      value.mount([should_throw](const forge::api::http::error_context& context)
                    -> std::optional<forge::net::http::response>
      {
         BOOST_TEST(context.payload.message == "internal error");
         BOOST_TEST(static_cast<unsigned>(context.status) == 500U);
         if (should_throw) {
            FORGE_THROW_EXCEPTION(forge::api::core::exceptions::codec_failed, "hidden-renderer-message");
         }
         return std::nullopt;
      });
      const auto error = value.invoke(R"({"value":2})");
      BOOST_TEST(error.result_int() == 500U);
      BOOST_TEST(error.body().find("internal error") != std::string::npos);
      BOOST_TEST(error.body().find("hidden-") == std::string::npos);
      BOOST_TEST(error["X-Job-Id"] == "job-123");
   }
}

BOOST_AUTO_TEST_CASE(explicit_route_renderer_overrides_publication_default)
{
   auto value = fixture{};
   unsigned defaults = 0;
   unsigned overrides = 0;
   value.router.mount(forge::api::http::binding()
       .use(forge::api::core::binding().serve(value.apis).build())
       .errors([&](const forge::api::http::error_context&)
                  -> std::optional<forge::net::http::response>
       {
          ++defaults;
          return std::nullopt;
       })
       .post<&api::submit, input, output>("/submit", {
          .error_renderer = [&](const forge::api::http::error_context& context)
                                -> std::optional<forge::net::http::response>
          {
             ++overrides;
             return forge::net::http::make_text_response(context.request, context.status, "route error");
          }})
       .build());
   const auto error = value.invoke(R"({"value":1})");
   BOOST_TEST(error.body() == "route error");
   BOOST_TEST(defaults == 0U);
   BOOST_TEST(overrides == 1U);
}

BOOST_AUTO_TEST_CASE(missing_local_registry_uses_the_external_error_boundary)
{
   auto value = fixture{};
   value.router.mount(forge::api::http::binding()
       .errors([](const forge::api::http::error_context& context)
                 -> std::optional<forge::net::http::response>
       {
          BOOST_CHECK(dynamic_cast<const forge::api::core::exceptions::incompatible_version*>(context.cause));
          return forge::net::http::make_text_response(context.request,
              forge::net::http::status::service_unavailable, "external unavailable");
       })
       .bind<api>()
       .build());
   const auto error = value.invoke(R"({"value":0})");
   BOOST_TEST(error.result_int() == 503U);
   BOOST_TEST(error.body() == "external unavailable");
   BOOST_TEST(value.service->calls == 0U);
}

BOOST_AUTO_TEST_CASE(unexpected_failure_has_no_borrowed_cause_and_no_diagnostic_disclosure)
{
   auto value = fixture{};
   value.mount([](const forge::api::http::error_context& context)
                 -> std::optional<forge::net::http::response>
   {
      BOOST_CHECK(context.cause == nullptr);
      BOOST_TEST(static_cast<unsigned>(context.status) == 500U);
      BOOST_TEST(context.payload.message == "internal error");
      return forge::net::http::make_text_response(context.request, context.status, "external internal error");
   });
   const auto error = value.invoke(R"({"value":3})");
   BOOST_TEST(error.result_int() == 500U);
   BOOST_TEST(error.body() == "external internal error");
   BOOST_TEST(error["X-Job-Id"] == "job-123");
   auto defaults = fixture{};
   defaults.mount();
   const auto unchanged = defaults.invoke(R"({"value":3})");
   BOOST_TEST(unchanged.result_int() == 500U);
   BOOST_TEST(unchanged.body().find("internal error") != std::string::npos);
   BOOST_TEST(unchanged.body().find("hidden-") == std::string::npos);
   BOOST_TEST(unchanged.body().find("forge.net.http") != std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::tests::http_error
