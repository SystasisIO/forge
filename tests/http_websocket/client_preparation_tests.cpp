#include <boost/test/unit_test.hpp>
#include <forge/api/core/macros.hpp>
#include <forge/api/http/macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read_until.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/streambuf.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/system/system_error.hpp>

import forge.api.core.connection;
import forge.api.core.exceptions;
import forge.api.core.handle;
import forge.api.core.stream_reader;
import forge.api.core.stream_writer;
import forge.api.http.binding;
import forge.api.http.openapi;
import forge.api.http.proxy;
import forge.asio.blocking;
import forge.asio.exceptions;
import forge.asio.runtime;
import forge.net.http.base_url;
import forge.net.http.body;
import forge.net.http.client;
import forge.net.http.connection;
import forge.net.http.exceptions;
import forge.net.http.route_context;
import forge.net.http.server;
import forge.net.http.router;
import forge.net.http.stream;
import forge.net.http.types;

namespace forge::net::http::preparation_test {

class probe_api : public forge::api::core::contract<probe_api, forge::api::core::surface::local |
                                                               forge::api::core::surface::remote> {
 public:
   virtual ~probe_api() = default;
   virtual boost::asio::awaitable<std::string> read(std::string ref) = 0;
   virtual boost::asio::awaitable<std::string> write(std::string ref) = 0;
   virtual boost::asio::awaitable<void> download(std::string ref,
                                                 forge::api::core::stream_writer<std::string> output) = 0;
   virtual boost::asio::awaitable<std::string> upload(std::string ref,
                                                      forge::api::core::stream_reader<std::string> input) = 0;
};

} // namespace forge::net::http::preparation_test

FORGE_API(::forge::net::http::preparation_test::probe_api, FORGE_API_CONTRACT("http.preparation.test", 1, 0),
          FORGE_API_METHOD(read, ref), FORGE_API_METHOD(write, ref), FORGE_API_METHOD(download, ref),
          FORGE_API_METHOD(upload, ref))

FORGE_HTTP_API(::forge::net::http::preparation_test::probe_api, FORGE_HTTP_GET(read, "/read/:ref"),
               FORGE_HTTP_POST(write, "/write/:ref", ok), FORGE_HTTP_GET(download, "/download/:ref"),
               FORGE_HTTP_POST(upload, "/upload/:ref", ok))

namespace forge::net::http::preparation_test {
namespace {

namespace asio = boost::asio;
using namespace std::chrono_literals;

asio::awaitable<void> delay(std::chrono::milliseconds duration) {
   auto timer = asio::steady_timer{co_await asio::this_coro::executor, duration};
   co_await timer.async_wait(asio::use_awaitable);
}

// A real loopback HTTP server. These are generic transport-provider tests, not an OAuth/IdP backend.
class fixture {
 public:
   explicit fixture(server_handler handler = {})
       : server_{
             runtime, {}, [this, handler = std::move(handler)](route_context& context) -> asio::awaitable<response> {
                received.fetch_add(1U);
                if (handler) {
                   co_return co_await handler(context);
                }
                co_return make_text_response(context.request, status::ok, context.request["Authorization"]);
             }} {
      forge::asio::blocking::run(runtime, server_.async_start());
      endpoint = parse_base_url("http://127.0.0.1:" + std::to_string(server_.port()) + "/resource");
   }

   ~fixture() {
      forge::asio::blocking::run(runtime, server_.async_stop());
   }

   forge::asio::runtime runtime;
   std::atomic_size_t received{0U};
   base_url endpoint;

 private:
   server server_;
};

class counted_body final : public body_reader::source {
 public:
   asio::awaitable<std::optional<body_chunk>> async_read() override {
      ++reads;
      if (reads > 1U) {
         co_return std::nullopt;
      }
      auto chunk = body_chunk{.bytes = std::vector<std::byte>(7U)};
      std::memcpy(chunk.bytes.data(), "payload", 7U);
      co_return chunk;
   }

   std::uint64_t bytes_read() const noexcept override {
      return reads == 0U ? 0U : 7U;
   }

   std::size_t reads = 0U;
};

enum class request_kind { unary, upload, download, upload_download };
constexpr auto request_kinds = {request_kind::unary, request_kind::upload, request_kind::download,
                                request_kind::upload_download};

template <typename Transport>
asio::awaitable<response> invoke(Transport& target, request_kind kind, request_options options = {},
                                 std::shared_ptr<counted_body> body = std::make_shared<counted_body>(),
                                 std::string_view path = "/probe?resource=one", std::string_view host = {}) {
   auto request_target = std::string{path};
   if constexpr (requires { target.make_target(path); }) {
      request_target = target.make_target(path);
   }
   auto value =
       request{kind == request_kind::upload || kind == request_kind::upload_download ? method::post : method::get,
               std::move(request_target), 11};
   value.set("X-Resource", "one");
   if (!host.empty()) {
      value.set(field::host, host);
   }
   if (kind == request_kind::unary) {
      co_return co_await target.async_request(std::move(value), options);
   }
   if (kind == request_kind::upload) {
      co_return co_await target.async_streaming_request(std::move(value), body_reader{body}, options);
   }
   auto result = kind == request_kind::download
                     ? co_await target.async_stream_request(std::move(value), options)
                     : co_await target.async_stream_request(std::move(value), body_reader{body}, options);
   result.head.body() = co_await result.body.async_read_all();
   co_return std::move(result.head);
}

client_options prepared_options() {
   return {.async_header_provider = [](const request&,
                                       request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
      co_await delay(1ms);
      co_return std::vector<header_entry>{{"Authorization", "Bearer transport-test"}};
   }};
}

} // namespace

BOOST_AUTO_TEST_SUITE(http_client_preparation_tests)

BOOST_AUTO_TEST_CASE(host_header_preserves_nondefault_ports_and_ipv6_brackets) {
   for (const auto& [url, expected] :
        {std::pair{"http://example.test", "example.test"}, std::pair{"http://example.test:80", "example.test"},
         std::pair{"https://example.test", "example.test"}, std::pair{"https://example.test:443", "example.test"},
         std::pair{"http://127.0.0.1:8080", "127.0.0.1:8080"}, std::pair{"https://example.test:80", "example.test:80"},
         std::pair{"https://example.test:9443", "example.test:9443"}, std::pair{"http://[::1]", "[::1]"},
         std::pair{"https://[::1]:443", "[::1]"}, std::pair{"http://[::1]:8080", "[::1]:8080"},
         std::pair{"https://[2001:db8::1]:9443", "[2001:db8::1]:9443"}}) {
      BOOST_CHECK_EQUAL(parse_base_url(url).host_header(), expected);
   }
}

BOOST_AUTO_TEST_CASE(ordinary_client_and_connection_shapes_send_the_full_authority) {
   auto test = fixture{[](route_context& context) -> asio::awaitable<response> {
      co_return make_text_response(context.request, status::ok, context.request[field::host]);
   }};
   const auto expected = test.endpoint.host + ":" + test.endpoint.port;
   for (const auto provider : {false, true}) {
      auto options = client_options{};
      if (provider) {
         options.header_provider = [](const request&) { return std::vector<header_entry>{{"X-Prepared", "yes"}}; };
      }
      auto target = client{test.runtime, test.endpoint, std::move(options)};
      for (const auto kind : request_kinds) {
         const auto result = forge::asio::blocking::run(test.runtime, invoke(target, kind, {.timeout = 2s}));
         BOOST_CHECK_EQUAL(result.body(), expected);
      }
   }
   auto target = connection{test.runtime, test.endpoint};
   for (const auto kind : request_kinds) {
      const auto result = forge::asio::blocking::run(test.runtime, invoke(target, kind, {.timeout = 2s}));
      BOOST_CHECK_EQUAL(result.body(), expected);
   }
   BOOST_CHECK_EQUAL(test.received.load(), 12U);
}

BOOST_AUTO_TEST_CASE(ordinary_client_and_connection_shapes_preserve_explicit_host) {
   auto test = fixture{[](route_context& context) -> asio::awaitable<response> {
      co_return make_text_response(context.request, status::ok, context.request[field::host]);
   }};
   const auto expected = std::string_view{"caller-selected.example:8443"};
   auto client_target = client{test.runtime, test.endpoint};
   auto connection_target = connection{test.runtime, test.endpoint};
   for (const auto kind : request_kinds) {
      const auto client_result =
          forge::asio::blocking::run(test.runtime, invoke(client_target, kind, {.timeout = 2s},
                                                          std::make_shared<counted_body>(), "/probe", expected));
      BOOST_CHECK_EQUAL(client_result.body(), expected);
      const auto connection_result =
          forge::asio::blocking::run(test.runtime, invoke(connection_target, kind, {.timeout = 2s},
                                                          std::make_shared<counted_body>(), "/probe", expected));
      BOOST_CHECK_EQUAL(connection_result.body(), expected);
   }
   BOOST_CHECK_EQUAL(test.received.load(), 8U);
}

BOOST_AUTO_TEST_CASE(all_request_shapes_share_async_preparation_and_exact_metadata) {
   auto test = fixture{};
   auto calls = std::atomic_size_t{0U};
   auto metadata_matches = std::atomic_bool{true};
   auto options = client_options{
       .async_header_provider = [&](const request& value,
                                    request_preparation_context context) -> asio::awaitable<std::vector<header_entry>> {
          ++calls;
          metadata_matches.store(metadata_matches.load() && context.origin == test.endpoint.origin() &&
                                 context.deadline > std::chrono::steady_clock::now() &&
                                 context.deadline <= std::chrono::steady_clock::now() + 2s &&
                                 value.target() == "/resource/probe?resource=one" &&
                                 value["Host"] == test.endpoint.host + ":" + test.endpoint.port &&
                                 value["X-Resource"] == "one");
          co_await delay(1ms);
          co_return std::vector<header_entry>{{"Authorization", "Bearer transport-test"}};
       }};
   auto target = client{test.runtime, test.endpoint, std::move(options)};
   for (const auto kind : request_kinds) {
      const auto result = forge::asio::blocking::run(test.runtime, invoke(target, kind, {.timeout = 2s}));
      BOOST_CHECK(result.result() == status::ok);
      BOOST_CHECK_EQUAL(result.body(), "Bearer transport-test");
   }
   BOOST_CHECK(metadata_matches.load());
   BOOST_CHECK_EQUAL(calls.load(), 4U);
   BOOST_CHECK_EQUAL(test.received.load(), 4U);
}

BOOST_AUTO_TEST_CASE(sync_provider_still_works_and_dual_provider_configuration_is_rejected) {
   auto test = fixture{};
   auto options = client_options{.header_provider = [](const request&) {
      return std::vector<header_entry>{{"Authorization", "Bearer sync-test"}};
   }};
   auto target = client{test.runtime, test.endpoint, options};
   const auto result = forge::asio::blocking::run(test.runtime, target.async_get("/probe"));
   BOOST_CHECK_EQUAL(result.body(), "Bearer sync-test");
   options.async_header_provider = prepared_options().async_header_provider;
   BOOST_CHECK_THROW((void)client(test.runtime, test.endpoint, std::move(options)), exceptions::bad_request);
   BOOST_CHECK_EQUAL(test.received.load(), 1U);
}

BOOST_AUTO_TEST_CASE(provider_failures_are_prompt_and_do_not_send_or_expose_diagnostics) {
   auto test = fixture{};
   auto target =
       client{test.runtime,
              test.endpoint,
              {.async_header_provider = [](const request&,
                                           request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
                 throw std::runtime_error{"secret-provider-diagnostic"};
                 co_return std::vector<header_entry>{};
              }}};
   auto result = asio::co_spawn(test.runtime.context(), target.async_get("/probe", {.timeout = 5s}), asio::use_future);
   BOOST_CHECK(result.wait_for(1s) == std::future_status::ready);
   BOOST_CHECK_EXCEPTION(static_cast<void>(result.get()), exceptions::internal, [](const exceptions::internal& error) {
      return std::string{error.what()}.find("secret-provider-diagnostic") == std::string::npos;
   });
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
   BOOST_CHECK_EQUAL(target.metrics().started_requests, 0U);
}

BOOST_AUTO_TEST_CASE(header_validation_is_shared_and_rejects_before_network_io) {
   auto test = fixture{};
   const auto invalid_results =
       std::vector<std::vector<header_entry>>{{{"Host", "other.example"}},
                                              {{"Content-Length", "7"}},
                                              {{"Transfer-Encoding", "chunked"}},
                                              {{"Connection", "close"}},
                                              {{"Authorization", "one"}, {"authorization", "two"}},
                                              {{"Bad Name", "one"}},
                                              {{"Authorization", "one\r\nX-Injected: two"}}};
   for (const auto& headers : invalid_results) {
      auto target = client{test.runtime,
                           test.endpoint,
                           {.async_header_provider = [headers](const request&, request_preparation_context)
                                -> asio::awaitable<std::vector<header_entry>> { co_return headers; }}};
      BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, target.async_get("/probe")), exceptions::bad_request);
      BOOST_CHECK_EQUAL(target.metrics().queued_requests, 0U);
   }
   auto collision = client{test.runtime, test.endpoint, prepared_options()};
   auto value = request{method::get, "/resource/probe", 11};
   value.set("authorization", "existing");
   BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, collision.async_request(std::move(value))),
                     exceptions::bad_request);

   auto bounded = prepared_options();
   bounded.max_provider_header_bytes = 8U;
   auto oversized = client{test.runtime, test.endpoint, std::move(bounded)};
   BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, oversized.async_get("/probe")),
                     exceptions::request_header_fields_too_large);
   auto count_limited = prepared_options();
   count_limited.max_provider_headers = 1U;
   count_limited.async_header_provider = [](const request&,
                                            request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
      co_return std::vector<header_entry>{{"X-One", "one"}, {"X-Two", "two"}};
   };
   auto excessive = client{test.runtime, test.endpoint, std::move(count_limited)};
   BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, excessive.async_get("/probe")),
                     exceptions::request_header_fields_too_large);
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
}

BOOST_AUTO_TEST_CASE(origin_mismatch_is_rejected_before_acquisition) {
   auto test = fixture{};
   auto calls = std::atomic_size_t{0U};
   auto options = prepared_options();
   const auto provider = options.async_header_provider;
   options.async_header_provider =
       [&, provider](const request& value,
                     request_preparation_context context) -> asio::awaitable<std::vector<header_entry>> {
      ++calls;
      co_return co_await provider(value, std::move(context));
   };
   auto target = client{test.runtime, test.endpoint, std::move(options)};
   for (const auto path : {"https://other.example/probe", "//other.example/probe", "/probe#fragment"}) {
      BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, target.async_request(request{method::get, path, 11})),
                        exceptions::bad_request);
   }
   auto value = request{method::get, "/probe", 11};
   value.set("Host", "other.example");
   BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, target.async_request(std::move(value))),
                     exceptions::bad_request);
   auto duplicate = request{method::get, "/probe", 11};
   duplicate.insert("Host", test.endpoint.host + ":" + test.endpoint.port);
   duplicate.insert("host", test.endpoint.host + ":" + test.endpoint.port);
   BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, target.async_request(std::move(duplicate))),
                     exceptions::bad_request);
   BOOST_CHECK_EQUAL(calls.load(), 0U);
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
}

BOOST_AUTO_TEST_CASE(expired_deadline_does_not_start_acquisition_for_any_request_shape) {
   auto test = fixture{};
   auto calls = std::atomic_size_t{0U};
   auto target =
       client{test.runtime,
              test.endpoint,
              {.async_header_provider = [&](const request&,
                                            request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
                 ++calls;
                 co_return std::vector<header_entry>{};
              }}};
   for (const auto kind : request_kinds) {
      auto body = std::make_shared<counted_body>();
      BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, invoke(target, kind, {.timeout = 0ms}, body)),
                        exceptions::gateway_timeout);
      BOOST_CHECK_EQUAL(body->reads, 0U);
   }
   BOOST_CHECK_EQUAL(calls.load(), 0U);
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
}

BOOST_AUTO_TEST_CASE(acquisition_timeout_cancels_the_provider_promptly) {
   auto test = fixture{};
   auto canceled = std::atomic_bool{false};
   auto target =
       client{test.runtime,
              test.endpoint,
              {.async_header_provider = [&](const request&,
                                            request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
                 try {
                    co_await delay(5s);
                 } catch (const boost::system::system_error& error) {
                    canceled.store(error.code() == asio::error::operation_aborted);
                    throw;
                 }
                 co_return std::vector<header_entry>{};
              }}};
   auto result =
       asio::co_spawn(test.runtime.context(), target.async_get("/probe", {.timeout = 20ms}), asio::use_future);
   BOOST_CHECK(result.wait_for(1s) == std::future_status::ready);
   BOOST_CHECK_THROW(static_cast<void>(result.get()), exceptions::gateway_timeout);
   BOOST_CHECK(canceled.load());
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
}

BOOST_AUTO_TEST_CASE(caller_absolute_deadline_is_visible_and_bounds_all_request_shapes) {
   auto test = fixture{};
   auto expected = std::chrono::steady_clock::time_point{};
   auto matches = std::atomic_bool{true};
   auto target = client{
       test.runtime,
       test.endpoint,
       {.async_header_provider =
            [&](const request&, request_preparation_context context) -> asio::awaitable<std::vector<header_entry>> {
          matches.store(matches.load() && context.deadline == expected);
          co_await delay(5s);
          co_return std::vector<header_entry>{};
       }}};
   for (const auto kind : request_kinds) {
      expected = std::chrono::steady_clock::now() + 20ms;
      auto result = asio::co_spawn(test.runtime.context(), invoke(target, kind, {.timeout = 2s, .deadline = expected}),
                                   asio::use_future);
      BOOST_CHECK(result.wait_for(1s) == std::future_status::ready);
      BOOST_CHECK_THROW(static_cast<void>(result.get()), exceptions::gateway_timeout);
   }
   BOOST_CHECK(matches.load());
   BOOST_CHECK_EQUAL(test.received.load(), 0U);

   auto plain = client{test.runtime, test.endpoint};
   for (const auto kind : request_kinds) {
      BOOST_CHECK_THROW(
          forge::asio::blocking::run(
              test.runtime, invoke(plain, kind, {.timeout = 2s, .deadline = std::chrono::steady_clock::now() - 1ms})),
          exceptions::gateway_timeout);
   }
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
}

BOOST_AUTO_TEST_CASE(maximum_timeout_saturates_without_clock_overflow) {
   auto test = fixture{};
   auto saturated = std::atomic_bool{false};
   auto target = client{
       test.runtime,
       test.endpoint,
       {.async_header_provider =
            [&](const request&, request_preparation_context context) -> asio::awaitable<std::vector<header_entry>> {
          saturated.store(context.deadline == std::chrono::steady_clock::time_point::max());
          co_return std::vector<header_entry>{};
       }}};
   const auto result = forge::asio::blocking::run(
       test.runtime, target.async_get("/probe", {.timeout = std::chrono::milliseconds::max()}));
   BOOST_CHECK(result.result() == status::ok);
   BOOST_CHECK(saturated.load());
}

BOOST_AUTO_TEST_CASE(late_results_after_timeout_cannot_send_or_read_upload_bodies) {
   auto test = fixture{};
   auto returned = std::atomic_size_t{0U};
   auto target =
       client{test.runtime,
              test.endpoint,
              {.async_header_provider = [&](const request&,
                                            request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
                 co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
                 co_await delay(60ms);
                 ++returned;
                 co_return std::vector<header_entry>{{"Authorization", "late-test-value"}};
              }}};
   for (const auto kind : request_kinds) {
      auto body = std::make_shared<counted_body>();
      BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, invoke(target, kind, {.timeout = 20ms}, body)),
                        exceptions::gateway_timeout);
      BOOST_CHECK_EQUAL(body->reads, 0U);
   }
   BOOST_CHECK_EQUAL(returned.load(), 4U);
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
   BOOST_CHECK_EQUAL(target.metrics().started_requests, 0U);
}

BOOST_AUTO_TEST_CASE(late_results_after_caller_cancellation_cannot_send_any_request_shape) {
   auto test = fixture{};
   for (const auto kind : request_kinds) {
      auto started = std::promise<void>{};
      auto ready = started.get_future();
      auto returned = std::atomic_bool{false};
      auto target =
          client{test.runtime,
                 test.endpoint,
                 {.async_header_provider =
                      [&](const request&, request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
                    co_await asio::this_coro::reset_cancellation_state(asio::disable_cancellation{});
                    started.set_value();
                    co_await delay(60ms);
                    returned.store(true);
                    co_return std::vector<header_entry>{{"Authorization", "late-test-value"}};
                 }}};
      auto signal = asio::cancellation_signal{};
      auto body = std::make_shared<counted_body>();
      auto result = asio::co_spawn(test.runtime.context(), invoke(target, kind, {.timeout = 2s}, body),
                                   asio::bind_cancellation_slot(signal.slot(), asio::use_future));
      BOOST_REQUIRE(ready.wait_for(1s) == std::future_status::ready);
      asio::post(test.runtime.context(), [&signal] { signal.emit(asio::cancellation_type::all); });
      BOOST_REQUIRE(result.wait_for(1s) == std::future_status::ready);
      BOOST_CHECK_THROW(static_cast<void>(result.get()), forge::asio::exceptions::canceled);
      BOOST_CHECK(returned.load());
      BOOST_CHECK_EQUAL(body->reads, 0U);
      BOOST_CHECK_EQUAL(target.metrics().queued_requests, 0U);
   }
   BOOST_CHECK_EQUAL(test.received.load(), 0U);
}

BOOST_AUTO_TEST_CASE(acquisition_does_not_block_other_calls_on_one_runtime_worker) {
   auto test = fixture{};
   auto started = std::promise<void>{};
   auto ready = started.get_future();
   auto gate = asio::steady_timer{test.runtime.context(), std::chrono::steady_clock::time_point::max()};
   auto target =
       client{test.runtime,
              test.endpoint,
              {.async_header_provider = [&](const request& value,
                                            request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
                 if (value.target().ends_with("/slow")) {
                    started.set_value();
                    auto error = boost::system::error_code{};
                    co_await gate.async_wait(asio::redirect_error(asio::use_awaitable, error));
                 }
                 co_return std::vector<header_entry>{{"Authorization", "Bearer transport-test"}};
              }}};
   auto slow = asio::co_spawn(test.runtime.context(), target.async_get("/slow"), asio::use_future);
   BOOST_REQUIRE(ready.wait_for(1s) == std::future_status::ready);
   auto fast = asio::co_spawn(test.runtime.context(), target.async_get("/fast"), asio::use_future);
   BOOST_REQUIRE(fast.wait_for(1s) == std::future_status::ready);
   BOOST_CHECK(fast.get().result() == status::ok);
   BOOST_CHECK(slow.wait_for(0ms) != std::future_status::ready);
   asio::post(test.runtime.context(), [&gate] { gate.cancel(); });
   BOOST_REQUIRE(slow.wait_for(1s) == std::future_status::ready);
   BOOST_CHECK(slow.get().result() == status::ok);
   BOOST_CHECK_EQUAL(test.received.load(), 2U);
}

BOOST_AUTO_TEST_CASE(acquisition_and_transport_share_one_deadline_for_all_request_shapes) {
   auto test = fixture{[](route_context& context) -> asio::awaitable<response> {
      co_await delay(120ms);
      co_return make_text_response(context.request, status::ok, "late");
   }};
   auto options = prepared_options();
   options.async_header_provider = [](const request&,
                                      request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
      co_await delay(120ms);
      co_return std::vector<header_entry>{{"Authorization", "Bearer transport-test"}};
   };
   auto target = client{test.runtime, test.endpoint, std::move(options)};
   for (const auto kind : request_kinds) {
      BOOST_CHECK_THROW(
          forge::asio::blocking::run(
              test.runtime, invoke(target, kind, {.timeout = 200ms, .retry_idempotent = true, .max_retries = 3U})),
          exceptions::gateway_timeout);
   }
   BOOST_CHECK_EQUAL(test.received.load(), 4U);
   BOOST_CHECK_EQUAL(target.metrics().retry_attempts, 0U);
}

BOOST_AUTO_TEST_CASE(response_body_deadline_still_includes_acquisition_after_headers_arrive) {
   auto runtime = forge::asio::runtime{};
   auto router = forge::net::http::router{};
   router.get_stream("/probe", [](stream_request& value) -> asio::awaitable<stream_response> {
      auto source = std::make_shared<counted_body>();
      co_return stream_response{.head = response{status::ok, value.context.request.version()},
                                .body = [source]() -> asio::awaitable<std::optional<body_chunk>> {
                                   co_await delay(120ms);
                                   co_return co_await source->async_read();
                                }};
   });
   auto server = forge::net::http::server{runtime, {}, std::move(router)};
   forge::asio::blocking::run(runtime, server.async_start());
   auto options = prepared_options();
   options.async_header_provider = [](const request&,
                                      request_preparation_context) -> asio::awaitable<std::vector<header_entry>> {
      co_await delay(120ms);
      co_return std::vector<header_entry>{{"Authorization", "Bearer transport-test"}};
   };
   auto target =
       client{runtime, parse_base_url("http://127.0.0.1:" + std::to_string(server.port())), std::move(options)};
   auto scenario = [&]() -> asio::awaitable<void> {
      auto result = co_await target.async_stream_request(request{method::get, "/probe", 11}, {.timeout = 200ms});
      BOOST_CHECK(result.head.result() == status::ok);
      static_cast<void>(co_await result.body.async_read_all());
   };
   BOOST_CHECK_THROW(forge::asio::blocking::run(runtime, scenario()), exceptions::gateway_timeout);
   forge::asio::blocking::run(runtime, server.async_stop());
}

BOOST_AUTO_TEST_CASE(redirects_and_unauthorized_responses_never_forward_or_replay) {
   auto destination = fixture{};
   auto source = fixture{[&](route_context& context) -> asio::awaitable<response> {
      const auto redirect = context.request.target().ends_with("/redirect");
      auto result =
          make_text_response(context.request, redirect ? static_cast<status>(307) : status::unauthorized, "");
      if (redirect) {
         result.set("Location", destination.endpoint.origin() + "/credential-leak");
      }
      co_return result;
   }};
   auto calls = std::atomic_size_t{0U};
   auto options = prepared_options();
   const auto provider = options.async_header_provider;
   options.async_header_provider =
       [&, provider](const request& value,
                     request_preparation_context context) -> asio::awaitable<std::vector<header_entry>> {
      ++calls;
      co_return co_await provider(value, std::move(context));
   };
   auto target = client{source.runtime, source.endpoint, std::move(options)};
   for (const auto kind : request_kinds) {
      const auto result = forge::asio::blocking::run(
          source.runtime, invoke(target, kind, {.retry_idempotent = true, .max_retries = 3U}));
      BOOST_CHECK(result.result() == status::unauthorized);
      const auto redirect =
          forge::asio::blocking::run(source.runtime, invoke(target, kind, {.retry_idempotent = true, .max_retries = 3U},
                                                            std::make_shared<counted_body>(), "/redirect"));
      BOOST_CHECK(redirect.result() == static_cast<status>(307));
   }
   BOOST_CHECK_EQUAL(calls.load(), 8U);
   BOOST_CHECK_EQUAL(source.received.load(), 8U);
   BOOST_CHECK_EQUAL(destination.received.load(), 0U);
   BOOST_CHECK_EQUAL(target.metrics().retry_attempts, 0U);
}

BOOST_AUTO_TEST_CASE(accepted_post_with_lost_response_is_not_replayed) {
   auto runtime = forge::asio::runtime{};
   auto listener = asio::ip::tcp::acceptor{
       runtime.context(), {asio::ip::make_address("127.0.0.1"), 0}};
   auto received_post = std::atomic_bool{false};
   auto drop_response = [&]() -> asio::awaitable<void> {
      auto socket = co_await listener.async_accept(asio::use_awaitable);
      auto headers = asio::streambuf{};
      co_await asio::async_read_until(socket, headers, "\r\n\r\n", asio::use_awaitable);
      const auto prefix = std::string{asio::buffers_begin(headers.data()), asio::buffers_end(headers.data())};
      received_post.store(prefix.starts_with("POST /mutation "));
      socket.set_option(asio::socket_base::linger{true, 0});
      socket.close();
   };
   auto server_done = asio::co_spawn(runtime.context(), drop_response(), asio::use_future);
   auto calls = std::atomic_size_t{0U};
   auto options = prepared_options();
   const auto provider = options.async_header_provider;
   options.async_header_provider =
       [&, provider](const request& value,
                     request_preparation_context context) -> asio::awaitable<std::vector<header_entry>> {
      ++calls;
      co_return co_await provider(value, std::move(context));
   };
   auto target = client{runtime,
                        parse_base_url("http://127.0.0.1:" + std::to_string(listener.local_endpoint().port())),
                        std::move(options)};
   BOOST_CHECK_THROW(
       forge::asio::blocking::run(runtime,
           target.async_request(request{method::post, "/mutation", 11},
                                {.timeout = 1s, .retry_idempotent = true, .max_retries = 3U})),
       exceptions::unavailable);
   BOOST_REQUIRE(server_done.wait_for(1s) == std::future_status::ready);
   server_done.get();
   BOOST_CHECK(received_post.load());
   BOOST_CHECK_EQUAL(calls.load(), 1U);
   BOOST_CHECK_EQUAL(target.metrics().retry_attempts, 0U);
}

BOOST_AUTO_TEST_CASE(typed_proxy_composes_once_for_multiple_methods_and_stream_preparation_failures) {
   auto test = fixture{[](route_context& context) -> asio::awaitable<response> {
      auto result = make_text_response(context.request, status::ok, "\"prepared\"");
      result.set(field::content_type, "application/json");
      co_return result;
   }};
   auto calls = std::atomic_size_t{0U};
   auto options = prepared_options();
   const auto provider = options.async_header_provider;
   options.async_header_provider =
       [&, provider](const request& value,
                     request_preparation_context context) -> asio::awaitable<std::vector<header_entry>> {
      ++calls;
      if (value.target().find("/download/") != std::string_view::npos ||
          value.target().find("/upload/") != std::string_view::npos) {
         throw exceptions::bad_request{"generic provider denied this resource"};
      }
      co_return co_await provider(value, std::move(context));
   };
   auto target = client{test.runtime, test.endpoint, std::move(options)};
   auto remote = forge::asio::blocking::run(test.runtime, forge::api::http::remote<probe_api>(target));
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(test.runtime, remote->read("one")), "prepared");
   BOOST_CHECK_EQUAL(forge::asio::blocking::run(test.runtime, remote->write("two")), "prepared");
   auto download = [&]() -> asio::awaitable<void> {
      auto stream = co_await remote.async_open<&probe_api::download>("denied");
      static_cast<void>(co_await stream.async_read());
      co_await stream.async_finish();
   };
   auto upload = [&]() -> asio::awaitable<void> {
      auto stream = co_await remote.async_open<&probe_api::upload>("denied");
      co_await stream.async_close();
      static_cast<void>(co_await stream.async_finish());
   };
   BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, download()), exceptions::bad_request);
   BOOST_CHECK_THROW(forge::asio::blocking::run(test.runtime, upload()), exceptions::bad_request);
   BOOST_CHECK_EQUAL(calls.load(), 4U);
   BOOST_CHECK_EQUAL(test.received.load(), 2U);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace forge::net::http::preparation_test
