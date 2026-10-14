#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <boost/asio/as_tuple.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/socket_base.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>

import forge.asio.runtime;
import forge.log.logger;
import forge.net.http.body;
import forge.net.http.exceptions;
import forge.net.http.route_context;
import forge.net.http.router;
import forge.net.http.server;
import forge.net.http.stream;
import forge.net.http.types;

namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace wire = boost::beast::http;
namespace http = forge::net::http;

constexpr auto deadline = std::chrono::seconds{3};
constexpr auto logger_name = "forge.net.http.server";
constexpr auto secret = "access-token-private-key-body-cookie-904736";
constexpr auto terminal_event = "http.request.finished";

std::string field(const forge::log_record& record, const std::string& key) {
   for (const auto& value : record.fields) {
      if (value.key == key) {
         return value.value;
      }
   }
   return {};
}

class capture_sink final : public forge::sink {
 public:
   void log(const forge::log_record& record) override {
      {
         const auto lock = std::scoped_lock{mutex_};
         records_.push_back(record);
      }
      changed_.notify_all();
   }

   std::vector<forge::log_record> records() const {
      const auto lock = std::scoped_lock{mutex_};
      return records_;
   }

   bool wait_for(const std::string& event, std::size_t count = 1) {
      auto lock = std::unique_lock{mutex_};
      return changed_.wait_for(lock, deadline, [&] {
         return static_cast<std::size_t>(std::count_if(records_.begin(), records_.end(), [&](const auto& record) {
            return field(record, "event") == event;
         })) >= count;
      });
   }

 private:
   mutable std::mutex mutex_;
   std::condition_variable changed_;
   std::vector<forge::log_record> records_;
};

class throwing_sink final : public forge::sink {
 public:
   void log(const forge::log_record&) override { throw std::runtime_error{secret}; }
};

struct log_fixture {
   log_fixture() {
      route.set_log_level(forge::log_level::all);
      route.add_sink(captured);
      forge::logger::update(logger_name, route);
   }
   ~log_fixture() { forge::logger::update(logger_name, previous); }

   std::vector<forge::log_record> events(const std::string& event) const {
      auto result = std::vector<forge::log_record>{};
      for (const auto& record : captured->records()) {
         if (field(record, "event") == event) {
            result.push_back(record);
         }
      }
      return result;
   }

   void require_redacted() const {
      for (const auto& record : captured->records()) {
         BOOST_TEST(record.logger == logger_name);
         BOOST_CHECK(forge::format_json_log_record(record).find(secret) == std::string::npos);
         BOOST_CHECK(record.exception_chain.empty());
         for (const auto& value : record.fields) {
            BOOST_CHECK(value.key != "target" && value.key != "uri" && value.key != "headers" &&
                        value.key != "body" && value.key != "reason" && value.key != "config");
         }
      }
   }

   forge::logger previous = forge::logger::get(logger_name);
   forge::logger route{logger_name};
   std::shared_ptr<capture_sink> captured = std::make_shared<capture_sink>();
};

http::server_config config(bool access = true) {
   auto result = http::server_config{};
   result.read_timeout = deadline;
   result.idle_timeout = deadline;
   result.access_log = access;
   return result;
}

struct exchange_result {
   unsigned status = 0;
   std::string body;
   boost::system::error_code error;
};

asio::awaitable<exchange_result> exchange(std::uint16_t port, wire::request<wire::string_body> request) {
   auto executor = co_await asio::this_coro::executor;
   auto stream = beast::tcp_stream{executor};
   stream.expires_after(deadline);
   co_await stream.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
   co_await wire::async_write(stream, request, asio::use_awaitable);
   auto buffer = beast::flat_buffer{};
   auto response = wire::response<wire::string_body>{};
   auto [error, bytes] = co_await wire::async_read(stream, buffer, response, asio::as_tuple(asio::use_awaitable));
   static_cast<void>(bytes);
   co_return exchange_result{response.result_int(), response.body(), error};
}

wire::request<wire::string_body> request(wire::verb method = wire::verb::post) {
   auto result = wire::request<wire::string_body>{method, std::string{"/"} + secret + "?token=" + secret, 11};
   result.set(wire::field::host, "localhost");
   result.set(wire::field::authorization, std::string{"Bearer "} + secret);
   result.set(wire::field::cookie, std::string{"session="} + secret);
   result.keep_alive(false);
   if (method == wire::verb::post) {
      result.body() = secret;
   }
   result.prepare_payload();
   return result;
}

exchange_result send(std::uint16_t port, wire::request<wire::string_body> message = request()) {
   auto context = asio::io_context{};
   auto result = asio::co_spawn(context, exchange(port, std::move(message)), asio::use_future);
   context.run();
   return result.get();
}

asio::awaitable<boost::system::error_code> malformed_request(std::uint16_t port) {
   auto executor = co_await asio::this_coro::executor;
   auto stream = beast::tcp_stream{executor};
   stream.expires_after(deadline);
   co_await stream.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
   const auto input = std::string{"GET /"} + secret + " HTTP/1.1\r\nHost: localhost\r\nbad header: " + secret + "\r\n\r\n";
   co_await asio::async_write(stream, asio::buffer(input), asio::use_awaitable);
   auto byte = char{};
   auto [error, count] = co_await stream.async_read_some(asio::buffer(&byte, 1), asio::as_tuple(asio::use_awaitable));
   static_cast<void>(count);
   co_return error;
}

asio::awaitable<void> reset_after_headers(std::uint16_t port) {
   auto executor = co_await asio::this_coro::executor;
   auto stream = beast::tcp_stream{executor};
   stream.expires_after(deadline);
   co_await stream.async_connect({asio::ip::make_address("127.0.0.1"), port}, asio::use_awaitable);
   auto message = request(wire::verb::get);
   co_await wire::async_write(stream, message, asio::use_awaitable);
   auto buffer = beast::flat_buffer{};
   auto parser = wire::response_parser<wire::string_body>{};
   co_await wire::async_read_header(stream, buffer, parser, asio::use_awaitable);
   stream.socket().set_option(asio::socket_base::linger{true, 0});
   stream.socket().close();
}

http::body_chunk chunk(const std::string& value) {
   auto bytes = std::vector<std::byte>(value.size());
   std::memcpy(bytes.data(), value.data(), value.size());
   return {.bytes = std::move(bytes)};
}

http::router streaming_router(bool producer_failure = false, bool guard_failure = false) {
   auto router = http::router{};
   router.get_stream("/:id", [producer_failure, guard_failure](http::stream_request& request)
       -> asio::awaitable<http::stream_response> {
      auto response = http::stream_response{};
      response.head = http::make_text_response(request.context.request, http::status::ok, "");
      response.head.erase(http::field::content_length);
      response.body = [producer_failure, index = std::make_shared<unsigned>(0)]()
          -> asio::awaitable<std::optional<http::body_chunk>> {
         if ((*index)++ == 0) {
            co_return chunk(secret);
         }
         if (producer_failure) {
            throw http::exceptions::internal{secret};
         }
         if (*index == 2) {
            co_return chunk(secret);
         }
         co_return std::nullopt;
      };
      if (guard_failure) {
         response.send_guard = [](asio::awaitable<void> send, std::function<void()>) -> asio::awaitable<void> {
            co_await std::move(send);
            // A complete wire write is not an accepted send if the guard rejects it.
            throw http::exceptions::unavailable{secret};
         };
      }
      co_return response;
   });
   return router;
}

void require_terminal(const forge::log_record& record, const std::string& outcome, unsigned status) {
   BOOST_TEST(field(record, "outcome") == outcome);
   BOOST_TEST(field(record, "status") == std::to_string(status));
   BOOST_TEST(field(record, "method") == std::to_string(static_cast<unsigned>(http::method::get)));
   BOOST_CHECK(record.level == forge::log_level::debug);
   BOOST_TEST(std::stoll(field(record, "duration_us")) >= 0);
}

} // namespace

BOOST_AUTO_TEST_CASE(http_observability_defaults_to_lifecycle_only_and_sink_cannot_veto_io) {
   auto logs = log_fixture{};
   logs.route.add_sink(std::make_shared<throwing_sink>());
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = http::server{runtime, config(false), [](http::route_context& context) -> asio::awaitable<http::response> {
      co_return http::make_text_response(context.request, http::status::ok, secret);
   }};
   BOOST_TEST(!http::server_config{}.access_log);
   BOOST_CHECK_NO_THROW(server.start());
   BOOST_TEST(server.port() != 0);
   BOOST_TEST(logs.events("http.listening").size() == 1);
   const auto result = send(server.port());
   BOOST_CHECK(!result.error);
   BOOST_TEST(result.body == secret);
   BOOST_CHECK_NO_THROW(server.stop());
   BOOST_TEST(logs.events("http.drained").size() == 1);
   BOOST_TEST(logs.events(terminal_event).empty());
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_access_records_redact_requests_and_do_not_promote_4xx_to_errors) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = http::server{runtime, config(), [](http::route_context& context) -> asio::awaitable<http::response> {
      co_return http::make_text_response(context.request, http::status::forbidden, secret);
   }};
   server.start();
   const auto result = send(server.port());
   BOOST_TEST(result.status == 403);
   BOOST_REQUIRE(logs.captured->wait_for(terminal_event));
   server.stop();
   const auto records = logs.events(terminal_event);
   BOOST_REQUIRE(records.size() == 1);
   BOOST_TEST(field(records[0], "method") == std::to_string(static_cast<unsigned>(http::method::post)));
   BOOST_TEST(field(records[0], "outcome") == "completed");
   BOOST_TEST(field(records[0], "status") == "403");
   BOOST_TEST(std::stoull(field(records[0], "received_bytes")) > 0);
   BOOST_TEST(std::stoull(field(records[0], "sent_bytes")) > 0);
   for (const auto& record : logs.captured->records()) {
      BOOST_CHECK(record.level != forge::log_level::warn && record.level != forge::log_level::error);
   }
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_handler_failures_log_typed_codes_without_exception_text) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = http::server{runtime, config(), [](http::route_context&) -> asio::awaitable<http::response> {
      throw http::exceptions::internal{secret};
      co_return http::response{};
   }};
   server.start();
   BOOST_TEST(send(server.port()).status == 500);
   BOOST_REQUIRE(logs.captured->wait_for(terminal_event));
   server.stop();
   const auto failures = logs.events("http.handler.failed");
   BOOST_REQUIRE(failures.size() == 1);
   BOOST_CHECK(failures[0].level == forge::log_level::error);
   BOOST_TEST(field(failures[0], "error_code") == "500");
   BOOST_TEST(field(failures[0], "error_category") == "forge");
   BOOST_REQUIRE(logs.events(terminal_event).size() == 1);
   BOOST_TEST(field(logs.events(terminal_event)[0], "outcome") == "completed");
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_stream_emits_one_terminal_record_after_the_whole_send) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = http::server{runtime, config(), streaming_router()};
   server.start();
   const auto result = send(server.port(), request(wire::verb::get));
   BOOST_CHECK(!result.error);
   BOOST_TEST(result.body == std::string{secret} + secret);
   BOOST_REQUIRE(logs.captured->wait_for(terminal_event));
   server.stop();
   const auto records = logs.events(terminal_event);
   BOOST_REQUIRE(records.size() == 1);
   require_terminal(records[0], "completed", 200);
   BOOST_TEST(field(records[0], "response_chunks") == "2");
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_stream_producer_failure_never_records_partial_send_as_completed) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = http::server{runtime, config(), streaming_router(true)};
   server.start();
   const auto result = send(server.port(), request(wire::verb::get));
   BOOST_CHECK(result.error);
   BOOST_REQUIRE(logs.captured->wait_for(terminal_event));
   server.stop();
   const auto records = logs.events(terminal_event);
   BOOST_REQUIRE(records.size() == 1);
   require_terminal(records[0], "aborted", 200);
   BOOST_TEST(field(records[0], "response_chunks") == "1");
   BOOST_TEST(field(records[0], "error_code") == "500");
   BOOST_REQUIRE(logs.events("http.session.failed").size() == 1);
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_stream_guard_failure_cannot_emit_an_early_success_record) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = http::server{runtime, config(), streaming_router(false, true)};
   server.start();
   const auto result = send(server.port(), request(wire::verb::get));
   BOOST_TEST(result.status == 200);
   BOOST_REQUIRE(logs.captured->wait_for(terminal_event));
   server.stop();
   const auto records = logs.events(terminal_event);
   BOOST_REQUIRE(records.size() == 1);
   require_terminal(records[0], "aborted", 200);
   BOOST_TEST(field(records[0], "error_code") == "503");
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_resource_rejections_are_bounded_summaries_flushed_before_drain) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto options = config();
   options.max_header_bytes = 128;
   auto server = http::server{runtime, options, http::router{}};
   server.start();
   const auto began = std::chrono::steady_clock::now();
   constexpr auto rejected = std::size_t{8};
   for (auto index = std::size_t{0}; index != rejected; ++index) {
      auto message = request(wire::verb::get);
      message.set("X-Test-Secret", std::string(1024, 's') + secret);
      BOOST_TEST(send(server.port(), std::move(message)).status == 431);
      BOOST_REQUIRE(logs.captured->wait_for(terminal_event, index + 1));
   }
   server.stop();
   const auto summaries = logs.events("http.admission.summary");
   auto total = std::uint64_t{0};
   for (const auto& summary : summaries) {
      total += std::stoull(field(summary, "header_rejected"));
      BOOST_CHECK(summary.level == forge::log_level::debug);
   }
   const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - began);
   BOOST_TEST(total == rejected);
   BOOST_TEST(summaries.size() <= 2 + static_cast<std::size_t>(elapsed.count() / 5));
   BOOST_TEST(logs.events(terminal_event).size() == rejected);
   const auto all = logs.captured->records();
   BOOST_REQUIRE(!all.empty());
   BOOST_TEST(field(all.back(), "event") == "http.drained");
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_failed_listen_logs_no_ready_event_and_redacts_config) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto options = config();
   options.bind_address = secret;
   auto server = http::server{runtime, options, http::router{}};
   BOOST_CHECK_THROW(server.start(), std::exception);
   BOOST_TEST(logs.events("http.listening").empty());
   BOOST_REQUIRE(logs.events("http.start.failed").size() == 1);
   BOOST_CHECK(logs.events("http.start.failed")[0].level == forge::log_level::error);
   BOOST_TEST(field(logs.events("http.start.failed")[0], "error_code") != "0");
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_parser_failure_has_one_aborted_record_with_no_fabricated_status) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto server = http::server{runtime, config(), http::router{}};
   server.start();
   auto context = asio::io_context{};
   auto result = asio::co_spawn(context, malformed_request(server.port()), asio::use_future);
   context.run();
   BOOST_CHECK(result.get());
   BOOST_REQUIRE(logs.captured->wait_for(terminal_event));
   server.stop();
   const auto records = logs.events(terminal_event);
   BOOST_REQUIRE(records.size() == 1);
   BOOST_TEST(field(records[0], "status") == "0");
   BOOST_TEST(field(records[0], "outcome") == "aborted");
   BOOST_TEST(field(records[0], "error_code") != "0");
   for (const auto& record : logs.captured->records()) {
      BOOST_CHECK(record.level != forge::log_level::warn && record.level != forge::log_level::error);
   }
   logs.require_redacted();
}

BOOST_AUTO_TEST_CASE(http_peer_reset_cancels_active_stream_and_stays_debug) {
   auto logs = log_fixture{};
   auto runtime = forge::asio::runtime{forge::asio::runtime_options{.worker_threads = 4}};
   auto router = http::router{};
   router.get_stream("/:id", [](http::stream_request& request) -> asio::awaitable<http::stream_response> {
      auto response = http::stream_response{};
      response.head = http::make_text_response(request.context.request, http::status::ok, "");
      response.head.erase(http::field::content_length);
      response.body = []() -> asio::awaitable<std::optional<http::body_chunk>> {
         auto executor = co_await asio::this_coro::executor;
         auto timer = asio::steady_timer{executor};
         timer.expires_after(deadline);
         co_await timer.async_wait(asio::use_awaitable);
         co_return chunk(secret);
      };
      co_return response;
   });
   auto server = http::server{runtime, config(), std::move(router)};
   server.start();
   auto context = asio::io_context{};
   auto result = asio::co_spawn(context, reset_after_headers(server.port()), asio::use_future);
   context.run();
   BOOST_CHECK_NO_THROW(result.get());
   BOOST_REQUIRE(logs.captured->wait_for(terminal_event));
   server.stop();
   const auto records = logs.events(terminal_event);
   BOOST_REQUIRE(records.size() == 1);
   require_terminal(records[0], "aborted", 200);
   BOOST_TEST(field(records[0], "response_chunks") == "0");
   for (const auto& record : logs.captured->records()) {
      BOOST_CHECK(record.level != forge::log_level::warn && record.level != forge::log_level::error);
   }
   logs.require_redacted();
}
