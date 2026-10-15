#include <boost/test/unit_test.hpp>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.asio.blocking;
import forge.asio.runtime;
import forge.net.http.base_url;
import forge.net.http.body;
import forge.net.http.client;
import forge.net.http.connection;
import forge.net.http.exceptions;
import forge.net.http.router;
import forge.net.http.stream;
import forge.net.http.types;
import forge.net.http.server;
import forge.net.tls.context;

namespace forge::tests::http_response_limits {

namespace asio = boost::asio;
namespace http = forge::net::http;

enum class request_mode { buffered, upload, download, duplex };

constexpr auto modes =
    std::array{request_mode::buffered, request_mode::upload, request_mode::download, request_mode::duplex};

http::body_chunk chunk(std::string_view value) {
   auto bytes = std::vector<std::byte>(value.size());
   std::memcpy(bytes.data(), value.data(), value.size());
   return {.bytes = std::move(bytes)};
}

class upload_source final : public http::body_reader::source {
 public:
   asio::awaitable<std::optional<http::body_chunk>> async_read() override {
      if (_sent) {
         co_return std::nullopt;
      }
      _sent = true;
      co_return chunk("upload");
   }

   std::uint64_t bytes_read() const noexcept override {
      return _sent ? 6 : 0;
   }

 private:
   bool _sent = false;
};

struct identity {
   std::string certificate;
   std::string key;
};

identity make_identity() {
   auto key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>{
       EVP_PKEY_Q_keygen(nullptr, nullptr, "EC", "prime256v1"), EVP_PKEY_free};
   auto certificate = std::unique_ptr<X509, decltype(&X509_free)>{X509_new(), X509_free};
   BOOST_REQUIRE(key);
   BOOST_REQUIRE(certificate);
   BOOST_REQUIRE_EQUAL(X509_set_version(certificate.get(), 2), 1);
   BOOST_REQUIRE_EQUAL(ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1), 1);
   BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60));
   BOOST_REQUIRE(X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600));
   BOOST_REQUIRE_EQUAL(X509_set_pubkey(certificate.get(), key.get()), 1);
   auto* subject = X509_get_subject_name(certificate.get());
   constexpr auto hostname = "response-limit.test";
   BOOST_REQUIRE_EQUAL(X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                                  reinterpret_cast<const unsigned char*>(hostname), -1, -1, 0),
                       1);
   BOOST_REQUIRE_EQUAL(X509_set_issuer_name(certificate.get(), subject), 1);
   auto extension = std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)>{
       X509V3_EXT_conf_nid(nullptr, nullptr, NID_basic_constraints, "critical,CA:TRUE"), X509_EXTENSION_free};
   BOOST_REQUIRE(extension);
   BOOST_REQUIRE_EQUAL(X509_add_ext(certificate.get(), extension.get(), -1), 1);
   BOOST_REQUIRE_GT(X509_sign(certificate.get(), key.get(), EVP_sha256()), 0);
   auto certificate_pem = std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new(BIO_s_mem()), BIO_free};
   auto key_pem = std::unique_ptr<BIO, decltype(&BIO_free)>{BIO_new(BIO_s_mem()), BIO_free};
   BOOST_REQUIRE(certificate_pem);
   BOOST_REQUIRE(key_pem);
   BOOST_REQUIRE_EQUAL(PEM_write_bio_X509(certificate_pem.get(), certificate.get()), 1);
   BOOST_REQUIRE_EQUAL(PEM_write_bio_PrivateKey(key_pem.get(), key.get(), nullptr, nullptr, 0, nullptr, nullptr), 1);
   const auto encode = [](BIO* value) {
      const char* bytes = nullptr;
      const auto size = BIO_get_mem_data(value, &bytes);
      BOOST_REQUIRE_GT(size, 0);
      return std::string{bytes, static_cast<std::size_t>(size)};
   };
   return {.certificate = encode(certificate_pem.get()), .key = encode(key_pem.get())};
}

class fixture {
 public:
   fixture(bool secure, bool chunked, std::size_t bytes, bool hold_body = false, bool hold_after_chunk = false)
       : _payload(std::make_shared<const std::string>(bytes, 'x')), _chunked(chunked) {
      auto server_options = http::server_config{};
      auto client_options = http::client_options{};
      if (secure) {
         const auto material = make_identity();
         server_options.tls_context_provider = std::make_shared<forge::net::tls::context_provider>(
             forge::net::tls::context_options{.role = forge::net::tls::endpoint_role::server,
                                              .verification = forge::net::tls::peer_verification::none,
                                              .certificate_chain_pem = material.certificate,
                                              .private_key_pem = material.key,
                                              .use_default_verify_paths = false});
         client_options.tls_context_provider =
             std::make_shared<forge::net::tls::context_provider>(forge::net::tls::context_options{
                 .trust_anchors_pem = {material.certificate}, .use_default_verify_paths = false});
         client_options.hostname = "response-limit.test";
      }

      auto routes = http::router{};
      auto respond = [payload = _payload, chunked, hold_body, hold_after_chunk, done = _send_completed,
                      calls = _calls](http::stream_request& request) -> asio::awaitable<http::stream_response> {
         const auto upload = co_await request.body.async_read_all();
         if (upload != "upload") {
            throw std::runtime_error{"Request did not traverse its upload body path"};
         }
         ++*calls;
         auto head = http::response{http::status::ok, 11};
         if (!chunked) {
            head.set(http::field::content_length, std::to_string(payload->size()));
         }
         auto response = http::stream_response{
             .head = std::move(head),
             .body = [payload, offset = std::size_t{}, tail_chunks = 0U, hold_body,
                      hold_after_chunk]() mutable -> asio::awaitable<std::optional<http::body_chunk>> {
                if (hold_body) {
                   auto timer = asio::steady_timer{co_await asio::this_coro::executor, std::chrono::seconds{5}};
                   co_await timer.async_wait(asio::use_awaitable);
                }
                if (hold_after_chunk && offset != 0) {
                   if (tail_chunks++ == 128U) {
                      co_return std::nullopt;
                   }
                   auto timer = asio::steady_timer{co_await asio::this_coro::executor, std::chrono::milliseconds{5}};
                   co_await timer.async_wait(asio::use_awaitable);
                   co_return chunk(std::string(64 * 1024, 'x'));
                }
                if (offset == payload->size()) {
                   co_return std::nullopt;
                }
                const auto size = std::min(payload->size() - offset, std::size_t{64 * 1024});
                const auto bytes = std::string_view{*payload}.substr(offset, size);
                offset += size;
                co_return chunk(bytes);
             }};
         if (hold_after_chunk) {
            response.send_guard = [done](asio::awaitable<void> send, std::function<void()>) -> asio::awaitable<void> {
               try {
                  co_await std::move(send);
               } catch (const boost::system::system_error&) {
                  done->set_value(true);
                  throw;
               } catch (...) {
                  done->set_value(false);
                  throw;
               }
               done->set_value(false);
            };
         }
         co_return response;
      };
      routes.post_stream("/body", respond);
      routes.get_stream("/body", std::move(respond));
      routes.head_stream("/body",
                         [payload = _payload](http::stream_request&) -> asio::awaitable<http::stream_response> {
                            auto head = http::response{http::status::ok, 11};
                            head.set(http::field::content_length, std::to_string(payload->size()));
                            co_return http::stream_response::buffered(std::move(head));
                         });
      auto no_body = [payload = _payload](http::stream_request& request) -> asio::awaitable<http::stream_response> {
         if (co_await request.body.async_read_all() != "upload") {
            throw std::runtime_error{"No-body response did not consume its request body"};
         }
         const auto status =
             request.context.request.target().ends_with("304") ? http::status::not_modified : http::status::no_content;
         auto head = http::response{status, 11};
         if (status == http::status::not_modified) {
            head.set(http::field::content_length, std::to_string(payload->size()));
         }
         co_return http::stream_response::buffered(std::move(head));
      };
      routes.post_stream("/no-body-204", no_body);
      routes.post_stream("/no-body-304", std::move(no_body));
      _server = std::make_unique<http::server>(_runtime, std::move(server_options), std::move(routes));
      forge::asio::blocking::run(_runtime, _server->async_start());
      _client = std::make_unique<http::client>(_runtime,
                                               http::parse_base_url(std::string{secure ? "https" : "http"} +
                                                                    "://127.0.0.1:" + std::to_string(_server->port())),
                                               std::move(client_options));
   }

   ~fixture() {
      forge::asio::blocking::run(_runtime, _server->async_stop());
   }

   asio::awaitable<std::string> fetch(request_mode mode, http::request_options options = {}, std::string path = "/body",
                                      http::method method = http::method::post, unsigned expected_status = 200) {
      auto request = http::request{method, std::move(path), 11};
      if (mode == request_mode::buffered || mode == request_mode::download) {
         request.body() = "upload";
      }
      auto uploaded = http::body_reader{std::make_shared<upload_source>()};
      if (mode == request_mode::buffered || mode == request_mode::upload) {
         auto response =
             mode == request_mode::buffered
                 ? co_await _client->async_request(std::move(request), options)
                 : co_await _client->async_streaming_request(std::move(request), std::move(uploaded), options);
         check_framing(response, expected_status);
         co_return response.body();
      }
      auto response = mode == request_mode::download
                          ? co_await _client->async_stream_request(std::move(request), options)
                          : co_await _client->async_stream_request(std::move(request), std::move(uploaded), options);
      check_framing(response.head, expected_status);
      co_return co_await response.body.async_read_all();
   }

   asio::awaitable<http::response_stream> open_stream(request_mode mode, http::request_options options) {
      auto request = http::request{http::method::post, "/body", 11};
      request.body() = "upload";
      if (mode == request_mode::download) {
         co_return co_await _client->async_stream_request(std::move(request), options);
      } else {
         request.body().clear();
         co_return co_await _client->async_stream_request(
             std::move(request), http::body_reader{std::make_shared<upload_source>()}, options);
      }
   }

   asio::awaitable<std::string> head(request_mode mode) {
      auto request = http::request{http::method::head, "/body", 11};
      const auto options = http::request_options{.max_response_body_bytes = 0};
      if (mode == request_mode::buffered || mode == request_mode::upload) {
         const auto response =
             mode == request_mode::buffered
                 ? co_await _client->async_request(std::move(request), options)
                 : co_await _client->async_streaming_request(std::move(request), http::body_reader{}, options);
         BOOST_TEST(response[http::field::content_length] == std::to_string(_payload->size()));
         co_return response.body();
      }
      auto response = mode == request_mode::download
                          ? co_await _client->async_stream_request(std::move(request), options)
                          : co_await _client->async_stream_request(std::move(request), http::body_reader{}, options);
      BOOST_TEST(response.head[http::field::content_length] == std::to_string(_payload->size()));
      co_return co_await response.body.async_read_all();
   }

   template <typename Result> Result run(asio::awaitable<Result> operation) {
      return forge::asio::blocking::run(_runtime, std::move(operation));
   }

   bool send_failed() const {
      return _send_done.wait_for(std::chrono::seconds{1}) == std::future_status::ready && _send_done.get();
   }

   unsigned calls() const {
      return _calls->load();
   }

   http::connection_metrics metrics() const {
      return _client->metrics();
   }

 private:
   void check_framing(const http::response& response, unsigned expected_status) const {
      BOOST_TEST(response.result_int() == expected_status);
      if (expected_status != 200) {
         if (expected_status == 304) {
            BOOST_TEST(response[http::field::content_length] == std::to_string(_payload->size()));
         }
         return;
      }
      if (_chunked) {
         BOOST_TEST(response[http::field::transfer_encoding] == "chunked");
      } else {
         BOOST_TEST(response[http::field::content_length] == std::to_string(_payload->size()));
      }
   }

   forge::asio::runtime _runtime{forge::asio::runtime_options{.worker_threads = 2}};
   std::shared_ptr<const std::string> _payload;
   bool _chunked;
   std::shared_ptr<std::atomic<unsigned>> _calls = std::make_shared<std::atomic<unsigned>>(0);
   std::shared_ptr<std::promise<bool>> _send_completed = std::make_shared<std::promise<bool>>();
   std::shared_future<bool> _send_done = _send_completed->get_future().share();
   std::unique_ptr<http::server> _server;
   std::unique_ptr<http::client> _client;
};

BOOST_AUTO_TEST_CASE(http_response_body_explicit_limit_allows_large_responses_on_every_path) {
   for (const bool secure : {false, true}) {
      for (const bool chunked : {false, true}) {
         auto test = fixture{secure, chunked, 9 * 1024 * 1024};
         for (const auto mode : modes) {
            BOOST_TEST_CONTEXT("TLS=" << secure << " chunked=" << chunked << " mode=" << static_cast<int>(mode)) {
               const auto body = test.run(test.fetch(mode, {.max_response_body_bytes = 10 * 1024 * 1024}));
               BOOST_TEST(body.size() == 9U * 1024 * 1024);
               BOOST_TEST(std::ranges::all_of(body, [](char value) { return value == 'x'; }));
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(http_response_body_default_limit_remains_eight_mib) {
   for (const bool secure : {false, true}) {
      for (const bool chunked : {false, true}) {
         auto test = fixture{secure, chunked, 8 * 1024 * 1024 + 1};
         for (const auto mode : modes) {
            BOOST_TEST_CONTEXT("TLS=" << secure << " chunked=" << chunked << " mode=" << static_cast<int>(mode)) {
               BOOST_CHECK_THROW(test.run(test.fetch(mode)), http::exceptions::unavailable);
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(http_response_body_limits_accept_exact_bounds_and_reject_overflow) {
   for (const bool secure : {false, true}) {
      for (const bool chunked : {false, true}) {
         auto test = fixture{secure, chunked, 5};
         for (const auto mode : modes) {
            BOOST_TEST_CONTEXT("TLS=" << secure << " chunked=" << chunked << " mode=" << static_cast<int>(mode)) {
               BOOST_CHECK_THROW(test.run(test.fetch(mode, {.max_response_body_bytes = 4})),
                                 http::exceptions::unavailable);
               BOOST_TEST(test.run(test.fetch(mode, {.max_response_body_bytes = 5})) == "xxxxx");
               BOOST_TEST(test.run(test.fetch(mode, {.max_response_body_bytes =
                                                         std::numeric_limits<std::uint64_t>::max()})) == "xxxxx");
            }
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(http_response_body_content_length_is_checked_before_body_reads) {
   for (const bool secure : {false, true}) {
      auto test = fixture{secure, false, 5, true};
      for (const auto mode : modes) {
         const auto options =
             http::request_options{.timeout = std::chrono::milliseconds{500}, .max_response_body_bytes = 4};
         if (mode == request_mode::download || mode == request_mode::duplex) {
            BOOST_CHECK_THROW(test.run(test.open_stream(mode, options)), http::exceptions::unavailable);
         } else {
            BOOST_CHECK_THROW(test.run(test.fetch(mode, options)), http::exceptions::unavailable);
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(http_response_body_zero_limit_accepts_empty_and_head_responses) {
   for (const bool secure : {false, true}) {
      for (const bool chunked : {false, true}) {
         auto empty = fixture{secure, chunked, 0};
         auto nonempty = fixture{secure, chunked, 1};
         for (const auto mode : modes) {
            BOOST_TEST(empty.run(empty.fetch(mode, {.max_response_body_bytes = 0})).empty());
            BOOST_CHECK_THROW(nonempty.run(nonempty.fetch(mode, {.max_response_body_bytes = 0})),
                              http::exceptions::unavailable);
            BOOST_TEST(nonempty.run(nonempty.head(mode)).empty());
            BOOST_TEST(
                nonempty
                    .run(nonempty.fetch(mode, {.max_response_body_bytes = 0}, "/no-body-204", http::method::post, 204))
                    .empty());
            BOOST_TEST(
                nonempty
                    .run(nonempty.fetch(mode, {.max_response_body_bytes = 0}, "/no-body-304", http::method::post, 304))
                    .empty());
         }
      }
   }
}

BOOST_AUTO_TEST_CASE(http_response_body_overflow_closes_buffered_connection_without_retry) {
   for (const bool secure : {false, true}) {
      for (const bool chunked : {false, true}) {
         auto test = fixture{secure, chunked, 128 * 1024};
         const auto options =
             http::request_options{.retry_idempotent = true, .max_retries = 2, .max_response_body_bytes = 1024};
         BOOST_CHECK_THROW(test.run(test.fetch(request_mode::buffered, options, "/body", http::method::get)),
                           http::exceptions::unavailable);
         BOOST_TEST(test.metrics().retry_attempts == 0U);
         BOOST_TEST(test.calls() == 1U);
         BOOST_TEST(test.run(test.fetch(request_mode::buffered, {.max_response_body_bytes = 128 * 1024}, "/body",
                                        http::method::get))
                        .size() == 128U * 1024);
         BOOST_TEST(test.calls() == 2U);
      }
   }
}

BOOST_AUTO_TEST_CASE(http_response_body_overflow_closes_stream_while_reader_remains_alive) {
   for (const bool secure : {false, true}) {
      for (const auto mode : {request_mode::download, request_mode::duplex}) {
         BOOST_TEST_CONTEXT("TLS=" << secure << " mode=" << static_cast<int>(mode)) {
            auto test = fixture{secure, true, 5, false, true};
            auto response = test.run(test.open_stream(mode, {.max_response_body_bytes = 4}));
            BOOST_CHECK_THROW(test.run(response.body.async_read_all()), http::exceptions::unavailable);
            BOOST_TEST(test.send_failed());
         }
      }
   }
}

} // namespace forge::tests::http_response_limits
