#include <boost/asio/awaitable.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/write.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>
#include <forge/exceptions/macros.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <vector>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

import forge.api.core.descriptor;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.core.server_supplied;
import forge.api.core.trusted_invocation;
import forge.api.core.stream_reader;
import forge.api.core.stream_writer;
import forge.api.http.binding;
import forge.api.http.error_renderer;
import forge.api.http.client_request;
import forge.api.http.openapi;
import forge.api.http.proxy;
import forge.codec.json;
import forge.api.http.parameters;
import forge.app.application_builder;
import forge.app.application_shell;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.config.core.document;
import forge.net.http.base_url;
import forge.net.http.body;
import forge.net.http.client;
import forge.net.http.exceptions;
import forge.net.http.route_context;
import forge.net.http.stream;
import forge.net.http.types;
import forge.net.http.upload;
import forge.plugins.net.http.server.api;
import forge.plugins.net.http.server.plugin;
import forge.plugins.net.http.server.types;
import forge.variant.value;
import forge.variant.containers;
import forge.variant.described;
import forge.variant.static_variant;
import forge.schema.object;

namespace forge::tests::http_admission {

struct identity {
   std::string id;
};

struct input : forge::net::http::endpoint_request {
   std::string id;
   identity caller;
};

struct upload_input : forge::net::http::endpoint_request {
   identity caller;
   forge::api::http::body_stream body;
};

struct business_payload {
   std::string caller;
};

struct wrapped_input : forge::net::http::endpoint_request {
   forge::api::http::body<business_payload> data;
   identity caller;
};

struct query_input : forge::net::http::endpoint_request {
   forge::api::http::query<std::string> label;
   identity caller;
};

struct form_input : forge::net::http::endpoint_request {
   forge::api::http::form_field<std::string> label;
   identity caller;
};

struct renamed_input {
   std::string id;
   identity caller;
};

using choice = std::variant<std::int8_t, std::string>;

struct numeric_input {
   std::int8_t count = 0;
   choice selected;
   std::uint64_t wide = 0;
   std::optional<std::uint64_t> maybe;
   bool flag = true;
   identity caller;
};

struct schema_numeric_input {
   std::int8_t count = 0;
   // Schema fields support numeric arrays; numeric_input covers actual indexed std::variant values.
   std::vector<std::int8_t> selected;
   std::uint64_t wide = 0;
   std::optional<std::uint64_t> maybe;
   bool flag = true;
   identity caller;
};

struct dotted_input {
   identity caller;
};

struct dotted_alias_input {
   identity caller;
};

struct unsupported_body_input {
   forge::api::http::body<input> data;
};

struct legacy_identity {
   std::string id;
};

struct legacy_input : forge::net::http::endpoint_request {
   std::string id;
   legacy_identity caller;
};

struct output {
   std::string value;
};

class api
    : public forge::api::core::contract<api, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<output> submit(input request) = 0;
   virtual boost::asio::awaitable<output> upload(upload_input request) = 0;
   virtual boost::asio::awaitable<output> wrapped(wrapped_input request) = 0;
   virtual boost::asio::awaitable<output> query(query_input request) = 0;
   virtual boost::asio::awaitable<output> form(form_input request) = 0;
   virtual boost::asio::awaitable<output> numeric(numeric_input request) = 0;
   virtual boost::asio::awaitable<output> schema_numeric(schema_numeric_input request) = 0;
   virtual boost::asio::awaitable<forge::net::http::streaming_response> download(input request) = 0;
};

class unsupported_api : public forge::api::core::contract<unsupported_api, forge::api::core::surface::local |
                                                                               forge::api::core::surface::remote> {
 public:
   virtual ~unsupported_api() = default;
   virtual boost::asio::awaitable<output> body(unsupported_body_input request) = 0;
   virtual boost::asio::awaitable<output> dotted(dotted_input request) = 0;
   virtual boost::asio::awaitable<output> dotted_alias(dotted_alias_input request) = 0;
   virtual boost::asio::awaitable<void> positional_body(forge::api::http::header<std::string> tag,
                                                        forge::api::http::body<input> data,
                                                        forge::api::core::stream_writer<output> output) = 0;
   virtual boost::asio::awaitable<void> positional_plain(std::string id, input data,
                                                         forge::api::core::stream_writer<output> output) = 0;
};

class legacy_api : public forge::api::core::contract<legacy_api, forge::api::core::surface::local |
                                                                     forge::api::core::surface::remote> {
 public:
   virtual ~legacy_api() = default;
   virtual boost::asio::awaitable<output> submit(legacy_input request) = 0;
};

class client_stream_api : public forge::api::core::contract<client_stream_api, forge::api::core::surface::local |
                                                                                   forge::api::core::surface::remote> {
 public:
   virtual ~client_stream_api() = default;
   virtual boost::asio::awaitable<output> upload(input request, forge::api::core::stream_reader<output> items) = 0;
};

class legacy_client_stream_api
    : public forge::api::core::contract<legacy_client_stream_api,
                                        forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~legacy_client_stream_api() = default;
   virtual boost::asio::awaitable<output> upload(legacy_input request,
                                                 forge::api::core::stream_reader<output> items) = 0;
};

BOOST_DESCRIBE_STRUCT(business_payload, (), (caller))
BOOST_DESCRIBE_STRUCT(wrapped_input, (), (data, caller))
BOOST_DESCRIBE_STRUCT(query_input, (), (label, caller))
BOOST_DESCRIBE_STRUCT(form_input, (), (label, caller))
BOOST_DESCRIBE_STRUCT(renamed_input, (), (id, caller))
BOOST_DESCRIBE_STRUCT(numeric_input, (), (count, selected, wide, maybe, flag, caller))
BOOST_DESCRIBE_STRUCT(schema_numeric_input, (), (count, selected, wide, maybe, flag, caller))
BOOST_DESCRIBE_STRUCT(dotted_input, (), (caller))
BOOST_DESCRIBE_STRUCT(dotted_alias_input, (), (caller))
BOOST_DESCRIBE_STRUCT(unsupported_body_input, (), (data))
BOOST_DESCRIBE_STRUCT(legacy_identity, (), (id))
BOOST_DESCRIBE_STRUCT(legacy_input, (), (id, caller))
BOOST_DESCRIBE_STRUCT(identity, (), (id))
BOOST_DESCRIBE_STRUCT(input, (), (id, caller))
BOOST_DESCRIBE_STRUCT(upload_input, (), (caller, body))
BOOST_DESCRIBE_STRUCT(output, (), (value))

} // namespace forge::tests::http_admission

template <> struct forge::api::core::server_supplied<forge::tests::http_admission::identity> {
   static constexpr bool required = true;
   static constexpr bool http_visible = false;

   static void reset(forge::tests::http_admission::identity& value) {
      value = {};
   }

   static bool apply(forge::tests::http_admission::identity& value,
                     const forge::api::core::trusted_invocation& trusted) {
      const auto* source = trusted.find<forge::tests::http_admission::identity>();
      if (source == nullptr) {
         return false;
      }
      value = *source;
      return true;
   }
};

template <> struct forge::api::core::server_supplied<forge::tests::http_admission::legacy_identity> {
   static constexpr bool required = false;

   static void reset(forge::tests::http_admission::legacy_identity& value) {
      value = {};
   }

   static bool apply(forge::tests::http_admission::legacy_identity& value,
                     const forge::api::core::trusted_invocation& trusted) {
      const auto* source = trusted.find<forge::tests::http_admission::legacy_identity>();
      if (source == nullptr) {
         return false;
      }
      value = *source;
      return true;
   }
};

template <> struct forge::schema::rules<forge::tests::http_admission::renamed_input> {
   static forge::schema::object_schema<forge::tests::http_admission::renamed_input> define() {
      auto rules = forge::schema::object<forge::tests::http_admission::renamed_input>();
      rules.field<&forge::tests::http_admission::renamed_input::id>("id").required();
      rules.field<&forge::tests::http_admission::renamed_input::caller>("native-facts").alias("old-facts");
      return rules;
   }
};

template <> struct forge::schema::rules<forge::tests::http_admission::schema_numeric_input> {
   static forge::schema::object_schema<forge::tests::http_admission::schema_numeric_input> define() {
      using input = forge::tests::http_admission::schema_numeric_input;
      auto rules = forge::schema::object<input>();
      rules.field<&input::count>("total").required().range(0, 100);
      rules.field<&input::selected>("choice").alias("legacy-choice").required();
      rules.field<&input::wide>("wide").required();
      rules.field<&input::maybe>("maybe");
      rules.field<&input::flag>("flag").default_value(true);
      rules.field<&input::caller>("native-facts").alias("old-facts");
      return rules;
   }
};

template <> struct forge::schema::rules<forge::tests::http_admission::dotted_input> {
   static forge::schema::object_schema<forge::tests::http_admission::dotted_input> define() {
      auto rules = forge::schema::object<forge::tests::http_admission::dotted_input>();
      rules.field<&forge::tests::http_admission::dotted_input::caller>("native.facts");
      return rules;
   }
};

template <> struct forge::schema::rules<forge::tests::http_admission::dotted_alias_input> {
   static forge::schema::object_schema<forge::tests::http_admission::dotted_alias_input> define() {
      auto rules = forge::schema::object<forge::tests::http_admission::dotted_alias_input>();
      rules.field<&forge::tests::http_admission::dotted_alias_input::caller>("native-facts").alias("old.facts");
      return rules;
   }
};

template <> struct forge::api::http::json_schema_traits<forge::tests::http_admission::renamed_input> {
   static forge::variant make() {
      return forge::variant{forge::mutable_variant_object{}("type", "object")(
          "properties",
          forge::mutable_variant_object{}("id", forge::api::http::detail::make_json_schema<std::string>())(
              "native-facts", forge::api::http::detail::make_json_schema<forge::tests::http_admission::identity>()))(
          "required", forge::variants{forge::variant{"id"}, forge::variant{"native-facts"}})};
   }
};

FORGE_API(::forge::tests::http_admission::legacy_api, FORGE_API_CONTRACT("http-admission-legacy", 1, 0),
          FORGE_API_METHOD(submit))
FORGE_HTTP_API(::forge::tests::http_admission::legacy_api, FORGE_HTTP_POST(submit, "/submit/:id", ok))

FORGE_API(::forge::tests::http_admission::client_stream_api, FORGE_API_CONTRACT("http-admission-client-stream", 1, 0),
          FORGE_API_METHOD(upload))
FORGE_API(::forge::tests::http_admission::legacy_client_stream_api,
          FORGE_API_CONTRACT("http-admission-legacy-client-stream", 1, 0), FORGE_API_METHOD(upload))
FORGE_API(::forge::tests::http_admission::api, FORGE_API_CONTRACT("http-admission-test", 1, 0),
          FORGE_API_METHOD(submit), FORGE_API_METHOD(upload), FORGE_API_METHOD(download), FORGE_API_METHOD(wrapped),
          FORGE_API_METHOD(query), FORGE_API_METHOD(form), FORGE_API_METHOD(numeric), FORGE_API_METHOD(schema_numeric))
FORGE_HTTP_API(::forge::tests::http_admission::api, FORGE_HTTP_POST(submit, "/submit/:id", ok),
               FORGE_HTTP_PUT(upload, "/upload", ok, FORGE_HTTP_BODY_STREAM(body)),
               FORGE_HTTP_POST(wrapped, "/wrapped", ok), FORGE_HTTP_GET(query, "/query"),
               FORGE_HTTP_POST(form, "/form", ok, FORGE_HTTP_FORM(label, "label")),
               FORGE_HTTP_POST(numeric, "/numeric", ok), FORGE_HTTP_POST(schema_numeric, "/schema-numeric", ok),
               FORGE_HTTP_GET(download, "/download/:id", FORGE_HTTP_RESPONSE_STREAM))

FORGE_API(::forge::tests::http_admission::unsupported_api, FORGE_API_CONTRACT("http-admission-unsupported", 1, 0),
          FORGE_API_METHOD(body), FORGE_API_METHOD(dotted), FORGE_API_METHOD(dotted_alias),
          FORGE_API_METHOD(positional_body, tag, data), FORGE_API_METHOD(positional_plain, id, data))
FORGE_HTTP_API(::forge::tests::http_admission::unsupported_api, FORGE_HTTP_POST(body, "/body", ok),
               FORGE_HTTP_POST(dotted, "/dotted", ok), FORGE_HTTP_POST(dotted_alias, "/dotted-alias", ok))

namespace forge::tests::http_admission {

struct observations {
   std::atomic<unsigned> admissions = 0;
   std::atomic<unsigned> handlers = 0;
   std::atomic<unsigned> response_reads = 0;
   std::atomic<bool> descriptor_seen = false;
};

class legacy_implementation final : public legacy_api {
 public:
   boost::asio::awaitable<output> submit(legacy_input request) override {
      co_return output{request.id + ":" + request.caller.id};
   }
};

class implementation final : public api {
 public:
   explicit implementation(std::shared_ptr<observations> state) : state_{std::move(state)} {}

   boost::asio::awaitable<output> submit(input request) override {
      ++state_->handlers;
      co_return output{request.id + ":" + request.caller.id};
   }

   boost::asio::awaitable<output> upload(upload_input request) override {
      ++state_->handlers;
      const auto body = co_await request.body.async_read_all();
      co_return output{request.caller.id + ":" + body};
   }

   boost::asio::awaitable<output> wrapped(wrapped_input request) override {
      ++state_->handlers;
      co_return output{request.caller.id + ":" + request.data.value.caller};
   }

   boost::asio::awaitable<output> query(query_input request) override {
      ++state_->handlers;
      co_return output{request.caller.id + ":" + request.label.value};
   }

   boost::asio::awaitable<output> form(form_input request) override {
      ++state_->handlers;
      co_return output{request.caller.id + ":" + request.label.value};
   }

   boost::asio::awaitable<output> numeric(numeric_input request) override {
      ++state_->handlers;
      co_return output{request.caller.id + ":" + std::to_string(request.count)};
   }

   boost::asio::awaitable<output> schema_numeric(schema_numeric_input request) override {
      ++state_->handlers;
      co_return output{request.caller.id + ":" + std::to_string(request.count)};
   }

   boost::asio::awaitable<forge::net::http::streaming_response> download(input request) override {
      ++state_->handlers;
      co_return forge::net::http::streaming_response::from_source({
          .body = [state = state_, message = std::move(request.caller.id),
                   sent = false]() mutable -> boost::asio::awaitable<std::optional<forge::net::http::body_chunk>> {
             ++state->response_reads;
             if (sent) {
                co_return std::nullopt;
             }
             sent = true;
             auto bytes = std::vector<std::byte>(message.size());
             std::memcpy(bytes.data(), message.data(), message.size());
             co_return forge::net::http::body_chunk{std::move(bytes)};
          },
      });
   }

 private:
   std::shared_ptr<observations> state_;
};

boost::asio::awaitable<unsigned> headers_only(std::uint16_t port) {
   const auto executor = co_await boost::asio::this_coro::executor;
   auto stream = boost::beast::tcp_stream{executor};
   stream.expires_after(std::chrono::seconds{2});
   co_await stream.async_connect({boost::asio::ip::make_address("127.0.0.1"), port}, boost::asio::use_awaitable);
   const auto headers = std::string{"PUT /secure/upload HTTP/1.1\r\nHost: 127.0.0.1\r\nExpect: 100-continue\r\n"
                                    "Content-Length: 1024\r\nContent-Type: application/octet-stream\r\n\r\n"};
   co_await boost::asio::async_write(stream, boost::asio::buffer(headers), boost::asio::use_awaitable);
   auto buffer = boost::beast::flat_buffer{};
   auto response = boost::beast::http::response<boost::beast::http::string_body>{};
   co_await boost::beast::http::async_read(stream, buffer, response, boost::asio::use_awaitable);
   co_return response.result_int();
}

} // namespace forge::tests::http_admission

BOOST_AUTO_TEST_CASE(http_publication_admits_trusted_identity_and_preserves_legacy_default) {
   namespace fixture = forge::tests::http_admission;
   namespace http = forge::plugins::net::http::server;
   auto state = std::make_shared<fixture::observations>();
   auto builder = forge::app::application_builder{};
   builder.name("http-admission-test")
       .runtime({.worker_threads = 2})
       .plugin(http::plugin::descriptor())
       .provide([state](forge::app::application_context& context) {
          context.apis().install<fixture::api>(fixture::api::describe(),
                                               std::make_shared<fixture::implementation>(state));
          context.apis().install<fixture::legacy_api>(fixture::legacy_api::describe(),
                                                      std::make_shared<fixture::legacy_implementation>());
       });
   auto app = std::move(builder).build();
   auto io = boost::asio::io_context{};
   auto reserved = boost::asio::ip::tcp::acceptor{io, {boost::asio::ip::make_address("127.0.0.1"), 0}};
   const auto port = reserved.local_endpoint().port();
   reserved.close();
   auto config = forge::config::core::document{};
   config.set("plugins.net.http.server.port", std::uint64_t{port});
   config.set("plugins.net.http.server.read-timeout-ms", std::uint64_t{500});
   app->configure(config);
   forge::asio::blocking::run(app->runtime(), app->initialize());
   const auto server = app->apis().get<http::api>(http::api::ref());
   forge::asio::blocking::run(
       app->runtime(),
       server->publish<fixture::api>({
           .base_path = "/secure",
           .error_renderer =
               [](const forge::api::http::error_context& context) -> std::optional<forge::net::http::response> {
              if (context.request.target() == "/secure/schema-numeric") {
                 return std::nullopt;
              }
              return forge::net::http::make_text_response(context.request, context.status, "external-denied");
           },
           .invocation = [state](const forge::net::http::route_context& context,
                                 const forge::api::core::method_descriptor& method)
               -> boost::asio::awaitable<forge::api::core::invocation_context> {
              ++state->admissions;
              state->descriptor_seen = method.name == "submit" || method.name == "upload" ||
                                       method.name == "download" || method.name == "wrapped" ||
                                       method.name == "query" || method.name == "form" || method.name == "numeric" ||
                                       method.name == "schema_numeric";
              if (context.request.header("X-Test-Admission") != std::optional<std::string_view>{"accepted"}) {
                 FORGE_THROW_EXCEPTION(forge::api::core::exceptions::invocation_denied, "private-admission-diagnostic");
              }
              co_return forge::api::core::invocation_context{
                  .trusted =
                      forge::api::core::trusted_invocation_builder{}.set(fixture::identity{"server-owner"}).build(),
              };
           },
       }));
   forge::asio::blocking::run(app->runtime(), server->publish<fixture::legacy_api>({.base_path = "/legacy"}));
   forge::asio::blocking::run(app->runtime(), app->startup());
   auto client = forge::net::http::client{app->runtime(),
                                          forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(port))};

   const auto send = [&](std::string target, std::string body, bool admitted) {
      auto request = forge::net::http::request{forge::net::http::method::post, target, 11};
      request.set(forge::net::http::field::content_type, "application/json");
      if (admitted) {
         request.set("X-Test-Admission", "accepted");
      }
      request.body() = std::move(body);
      request.prepare_payload();
      return forge::asio::blocking::run(app->runtime(), client.async_request(std::move(request)));
   };

   const auto accepted = send("/secure/submit/item", R"({"id":"item"})", true);
   BOOST_TEST(accepted.result_int() == 200U);
   BOOST_TEST(accepted.body().find("item:server-owner") != std::string::npos);
   BOOST_TEST(accepted.body().find("forged") == std::string::npos);
   const auto bodyless = send("/secure/submit/path-only", "", true);
   BOOST_TEST(bodyless.result_int() == 200U);
   BOOST_TEST(bodyless.body().find("path-only:server-owner") != std::string::npos);
   const auto legacy = send("/legacy/submit/anonymous", R"({"id":"anonymous","caller":{"id":"forged"}})", false);
   BOOST_TEST(legacy.result_int() == 200U);
   BOOST_TEST(legacy.body().find("anonymous:") != std::string::npos);
   const auto legacy_bodyless = send("/legacy/submit/anonymous", "", false);
   BOOST_TEST(legacy_bodyless.result_int() == 422U);
   const auto wrapped = send("/secure/wrapped", R"({"caller":"business-value"})", true);
   BOOST_TEST(wrapped.result_int() == 200U);
   BOOST_TEST(wrapped.body().find("server-owner:business-value") != std::string::npos);
   auto query_request = forge::net::http::request{forge::net::http::method::get, "/secure/query?label=query-value", 11};
   query_request.set("X-Test-Admission", "accepted");
   const auto query = forge::asio::blocking::run(app->runtime(), client.async_request(std::move(query_request)));
   BOOST_TEST(query.result_int() == 200U);
   BOOST_TEST(query.body().find("server-owner:query-value") != std::string::npos);
   auto form_request = forge::net::http::request{forge::net::http::method::post, "/secure/form", 11};
   const auto multipart = forge::net::http::write_multipart_form({{
       .name = "label",
       .body = "multipart-value",
   }});
   form_request.set("X-Test-Admission", "accepted");
   form_request.set(forge::net::http::field::content_type, multipart.content_type);
   form_request.body() = multipart.body;
   form_request.prepare_payload();
   const auto form = forge::asio::blocking::run(app->runtime(), client.async_request(std::move(form_request)));
   BOOST_TEST(form.result_int() == 200U);
   BOOST_TEST(form.body().find("server-owner:multipart-value") != std::string::npos);
   const auto calls = state->handlers.load();
   for (const auto body : {R"({"id":"item","caller":{"id":"forged"}})", "{}", R"({"id":"item","unknown":1})",
                           R"({"id":1})", R"({"id":"item","id":"item"})"}) {
      const auto invalid = send("/secure/submit/item", body, true);
      BOOST_TEST(invalid.result_int() == 422U);
   }
   for (const auto body : {R"({"id":"item"})", "{"}) {
      const auto denied = send("/secure/submit/item", body, false);
      BOOST_TEST(denied.result_int() == 403U);
      BOOST_TEST(denied.body() == "external-denied");
      BOOST_TEST(denied.body().find("private") == std::string::npos);
   }
   const auto stream = forge::asio::blocking::run(app->runtime(), client.async_get("/secure/download/item"));
   BOOST_TEST(stream.result_int() == 403U);
   BOOST_TEST(stream.body() == "external-denied");
   const auto before_continue = forge::asio::blocking::run(app->runtime(), fixture::headers_only(port));
   BOOST_TEST(before_continue == 403U);
   BOOST_TEST(state->handlers.load() == calls);
   BOOST_TEST(state->response_reads.load() == 0U);
   BOOST_TEST(state->descriptor_seen.load());
   BOOST_TEST(state->admissions.load() == 14U);
   auto upload_request = forge::net::http::request{forge::net::http::method::put, "/secure/upload", 11};
   upload_request.set("X-Test-Admission", "accepted");
   upload_request.set(forge::net::http::field::content_type, "application/octet-stream");
   upload_request.body() = "uploaded";
   upload_request.prepare_payload();
   const auto upload = forge::asio::blocking::run(app->runtime(), client.async_request(std::move(upload_request)));
   BOOST_TEST(upload.result_int() == 200U);
   BOOST_TEST(upload.body().find("server-owner:uploaded") != std::string::npos);
   auto download_request = forge::net::http::request{forge::net::http::method::get, "/secure/download/item", 11};
   download_request.set("X-Test-Admission", "accepted");
   const auto download = forge::asio::blocking::run(app->runtime(), client.async_request(std::move(download_request)));
   BOOST_TEST(download.result_int() == 200U);
   BOOST_TEST(download.body() == "server-owner");
   BOOST_TEST(state->response_reads.load() == 2U);
   BOOST_TEST(state->admissions.load() == 16U);
   const auto before_numeric = state->handlers.load();
   for (const auto body : {R"({"count":0.0,"selected":[0,10],"wide":10,"flag":true})",
                           R"({"count":0,"selected":[0.0,10],"wide":10,"flag":true})",
                           R"({"count":128,"selected":[0,10],"wide":10,"flag":true})",
                           R"({"count":0,"selected":[0,10],"wide":10,"flag":true,"extra":1})",
                           R"({"selected":[0,10],"wide":10,"flag":true})"}) {
      BOOST_TEST(send("/secure/numeric", body, true).result_int() == 422U);
   }
   // Keep all other required fields present so each case isolates scalar/array integer fidelity or exact validation.
   for (const auto body :
        {R"({"total":0.0,"choice":[0,10],"wide":10,"flag":true})",
         R"({"total":0,"choice":[0.0,10],"wide":10,"flag":true})",
         R"({"total":101,"choice":[0,10],"wide":10,"flag":true})",
         R"({"total":0,"choice":[0,10],"wide":10,"flag":true,"extra":1})",
         R"({"choice":[0,10],"wide":10,"flag":true})",
         R"({"total":0,"total":0,"choice":[0,10],"wide":10,"flag":true})"}) {
      BOOST_TEST(send("/secure/schema-numeric", body, true).result_int() == 422U);
   }
   const auto missing_flag = send("/secure/schema-numeric", R"({"total":0,"choice":[0,10],"wide":10})", true);
   BOOST_TEST(missing_flag.result_int() == 422U);
   BOOST_TEST(missing_flag.body().find("flag") != std::string::npos);
   BOOST_TEST(state->handlers.load() == before_numeric);
   BOOST_TEST(send("/secure/numeric", R"({"count":0,"selected":[0,10],"wide":18446744073709551615,"flag":true})", true)
                  .result_int() == 200U);
   const auto schema_numeric = send(
       "/secure/schema-numeric", R"({"total":0,"legacy-choice":[0,10],"wide":18446744073709551615,"flag":true})", true);
   BOOST_TEST_CONTEXT("Schema numeric response: " << schema_numeric.body()) {
      BOOST_TEST(schema_numeric.result_int() == 200U);
   }
   BOOST_TEST(state->handlers.load() == before_numeric + 2U);
   forge::asio::blocking::run(app->runtime(), app->shutdown());
}

BOOST_AUTO_TEST_CASE(http_hidden_supplied_field_is_explicit_and_preserves_default_wire_shape) {
   namespace fixture = forge::tests::http_admission;
   auto request = fixture::input{};
   request.id = "item";
   request.caller.id = "untrusted";
   BOOST_TEST(forge::api::http::detail::encode_body_value(request, forge::api::http::body_codec::json) ==
              R"({"id":"item"})");
   auto legacy = fixture::legacy_input{};
   legacy.id = "item";
   legacy.caller.id = "untrusted";
   BOOST_TEST(forge::api::http::detail::encode_body_value(legacy, forge::api::http::body_codec::json) ==
              R"({"id":"item","caller":{"id":"untrusted"}})");
   const auto document = forge::api::http::openapi<fixture::api>();
   BOOST_TEST(!document["paths"]["/submit/{id}"]["post"].get_object().contains("requestBody"));
   const auto previous = forge::api::http::openapi<fixture::legacy_api>();
   const auto& properties =
       previous["paths"]["/submit/{id}"]["post"]["requestBody"]["content"]["application/json"]["schema"]["properties"];
   BOOST_TEST(properties.get_object().contains("caller"));
   const auto xml = forge::api::http::route{
       .verb = forge::net::http::method::post,
       .method_name = "submit",
       .target = "/submit/:id",
       .request_body_codec = forge::api::http::body_codec::xml,
   };
   BOOST_CHECK_THROW((forge::api::http::binding().route<&fixture::api::submit, fixture::input, fixture::output>(xml)),
                     forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(forge::api::http::detail::encode_body_value(request, forge::api::http::body_codec::xml),
                     forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_NO_THROW(forge::api::http::detail::encode_body_value(legacy, forge::api::http::body_codec::xml));
}

BOOST_AUTO_TEST_CASE(http_hidden_supplied_fixed_stream_metadata_does_not_consume_the_body) {
   namespace fixture = forge::tests::http_admission;
   const auto route = forge::api::http::route{
       .verb = forge::net::http::method::post,
       .method_name = "upload",
       .target = "/upload/:id",
   };
   BOOST_CHECK_NO_THROW(
       (forge::api::http::detail::validate_live_stream_route<&fixture::client_stream_api::upload, fixture::input>(
           route)));
   BOOST_CHECK_THROW((forge::api::http::detail::validate_live_stream_route<&fixture::legacy_client_stream_api::upload,
                                                                           fixture::legacy_input>(route)),
                     forge::net::http::exceptions::bad_request);
   auto forbidden = route;
   forbidden.target = "/upload/:caller";
   BOOST_CHECK_THROW(
       (forge::api::http::detail::validate_server_supplied_route<&fixture::api::submit, fixture::input>(forbidden)),
       forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_hidden_supplied_field_uses_schema_names_and_aliases_consistently) {
   namespace fixture = forge::tests::http_admission;
   const auto schema = forge::api::http::detail::request_json_schema<fixture::renamed_input>();
   BOOST_TEST(!schema["properties"].get_object().contains("native-facts"));
   BOOST_TEST(schema["properties"].get_object().contains("id"));
   BOOST_TEST(schema["required"].get_array().size() == 1U);
   BOOST_TEST(schema["required"].get_array().front().as_string() == "id");
   for (const auto* name : {"native-facts", "old-facts"}) {
      auto value = forge::variant{forge::mutable_variant_object{}("id", "item")(name, forge::mutable_variant_object{})};
      BOOST_CHECK_THROW(forge::api::http::detail::project_http_json_fields<fixture::renamed_input>(value, true),
                        forge::net::http::exceptions::bad_request);
   }
   auto wrapped = fixture::wrapped_input{};
   wrapped.data = {.value = {.caller = "business-value"}, .present = true};
   wrapped.caller.id = "untrusted";
   const auto body = forge::api::http::detail::dto_codec_body(wrapped, true, forge::api::http::body_codec::json);
   BOOST_REQUIRE(body.has_value());
   BOOST_TEST(*body == R"({"caller":"business-value"})");
}

BOOST_AUTO_TEST_CASE(http_hidden_supplied_field_scrubs_generated_member_names_without_custom_schema) {
   namespace fixture = forge::tests::http_admission;
   const auto schema = forge::api::http::detail::request_json_schema<fixture::schema_numeric_input>();
   BOOST_TEST(!schema["properties"].get_object().contains("caller"));
   BOOST_TEST(!schema["properties"].get_object().contains("native-facts"));
   for (const auto& name : schema["required"].get_array()) {
      BOOST_TEST(name.as_string() != "caller");
      BOOST_TEST(name.as_string() != "native-facts");
   }
   auto value = fixture::schema_numeric_input{};
   value.count = 10;
   value.selected = {1, 10};
   value.wide = UINT64_MAX;
   value.maybe = 42;
   value.caller.id = "private";
   const auto encoded = forge::api::http::detail::encode_body_value(value, forge::api::http::body_codec::json);
   const auto previous = forge::codec::json::write(value);
   BOOST_REQUIRE(previous.ok());
   auto expected = forge::codec::json::read_value(previous.text);
   BOOST_REQUIRE(expected.ok());
   forge::api::http::detail::project_http_json_fields<fixture::schema_numeric_input>(expected.value, false);
   BOOST_TEST(encoded == forge::codec::json::write_value(expected.value).text);
   BOOST_TEST(encoded.find("private") == std::string::npos);
   BOOST_TEST(encoded.find("native-facts") == std::string::npos);
   BOOST_TEST(encoded.find(R"("total":10)") != std::string::npos);
   BOOST_TEST(encoded.find(R"("choice":[1,10])") != std::string::npos);
   BOOST_TEST(encoded.find("18446744073709551615") != std::string::npos);
   auto plain = fixture::numeric_input{};
   plain.selected = std::int8_t{10};
   plain.wide = UINT64_MAX;
   plain.caller.id = "private";
   BOOST_TEST(forge::api::http::detail::encode_body_value(plain, forge::api::http::body_codec::json) ==
              R"({"count":0,"selected":[0,10],"wide":18446744073709551615,"flag":true})");
}

BOOST_AUTO_TEST_CASE(http_hidden_supplied_field_rejects_query_bindings_at_server_construction) {
   namespace fixture = forge::tests::http_admission;
   const auto route = forge::api::http::route{
       .verb = forge::net::http::method::post,
       .method_name = "submit",
       .target = "/submit/:id?native={caller}",
   };
   BOOST_CHECK_THROW((forge::api::http::binding().route<&fixture::api::submit, fixture::input, fixture::output>(route)),
                     forge::net::http::exceptions::bad_request);
   BOOST_CHECK_THROW((forge::api::http::binding().post<&fixture::api::submit, fixture::input, fixture::output>(
                         "/submit/:id", {.query = {{.field = "caller", .name = "native"}}})),
                     forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_hidden_supplied_field_rejects_unsupported_wrappers_and_schema_paths) {
   namespace fixture = forge::tests::http_admission;
   namespace http = forge::api::http;
   BOOST_CHECK_THROW(
       (http::binding().post<&fixture::unsupported_api::body, fixture::unsupported_body_input, fixture::output>(
           "/body")),
       forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(http::detail::request_json_schema<fixture::unsupported_body_input>(),
                     forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(
       (http::binding().post<&fixture::unsupported_api::dotted, fixture::dotted_input, fixture::output>("/dotted")),
       forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(http::detail::request_json_schema<fixture::dotted_input>(),
                     forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(
       (http::binding().post<&fixture::unsupported_api::dotted_alias, fixture::dotted_alias_input, fixture::output>(
           "/dotted-alias")),
       forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(http::detail::encode_body_value(fixture::dotted_alias_input{}, http::body_codec::json),
                     forge::api::core::exceptions::incompatible_version);
   using wrapped = std::tuple<http::header<std::string>, http::body<fixture::input>>;
   const auto route =
       http::route{.verb = forge::net::http::method::post, .method_name = "positional_body", .target = "/body"};
   BOOST_CHECK_THROW((http::binding().route<&fixture::unsupported_api::positional_body, wrapped, void>(route)),
                     forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(
       (http::detail::make_openapi_operation<&fixture::unsupported_api::positional_body, wrapped, void>(route)),
       forge::api::core::exceptions::incompatible_version);
   using plain = std::tuple<std::string, fixture::input>;
   const auto xml = http::route{.verb = forge::net::http::method::post,
                                .method_name = "positional_plain",
                                .target = "/body/:id",
                                .request_body_codec = http::body_codec::xml};
   BOOST_CHECK_THROW((http::binding().route<&fixture::unsupported_api::positional_plain, plain, void>(xml)),
                     forge::api::core::exceptions::incompatible_version);
   BOOST_CHECK_THROW(
       (http::detail::make_openapi_operation<&fixture::unsupported_api::positional_plain, plain, void>(xml)),
       forge::api::core::exceptions::incompatible_version);
}
