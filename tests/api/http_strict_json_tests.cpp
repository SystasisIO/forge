#include <boost/asio/awaitable.hpp>
#include <boost/describe.hpp>
#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

import forge.api.core.binding;
import forge.api.core.descriptor;
import forge.api.core.exceptions;
import forge.api.core.registry;
import forge.api.core.stream_writer;
import forge.api.core.types;
import forge.api.http.binding;
import forge.api.http.parameters;
import forge.api.http.proxy;
import forge.asio.blocking;
import forge.asio.runtime;
import forge.codec.json;
import forge.net.http.base_url;
import forge.net.http.client;
import forge.net.http.connection;
import forge.net.http.exceptions;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.types;
import forge.net.transport.frame;
import forge.raw.raw;
import forge.schema.object;
import forge.variant.containers;
import forge.variant.conversion;
import forge.variant.described;
import forge.variant.static_variant;

namespace forge::tests::http_strict_json {

using choice = std::variant<std::int8_t, std::string>;

struct child {
   std::int8_t count = 0;
   std::optional<std::uint64_t> maybe;
};

struct payload {
   std::int8_t count = 0;
   std::uint64_t wide = 0;
   child nested;
   choice selected;
   std::optional<std::uint64_t> maybe;
};

struct body_input {
   forge::api::http::body<payload> document;
};

struct boolean_input {
   bool value = false;
};

struct route_input {
   std::string tag;
   std::int8_t count = 0;
   std::uint64_t wide = 0;
};

struct optional_input {
   std::optional<std::int8_t> count;
   std::optional<std::uint64_t> wide;
};

struct route_default_input {
   std::string tag;
   int limit = 7;
};

struct required_optional_input {
   std::optional<std::int8_t> member;
};

struct reply {
   payload value;
   std::string tag;
   bool present = false;
};

struct response_request {
   std::uint32_t timeout_ms = 0;
};

class api
    : public forge::api::core::contract<api, forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~api() = default;
   virtual boost::asio::awaitable<reply> typed(payload value) = 0;
   virtual boost::asio::awaitable<reply> positional(std::string tag, payload value) = 0;
   virtual boost::asio::awaitable<reply> body_dto(body_input value) = 0;
   virtual boost::asio::awaitable<route_input> route_only(route_input value) = 0;
   virtual boost::asio::awaitable<optional_input> optional_only(optional_input value) = 0;
   virtual boost::asio::awaitable<route_default_input> route_default(route_default_input value) = 0;
   virtual boost::asio::awaitable<required_optional_input> required_optional(required_optional_input value) = 0;
   virtual boost::asio::awaitable<bool> boolean(boolean_input value) = 0;
   virtual boost::asio::awaitable<void>
   stream(std::string tag, payload value, forge::api::core::stream_writer<reply> output) = 0;
};

class response_api
    : public forge::api::core::contract<response_api,
                                       forge::api::core::surface::local | forge::api::core::surface::remote> {
 public:
   virtual ~response_api() = default;
   virtual boost::asio::awaitable<reply> read(response_request value) = 0;
};

class implementation final : public api {
 public:
   boost::asio::awaitable<reply> typed(payload value) override;
   boost::asio::awaitable<reply> positional(std::string tag, payload value) override;
   boost::asio::awaitable<reply> body_dto(body_input value) override;
   boost::asio::awaitable<route_input> route_only(route_input value) override;
   boost::asio::awaitable<optional_input> optional_only(optional_input value) override;
   boost::asio::awaitable<route_default_input> route_default(route_default_input value) override;
   boost::asio::awaitable<required_optional_input> required_optional(required_optional_input value) override;
   boost::asio::awaitable<bool> boolean(boolean_input value) override;
   boost::asio::awaitable<void>
   stream(std::string tag, payload value, forge::api::core::stream_writer<reply> output) override;

   std::atomic_uint calls{0};
};

struct scripted_response_state {
   std::mutex mutex;
   std::string json;
   forge::net::http::status status = forge::net::http::status::ok;
   std::atomic_uint calls{0};
};

struct route_case {
   const char* path;
   const char* tag;
   bool streaming = false;
};

struct json_case {
   const char* name;
   std::string json;
};

struct accepted_case {
   const char* name;
   std::string json;
   payload expected;
};

struct fixture {
   fixture();
   ~fixture();

   forge::net::http::response post(const route_case& route, std::string json);
   void set_response(std::string json, forge::net::http::status status = forge::net::http::status::ok);

   forge::asio::runtime runtime{forge::asio::runtime_options{.worker_threads = 2}};
   forge::api::core::registry apis;
   std::shared_ptr<implementation> service = std::make_shared<implementation>();
   std::shared_ptr<scripted_response_state> scripted = std::make_shared<scripted_response_state>();
   std::unique_ptr<forge::net::http::server> server;
   std::unique_ptr<forge::net::http::client> client;
};

BOOST_DESCRIBE_STRUCT(child, (), (count, maybe))
BOOST_DESCRIBE_STRUCT(payload, (), (count, wide, nested, selected, maybe))
BOOST_DESCRIBE_STRUCT(body_input, (), (document))
BOOST_DESCRIBE_STRUCT(boolean_input, (), (value))
BOOST_DESCRIBE_STRUCT(route_input, (), (tag, count, wide))
BOOST_DESCRIBE_STRUCT(optional_input, (), (count, wide))
BOOST_DESCRIBE_STRUCT(route_default_input, (), (tag, limit))
BOOST_DESCRIBE_STRUCT(required_optional_input, (), (member))
BOOST_DESCRIBE_STRUCT(reply, (), (value, tag, present))
BOOST_DESCRIBE_STRUCT(response_request, (), (timeout_ms))

} // namespace forge::tests::http_strict_json

namespace forge::schema {

template <> struct rules<forge::tests::http_strict_json::required_optional_input> {
   static object_schema<forge::tests::http_strict_json::required_optional_input> define() {
      auto schema = object<forge::tests::http_strict_json::required_optional_input>();
      schema.field<&forge::tests::http_strict_json::required_optional_input::member>("member").required();
      return schema;
   }
};

} // namespace forge::schema

FORGE_API(::forge::tests::http_strict_json::api, FORGE_API_CONTRACT("http-strict-json", 1, 0),
          FORGE_API_METHOD_TYPED(typed, ::forge::tests::http_strict_json::payload,
                                 ::forge::tests::http_strict_json::reply),
          FORGE_API_METHOD(positional, tag, value),
          FORGE_API_METHOD_TYPED(body_dto, ::forge::tests::http_strict_json::body_input,
                                 ::forge::tests::http_strict_json::reply),
          FORGE_API_METHOD_TYPED(route_only, ::forge::tests::http_strict_json::route_input,
                                 ::forge::tests::http_strict_json::route_input),
          FORGE_API_METHOD_TYPED(optional_only, ::forge::tests::http_strict_json::optional_input,
                                 ::forge::tests::http_strict_json::optional_input),
          FORGE_API_METHOD_TYPED(route_default, ::forge::tests::http_strict_json::route_default_input,
                                 ::forge::tests::http_strict_json::route_default_input),
          FORGE_API_METHOD_TYPED(required_optional, ::forge::tests::http_strict_json::required_optional_input,
                                 ::forge::tests::http_strict_json::required_optional_input),
          FORGE_API_METHOD_TYPED(boolean, ::forge::tests::http_strict_json::boolean_input, bool),
          FORGE_API_METHOD(stream, tag, value))
FORGE_HTTP_API(::forge::tests::http_strict_json::api,
               FORGE_HTTP_POST(typed, "/strict/typed", ok),
               FORGE_HTTP_POST(positional, "/strict/positional/:tag", ok),
               FORGE_HTTP_POST(body_dto, "/strict/body-dto", ok),
               FORGE_HTTP_POST(route_only, "/strict/route-only/:tag?count={count}&wide={wide}", ok),
               FORGE_HTTP_POST(optional_only, "/strict/optional-only", ok),
               FORGE_HTTP_POST(route_default, "/strict/route-default/:tag?limit={limit}", ok),
               FORGE_HTTP_POST(required_optional, "/strict/required-optional", ok),
               FORGE_HTTP_POST(boolean, "/strict/boolean", ok), FORGE_HTTP_POST(stream, "/strict/stream/:tag", ok))

FORGE_API(::forge::tests::http_strict_json::response_api, FORGE_API_CONTRACT("http-strict-json-response", 1, 0),
          FORGE_API_METHOD_TYPED(read, ::forge::tests::http_strict_json::response_request,
                                 ::forge::tests::http_strict_json::reply))
// The proxy's public timeout mapping bounds these calls to the five-second grace, without sleeps or private hooks.
FORGE_HTTP_API(::forge::tests::http_strict_json::response_api,
               FORGE_HTTP_POST(read, "/strict/response", ok, FORGE_HTTP_TIMEOUT(timeout_ms)))

namespace forge::tests::http_strict_json {

constexpr auto routes = std::array{
   route_case{"/strict/typed", "typed"},
   route_case{"/strict/positional/probe", "probe"},
   route_case{"/strict/body-dto", "body-dto"},
   route_case{"/strict/stream/probe", "probe", true},
};

boost::asio::awaitable<reply> implementation::typed(payload value) {
   ++calls;
   co_return reply{std::move(value), "typed", true};
}

boost::asio::awaitable<reply> implementation::positional(std::string tag, payload value) {
   ++calls;
   co_return reply{std::move(value), std::move(tag), true};
}

boost::asio::awaitable<reply> implementation::body_dto(body_input value) {
   ++calls;
   co_return reply{std::move(value.document.value), "body-dto", value.document.present};
}

boost::asio::awaitable<route_input> implementation::route_only(route_input value) {
   ++calls;
   co_return value;
}

boost::asio::awaitable<optional_input> implementation::optional_only(optional_input value) {
   ++calls;
   co_return value;
}

boost::asio::awaitable<route_default_input> implementation::route_default(route_default_input value) {
   ++calls;
   co_return value;
}

boost::asio::awaitable<required_optional_input> implementation::required_optional(required_optional_input value) {
   ++calls;
   co_return value;
}

boost::asio::awaitable<bool> implementation::boolean(boolean_input value) {
   ++calls;
   co_return value.value;
}

boost::asio::awaitable<void>
implementation::stream(std::string tag, payload value, forge::api::core::stream_writer<reply> output) {
   ++calls;
   co_await output.async_write(reply{std::move(value), std::move(tag), true});
   co_await output.async_close();
}

fixture::fixture() {
   apis.install<api>(api::describe(), service);
   auto router = forge::net::http::router{};
   router.mount(forge::api::http::binding().use(forge::api::core::binding().serve(apis).build()).bind<api>().build());
   router.post("/strict/response", [state = scripted](forge::net::http::route_context& context)
                   -> boost::asio::awaitable<forge::net::http::response> {
      auto json = std::string{};
      auto status = forge::net::http::status::ok;
      {
         const auto lock = std::lock_guard{state->mutex};
         json = state->json;
         status = state->status;
      }
      ++state->calls;
      co_return forge::net::http::make_text_response(context.request, status, json, "application/json");
   });
   server = std::make_unique<forge::net::http::server>(
      runtime, forge::net::http::server_config{.bind_address = "127.0.0.1", .port = 0,
                                              .read_timeout = std::chrono::seconds{2},
                                              .idle_timeout = std::chrono::seconds{5}},
      std::move(router));
   server->start();
   client = std::make_unique<forge::net::http::client>(
      runtime, forge::net::http::parse_base_url("http://127.0.0.1:" + std::to_string(server->port())));
}

fixture::~fixture() {
   server->stop();
}

forge::net::http::response fixture::post(const route_case& route, std::string json) {
   auto request = forge::net::http::request{forge::net::http::method::post, route.path, 11};
   request.set(forge::net::http::field::content_type, "application/json");
   request.set(forge::net::http::field::accept,
               route.streaming ? "application/vnd.forge.api-stream" : "application/json");
   request.body() = std::move(json);
   request.prepare_payload();
   return forge::asio::blocking::run(
      runtime, client->async_request(std::move(request),
         forge::net::http::request_options{.timeout = std::chrono::seconds{2},
                                           .deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2}}));
}

void fixture::set_response(std::string json, forge::net::http::status status) {
   const auto lock = std::lock_guard{scripted->mutex};
   scripted->json = std::move(json);
   scripted->status = status;
}

std::string document(std::string_view count = "7", std::string_view wide = "11",
                      std::string_view nested = R"({"count":9})", std::string_view selected = "[0,10]",
                      std::string_view extra_members = "") {
   return "{\"count\":" + std::string{count} + ",\"wide\":" + std::string{wide} +
          ",\"nested\":" + std::string{nested} + ",\"selected\":" + std::string{selected} +
          std::string{extra_members} + "}";
}

std::vector<json_case> invalid_integers() {
   auto cases = std::vector<json_case>{};
   for (const auto* token : {"128", "-129", "\"7\"", "7.5", "7.0", "null", "true", "false"}) {
      cases.push_back({token, document(token)});
      cases.push_back({token, document("7", "11", "{\"count\":" + std::string{token} + "}")});
   }
   for (const auto* token : {"-1", "18446744073709551616", "\"11\"", "11.5", "null", "true", "false"}) {
      cases.push_back({token, document("7", token)});
   }
   cases.push_back({"quoted optional", document("7", "11", R"({"count":9,"maybe":"11"})")});
   cases.push_back({"boolean optional", document("7", "11", R"({"count":9})", "[0,10]", R"(,"maybe":true)")});
   return cases;
}

std::vector<json_case> invalid_records() {
   return {
      {"missing required root fields", "{}"},
      {"missing required count", R"({"wide":11,"nested":{"count":9},"selected":[0,10]})"},
      {"missing required nested object", R"({"count":7,"wide":11,"selected":[0,10]})"},
      {"missing required nested field", document("7", "11", "{}")},
      {"missing required variant", R"({"count":7,"wide":11,"nested":{"count":9}})"},
      {"unknown root field", document("7", "11", R"({"count":9})", "[0,10]", R"(,"extra":0)")},
      {"unknown nested field", document("7", "11", R"({"count":9,"extra":0})")},
      {"null required nested object", document("7", "11", "null")},
   };
}

std::vector<json_case> invalid_variants() {
   auto cases = std::vector<json_case>{};
   for (const auto* token : {"[]", "[0]", "[1]", "[0,10,11]", R"([1,"text",11])",
                             "[0.5,10]", "[-0.5,10]", "[0.0,10]", R"([1.0,"text"])",
                             R"(["0",10])", "[null,10]", "[false,10]", R"([true,"text"])",
                             "[-1,10]", "[2,10]", "[18446744073709551615,10]",
                             R"([0,"text"])", "[1,10]", "[0,128]", "[0,-129]", "[0,10.5]",
                             "[0,null]", "[0,true]", R"({"index":0,"value":10})"}) {
      cases.push_back({token, document("7", "11", R"({"count":9})", token)});
   }
   return cases;
}

std::vector<accepted_case> accepted_documents() {
   constexpr auto maximum = (std::numeric_limits<std::uint64_t>::max)();
   return {
      {"minimum int8 and uint64, optionals absent",
       document("-128", "0", R"({"count":-128})", "[0,-128]"),
       payload{-128, 0, child{-128, std::nullopt}, choice{std::in_place_index<0>, -128}, std::nullopt}},
      {"maximum integers and null optionals",
       document("127", "18446744073709551615", R"({"count":127,"maybe":null})", "[0,127]", R"(,"maybe":null)"),
       payload{127, maximum, child{127, std::nullopt}, choice{std::in_place_index<0>, 127}, std::nullopt}},
      {"string variant and exact uint64 optional values",
       document("0", "9007199254740993", R"({"count":0,"maybe":18446744073709551615})",
                 R"([1,"text"])", R"(,"maybe":9007199254740993)"),
       payload{0, 9007199254740993ULL, child{0, maximum}, choice{std::in_place_index<1>, "text"},
               9007199254740993ULL}},
   };
}

void check_optional(const std::optional<std::uint64_t>& actual, const std::optional<std::uint64_t>& expected) {
   BOOST_REQUIRE(actual.has_value() == expected.has_value());
   if (expected.has_value()) {
      BOOST_TEST(*actual == *expected);
   }
}

void check_payload(const payload& actual, const payload& expected) {
   BOOST_TEST(static_cast<int>(actual.count) == static_cast<int>(expected.count));
   BOOST_TEST(actual.wide == expected.wide);
   BOOST_TEST(static_cast<int>(actual.nested.count) == static_cast<int>(expected.nested.count));
   check_optional(actual.nested.maybe, expected.nested.maybe);
   check_optional(actual.maybe, expected.maybe);
   BOOST_REQUIRE(actual.selected.index() == expected.selected.index());
   if (expected.selected.index() == 0U) {
      BOOST_TEST(static_cast<int>(std::get<0>(actual.selected)) == static_cast<int>(std::get<0>(expected.selected)));
   } else {
      BOOST_TEST(std::get<1>(actual.selected) == std::get<1>(expected.selected));
   }
}

reply decode_reply(const forge::net::http::response& response, bool streaming) {
   BOOST_REQUIRE_EQUAL(response.result_int(), 200U);
   if (!streaming) {
      const auto decoded = forge::codec::json::read<reply>(
         response.body(), {.described_records = forge::codec::json::described_record_policy::exact});
      BOOST_REQUIRE_MESSAGE(decoded.ok(), "invalid success JSON: " << response.body());
      return decoded.value;
   }

   BOOST_TEST(response[forge::net::http::field::content_type] == "application/vnd.forge.api-stream; version=2");
   auto remaining = std::span<const std::uint8_t>{
      reinterpret_cast<const std::uint8_t*>(response.body().data()), response.body().size()};
   auto result = std::optional<reply>{};
   auto index = 0U;
   const auto kinds = std::array{forge::api::core::frame_kind::stream_item, forge::api::core::frame_kind::stream_end,
                                 forge::api::core::frame_kind::response};
   while (!remaining.empty()) {
      const auto packet = forge::net::transport::decode_frame(remaining);
      BOOST_REQUIRE(packet.status == forge::net::transport::frame_decode_status::complete);
      BOOST_REQUIRE_GT(packet.consumed, 0U);
      auto [version, frame] = forge::raw::unpack_exact<std::tuple<std::uint16_t, forge::api::core::frame>>(packet.payload);
      BOOST_TEST(version == 2U);
      BOOST_REQUIRE_LT(index, kinds.size());
      BOOST_CHECK(frame.kind == kinds[index]);
      if (index == 0U) {
         result = forge::raw::unpack_exact<reply>(frame.payload);
      }
      ++index;
      remaining = remaining.subspan(packet.consumed);
   }
   BOOST_TEST(index == kinds.size());
   BOOST_REQUIRE(result.has_value());
   return std::move(*result);
}

void check_route_control(fixture& value, const route_case& route) {
   BOOST_TEST_CONTEXT("success control route=" << route.path) {
      const auto witness = accepted_documents().front();
      const auto before = value.service->calls.load();
      const auto decoded = decode_reply(value.post(route, witness.json), route.streaming);
      check_payload(decoded.value, witness.expected);
      BOOST_REQUIRE_EQUAL(decoded.tag, route.tag);
      BOOST_REQUIRE(decoded.present);
      BOOST_REQUIRE_EQUAL(value.service->calls.load(), before + 1U);
   }
}

void check_rejections(const std::vector<json_case>& cases) {
   auto value = fixture{};
   for (const auto& route : routes) {
      check_route_control(value, route);
      for (const auto& entry : cases) {
         BOOST_TEST_CONTEXT("route=" << route.path << ", case=" << entry.name << ", json=" << entry.json) {
            const auto before = value.service->calls.load();
            const auto response = value.post(route, entry.json);
            BOOST_TEST(response.result_int() == 422U);
            BOOST_TEST(response.body().find("validation_error") != std::string::npos);
            BOOST_TEST(value.service->calls.load() == before);
         }
      }
      check_route_control(value, route);
   }
}

std::string response_document(std::string_view value) {
   return "{\"value\":" + std::string{value} + R"(,"tag":"response","present":true})";
}

void check_proxy_control(fixture& value, forge::api::http::proxy<response_api>& remote) {
   BOOST_TEST_CONTEXT("success control HTTP proxy") {
      const auto witness = accepted_documents().front();
      value.set_response(response_document(witness.json));
      const auto before = value.scripted->calls.load();
      const auto service_before = value.service->calls.load();
      const auto decoded = forge::asio::blocking::run(value.runtime, remote.read(response_request{}));
      check_payload(decoded.value, witness.expected);
      BOOST_REQUIRE_EQUAL(decoded.tag, "response");
      BOOST_REQUIRE(decoded.present);
      BOOST_REQUIRE_EQUAL(value.scripted->calls.load(), before + 1U);
      BOOST_REQUIRE_EQUAL(value.service->calls.load(), service_before);
   }
}

std::string error_document(std::string_view code, std::string_view extra_members = "") {
   return R"({"error":"resource_exhausted","message":"queue full","retryable":true,)"
          R"("status_code":"resource_exhausted","identity":{"category":"forge.api","code":)" +
          std::string{code} + "}" + std::string{extra_members} + "}";
}

void check_remote_identity(const forge::exceptions::base& error, forge::api::core::exceptions::code code,
                            std::string_view name) {
   BOOST_TEST(std::string{error.code().category().name()} == "forge.api");
   BOOST_TEST(error.code().value() == static_cast<int>(code));
   auto category_found = false;
   auto code_found = false;
   auto name_found = false;
   for (const auto& field : error.context()) {
      if (field.key == "remote.category") {
         category_found = true;
         BOOST_TEST(field.value == "forge.api");
      } else if (field.key == "remote.code") {
         code_found = true;
         BOOST_TEST(field.value == std::to_string(static_cast<std::uint32_t>(code)));
      } else if (field.key == "remote.error") {
         name_found = true;
         BOOST_TEST(field.value == name);
      }
   }
   BOOST_TEST(category_found);
   BOOST_TEST(code_found);
   BOOST_TEST(name_found);
}

BOOST_AUTO_TEST_SUITE(http_strict_json_loopback)

BOOST_AUTO_TEST_CASE(bool_scalar_roundtrips_through_binding_and_proxy_without_integer_coercion) {
   auto value = fixture{};
   auto remote = forge::api::http::proxy<api>{*value.client};
   const auto route = route_case{"/strict/boolean", "boolean"};
   for (const auto expected : std::array{false, true}) {
      BOOST_TEST_CONTEXT("value=" << expected) {
         const auto* literal = expected ? "true" : "false";
         const auto before = value.service->calls.load();
         const auto response = value.post(route, std::string{"{\"value\":"} + literal + '}');
         BOOST_REQUIRE_EQUAL(response.result_int(), 200U);
         BOOST_TEST(response.body() == literal);
         BOOST_TEST(value.service->calls.load() == before + 1U);

         const auto decoded = forge::asio::blocking::run(value.runtime, remote.boolean(boolean_input{expected}));
         BOOST_TEST(decoded == expected);
         BOOST_TEST(value.service->calls.load() == before + 2U);
      }
   }

   for (const auto* json : {"0", "1", "\"true\"", "null"}) {
      BOOST_TEST_CONTEXT("invalid bool=" << json) {
         const auto before = value.service->calls.load();
         const auto response = value.post(route, std::string{"{\"value\":"} + json + '}');
         BOOST_TEST(response.result_int() == 422U);
         BOOST_TEST(response.body().find("validation_error") != std::string::npos);
         BOOST_TEST(value.service->calls.load() == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(supported_routes_and_proxy_success_controls_are_usable) {
   auto value = fixture{};
   for (const auto& route : routes) {
      check_route_control(value, route);
   }
   auto remote = forge::api::http::proxy<response_api>{*value.client};
   check_proxy_control(value, remote);
}

BOOST_AUTO_TEST_CASE(rejects_out_of_range_and_coerced_integer_members_before_service_invocation) {
   check_rejections(invalid_integers());
}

BOOST_AUTO_TEST_CASE(rejects_missing_required_and_unknown_nested_fields_before_service_invocation) {
   check_rejections(invalid_records());
}

BOOST_AUTO_TEST_CASE(rejects_variant_shape_index_and_pairing_before_service_invocation) {
   check_rejections(invalid_variants());
}

BOOST_AUTO_TEST_CASE(accepts_boundaries_optionals_and_variants_and_preserves_decoded_values) {
   auto value = fixture{};
   for (const auto& route : routes) {
      for (const auto& entry : accepted_documents()) {
         BOOST_TEST_CONTEXT("route=" << route.path << ", case=" << entry.name) {
            const auto before = value.service->calls.load();
            const auto decoded = decode_reply(value.post(route, entry.json), route.streaming);
            check_payload(decoded.value, entry.expected);
            BOOST_TEST(decoded.tag == route.tag);
            BOOST_TEST(decoded.present);
            BOOST_TEST(value.service->calls.load() == before + 1U);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(empty_required_typed_and_positional_json_bodies_must_not_invoke_default_payloads) {
   auto value = fixture{};
   for (const auto& route : {routes[0], routes[1], routes[3]}) {
      BOOST_TEST_CONTEXT("required body route=" << route.path) {
         check_route_control(value, route);
         const auto before = value.service->calls.load();
         const auto response = value.post(route, "");
         BOOST_TEST(response.result_int() == 422U);
         BOOST_TEST(response.body().find("validation_error") != std::string::npos);
         BOOST_TEST(value.service->calls.load() == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(absent_explicit_body_wrappers_preserve_present_false) {
   auto value = fixture{};
   for (const auto& route : {routes[2]}) {
      BOOST_TEST_CONTEXT("optional body route=" << route.path) {
         const auto before = value.service->calls.load();
         const auto decoded = decode_reply(value.post(route, ""), false);
         BOOST_TEST(!decoded.present);
         BOOST_TEST(decoded.tag == route.tag);
         check_payload(decoded.value, payload{});
         BOOST_TEST(value.service->calls.load() == before + 1U);
      }
   }
}

BOOST_AUTO_TEST_CASE(bodyless_post_binds_all_required_dto_fields_from_path_and_query) {
   auto value = fixture{};
   const auto route = route_case{"/strict/route-only/from-path?count=-128&wide=18446744073709551615", "from-path"};
   const auto before = value.service->calls.load();
   const auto response = value.post(route, "");
   BOOST_REQUIRE_EQUAL(response.result_int(), 200U);
   const auto decoded = forge::codec::json::read<route_input>(
      response.body(), {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE_MESSAGE(decoded.ok(), "invalid route-only success JSON: " << response.body());
   BOOST_TEST(decoded.value.tag == "from-path");
   BOOST_TEST(static_cast<int>(decoded.value.count) == -128);
   BOOST_TEST(decoded.value.wide == (std::numeric_limits<std::uint64_t>::max)());
   BOOST_TEST(value.service->calls.load() == before + 1U);
}

BOOST_AUTO_TEST_CASE(bodyless_post_accepts_optional_only_dto_without_synthesizing_values) {
   auto value = fixture{};
   const auto route = route_case{"/strict/optional-only", "optional-only"};
   const auto before = value.service->calls.load();
   const auto response = value.post(route, "");
   BOOST_REQUIRE_EQUAL(response.result_int(), 200U);
   const auto decoded = forge::codec::json::read<optional_input>(
      response.body(), {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE_MESSAGE(decoded.ok(), "invalid optional-only success JSON: " << response.body());
   BOOST_TEST(!decoded.value.count.has_value());
   BOOST_TEST(!decoded.value.wide.has_value());
   BOOST_TEST(value.service->calls.load() == before + 1U);
}

BOOST_AUTO_TEST_CASE(bodyless_post_preserves_default_for_explicitly_mapped_but_absent_query_field) {
   auto value = fixture{};
   for (const auto& [path, expected] : std::array{
           std::pair{"/strict/route-default/from-path?limit=11", 11},
           std::pair{"/strict/route-default/from-path", 7}}) {
      BOOST_TEST_CONTEXT("path=" << path) {
         const auto before = value.service->calls.load();
         const auto response = value.post(route_case{path, "from-path"}, "");
         BOOST_REQUIRE_EQUAL(response.result_int(), 200U);
         const auto decoded = forge::codec::json::read<route_default_input>(
            response.body(), {.described_records = forge::codec::json::described_record_policy::exact});
         BOOST_REQUIRE_MESSAGE(decoded.ok(), "invalid route-default success JSON: " << response.body());
         BOOST_TEST(decoded.value.tag == "from-path");
         BOOST_TEST(decoded.value.limit == expected);
         BOOST_TEST(value.service->calls.load() == before + 1U);
      }
   }
}

BOOST_AUTO_TEST_CASE(schema_required_optional_rejects_empty_and_missing_body_members_before_invocation) {
   auto value = fixture{};
   const auto route = route_case{"/strict/required-optional", "required-optional"};
   const auto before_control = value.service->calls.load();
   const auto control = value.post(route, R"({"member":7})");
   BOOST_REQUIRE_EQUAL(control.result_int(), 200U);
   const auto decoded = forge::codec::json::read<required_optional_input>(
      control.body(), {.described_records = forge::codec::json::described_record_policy::exact});
   BOOST_REQUIRE_MESSAGE(decoded.ok(), "invalid required-optional success JSON: " << control.body());
   BOOST_REQUIRE(decoded.value.member.has_value());
   BOOST_TEST(static_cast<int>(*decoded.value.member) == 7);
   BOOST_REQUIRE_EQUAL(value.service->calls.load(), before_control + 1U);

   for (const auto* body : {"", "{}"}) {
      BOOST_TEST_CONTEXT("missing required optional body=" << body) {
         const auto before = value.service->calls.load();
         const auto response = value.post(route, body);
         BOOST_TEST(response.result_int() == 422U);
         BOOST_TEST(response.body().find("validation_error") != std::string::npos);
         BOOST_TEST(value.service->calls.load() == before);
      }
   }
}

BOOST_AUTO_TEST_CASE(http_proxy_rejects_malformed_success_json_instead_of_coercing_it) {
   auto value = fixture{};
   auto remote = forge::api::http::proxy<response_api>{*value.client};
   check_proxy_control(value, remote);
   auto cases = invalid_integers();
   const auto records = invalid_records();
   const auto variants = invalid_variants();
   cases.insert(cases.end(), records.begin(), records.end());
   cases.insert(cases.end(), variants.begin(), variants.end());
   for (auto& entry : cases) {
      entry.json = response_document(entry.json);
   }
   cases.push_back({"missing success response fields", "{}"});
   cases.push_back({"empty success body", ""});
   cases.push_back({"syntactically invalid success body", "{"});
   cases.push_back({"unknown success response field",
                    "{\"value\":" + document() + R"(,"tag":"response","present":true,"extra":0})"});

   for (const auto& entry : cases) {
      BOOST_TEST_CONTEXT("case=" << entry.name << ", json=" << entry.json) {
         value.set_response(entry.json);
         const auto before = value.scripted->calls.load();
         BOOST_CHECK_THROW(forge::asio::blocking::run(value.runtime, remote.read(response_request{})),
                            forge::net::http::exceptions::bad_request);
         BOOST_TEST(value.scripted->calls.load() == before + 1U);
         BOOST_TEST(value.service->calls.load() == 0U);
      }
   }
}

BOOST_AUTO_TEST_CASE(http_proxy_accepts_canonical_success_json_without_losing_integer_precision) {
   auto value = fixture{};
   auto remote = forge::api::http::proxy<response_api>{*value.client};
   for (const auto& entry : accepted_documents()) {
      BOOST_TEST_CONTEXT("case=" << entry.name) {
         value.set_response(response_document(entry.json));
         const auto before = value.scripted->calls.load();
         const auto decoded = forge::asio::blocking::run(value.runtime, remote.read(response_request{}));
         check_payload(decoded.value, entry.expected);
         BOOST_TEST(decoded.tag == "response");
         BOOST_TEST(decoded.present);
         BOOST_TEST(value.scripted->calls.load() == before + 1U);
      }
   }
}

BOOST_AUTO_TEST_CASE(http_proxy_uses_generic_error_fallback_without_fabricating_typed_identity) {
   auto value = fixture{};
   auto remote = forge::api::http::proxy<response_api>{*value.client};
   check_proxy_control(value, remote);
   auto cases = std::vector<json_case>{};
   for (const auto* token : {"4294967304", "\"8\"", "8.5", "null", "true", "false"}) {
      cases.push_back({token, error_document(token)});
   }
   constexpr auto secret = std::string_view{"http-strict-json-secret"};
   cases.push_back({"sensitive unknown field",
                    error_document("8", R"(,"provider_token":"http-strict-json-secret")")});
   cases.push_back({"non-JSON error body", "provider_token=http-strict-json-secret"});
   for (const auto& entry : cases) {
      BOOST_TEST_CONTEXT("error fallback case=" << entry.name) {
         value.set_response(entry.json, forge::net::http::status::too_many_requests);
         const auto before = value.scripted->calls.load();
         auto fallback = false;
         try {
            static_cast<void>(forge::asio::blocking::run(value.runtime, remote.read(response_request{})));
            BOOST_ERROR("an HTTP error response must not return a success value");
         } catch (const forge::api::core::exceptions::remote_internal& error) {
            fallback = true;
            check_remote_identity(error, forge::api::core::exceptions::code::remote_internal, "http_error");
            BOOST_TEST(error.message() == "HTTP API request failed");
            BOOST_TEST(std::string{error.what()}.find(secret) == std::string::npos);
            for (const auto& field : error.context()) {
               BOOST_TEST(field.value.find(entry.json) == std::string::npos);
               BOOST_TEST(field.value.find(secret) == std::string::npos);
            }
            for (const auto& frame : error.context_frames()) {
               BOOST_TEST(frame.message.find(entry.json) == std::string::npos);
               BOOST_TEST(frame.message.find(secret) == std::string::npos);
               for (const auto& field : frame.context) {
                  BOOST_TEST(field.value.find(entry.json) == std::string::npos);
                  BOOST_TEST(field.value.find(secret) == std::string::npos);
               }
            }
         } catch (const forge::exceptions::base& error) {
            BOOST_ERROR("malformed identity produced a typed exception: " << error.code());
         }
         BOOST_TEST(fallback);
         BOOST_TEST(value.scripted->calls.load() == before + 1U);
         BOOST_TEST(value.service->calls.load() == 0U);
      }
   }
}

BOOST_AUTO_TEST_CASE(http_proxy_preserves_valid_forge_error_identity) {
   auto value = fixture{};
   auto remote = forge::api::http::proxy<response_api>{*value.client};
   value.set_response(error_document("8"), forge::net::http::status::too_many_requests);
   BOOST_CHECK_EXCEPTION(
      forge::asio::blocking::run(value.runtime, remote.read(response_request{})),
      forge::api::core::exceptions::resource_exhausted,
      [](const forge::api::core::exceptions::resource_exhausted& error) {
         check_remote_identity(error, forge::api::core::exceptions::code::resource_exhausted, "resource_exhausted");
         return error.message() == "queue full";
      });
   BOOST_TEST(value.scripted->calls.load() == 1U);
   BOOST_TEST(value.service->calls.load() == 0U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::tests::http_strict_json
