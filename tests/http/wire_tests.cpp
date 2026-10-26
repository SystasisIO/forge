#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

import forge.net.http.wire;

namespace {

std::vector<std::uint8_t> bytes(std::string_view value) {
   return {value.begin(), value.end()};
}

std::string text(std::span<const std::uint8_t> value) {
   return {reinterpret_cast<const char*>(value.data()), value.size()};
}

forge::net::http::request request() {
   auto value = forge::net::http::request{forge::net::http::method::post, "/v1/tasks", 11};
   value.set(forge::net::http::field::host, "source.test");
   value.set("x-auth", "Bearer private-key");
   value.keep_alive(false);
   value.body() = "{\"a\":1}";
   return value;
}

} // namespace

BOOST_AUTO_TEST_CASE(http_wire_request_golden_and_exact_sensitive_ranges) {
   const auto names = std::array<std::string_view, 1>{"X-Auth"};
   const auto value = forge::net::http::serialize_request(request(), names);
   const auto expected = std::string{"POST /v1/tasks HTTP/1.1\r\nHost: source.test\r\nx-auth: Bearer private-key\r\n"
                                     "Connection: close\r\nContent-Length: 7\r\n\r\n{\"a\":1}"};
   BOOST_TEST(text(value.bytes) == expected);
   BOOST_TEST(value.bytes.capacity() == value.bytes.size());
   BOOST_TEST(value.body_begin == expected.find("\r\n\r\n") + 4);
   BOOST_REQUIRE_EQUAL(value.sensitive.size(), 1U);
   BOOST_TEST(value.sensitive[0].begin == expected.find("Bearer private-key"));
   BOOST_TEST(value.sensitive[0].end == expected.find("\r\n", value.sensitive[0].begin));
   BOOST_TEST(text(std::span{value.bytes}.subspan(value.sensitive[0].begin,
                                                  value.sensitive[0].end - value.sensitive[0].begin)) ==
              "Bearer private-key");
   const auto parsed = forge::net::http::parse_request(value.bytes);
   BOOST_TEST(parsed.body() == "{\"a\":1}");
   BOOST_TEST(parsed.target() == "/v1/tasks");
   BOOST_CHECK(parsed.method() == forge::net::http::method::post);
   BOOST_CHECK(!parsed.keep_alive());
}

BOOST_AUTO_TEST_CASE(http_wire_serializer_retains_exact_capacity_for_segmented_header_and_binary_body) {
   const auto names = std::array<std::string_view, 1>{"X-Auth"};
   for (const auto size : std::array<std::size_t, 12>{0, 1, 7, 127, 128, 255, 256, 511, 512, 4096, 16384, 32768}) {
      auto source = request();
      source.set("X-Segment-A", std::string(29, 'a'));
      source.set("X-Segment-B", std::string(71, 'b'));
      auto body = std::string(size, 'x');
      if (size > 2) {
         body[size / 2] = '\0';
      }
      source.body() = body;
      const auto baseline = forge::net::http::serialize_request(source, names);
      const auto value = forge::net::http::serialize_request(
          source, names, {.max_wire_bytes = baseline.bytes.size(), .max_body_bytes = size});
      BOOST_TEST(value.bytes.capacity() == value.bytes.size());
      BOOST_TEST(value.bytes.size() == baseline.bytes.size());
      BOOST_CHECK_EQUAL_COLLECTIONS(value.bytes.begin(), value.bytes.end(), baseline.bytes.begin(), baseline.bytes.end());
      BOOST_TEST(value.body_begin + body.size() == value.bytes.size());
      BOOST_TEST(source.body() == body);
      BOOST_REQUIRE_EQUAL(value.sensitive.size(), 1U);
      const auto& range = value.sensitive.front();
      BOOST_TEST(range.end < value.body_begin);
      BOOST_TEST(text(std::span{value.bytes}.subspan(range.begin, range.end - range.begin)) == "Bearer private-key");
      const auto parsed = forge::net::http::parse_request(value.bytes);
      BOOST_TEST(parsed.body() == body);
      BOOST_TEST(parsed["X-Segment-A"] == std::string(29, 'a'));
      BOOST_TEST(parsed["X-Segment-B"] == std::string(71, 'b'));
      BOOST_TEST(parsed.target() == "/v1/tasks");
      BOOST_CHECK_THROW(forge::net::http::serialize_request(
                            source, names, {.max_wire_bytes = value.bytes.size() - 1, .max_body_bytes = size}),
                        forge::net::http::exceptions::payload_too_large);
   }
}

BOOST_AUTO_TEST_CASE(http_wire_sensitive_ranges_are_field_offsets_and_cover_repeated_custom_fields) {
   auto source = request();
   source.insert("X-Custom", "first");
   source.insert("x-custom", "second");
   source.insert("X-Empty", "");
   source.body() = "Bearer private-key first second";
   const auto names = std::array<std::string_view, 3>{"x-AUTH", "X-CUSTOM", "x-empty"};
   const auto value = forge::net::http::serialize_request(source, names);
   BOOST_REQUIRE_EQUAL(value.sensitive.size(), 4U);
   auto previous = std::size_t{};
   const auto expected = std::array<std::string_view, 4>{"Bearer private-key", "first", "second", ""};
   for (auto index = std::size_t{}; index < value.sensitive.size(); ++index) {
      const auto& range = value.sensitive[index];
      BOOST_TEST(range.begin >= previous);
      BOOST_TEST(range.end < value.body_begin);
      BOOST_TEST(text(std::span{value.bytes}.subspan(range.begin, range.end - range.begin)) == expected[index]);
      previous = range.end;
   }
   BOOST_TEST(text(std::span{value.bytes}.subspan(value.body_begin)) == source.body());
}

BOOST_AUTO_TEST_CASE(http_wire_serializer_rejects_injection_before_beast_normalization) {
   auto source = request();
   source.set("x-auth", "Bearer token\r\nX-Evil: injected");
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source), forge::net::http::exceptions::bad_request);
   source = request();
   source.insert("X-Header\r\nInjected", "value");
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source), forge::net::http::exceptions::bad_request);
   source = request();
   source.target("/path\r\nX-Evil: value");
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source), forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_wire_serializer_rejects_ambiguous_or_mismatching_framing) {
   auto source = request();
   source.insert("Content-Length", "7");
   source.insert("content-length", "7");
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source), forge::net::http::exceptions::bad_request);
   source = request();
   source.set("Content-Length", "1");
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source), forge::net::http::exceptions::bad_request);
   source = request();
   source.set("Transfer-Encoding", "chunked");
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source), forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_wire_serializer_sensitive_names_are_case_insensitive_and_fail_closed) {
   const auto missing = std::array<std::string_view, 1>{"Missing-Authorization"};
   BOOST_CHECK_THROW(forge::net::http::serialize_request(request(), missing),
                     forge::net::http::exceptions::bad_request);
   const auto repeated = std::array<std::string_view, 2>{"x-auth", "X-AUTH"};
   BOOST_CHECK_THROW(forge::net::http::serialize_request(request(), repeated),
                     forge::net::http::exceptions::bad_request);
   const auto malformed = std::array<std::string_view, 1>{"bad\r\nname"};
   BOOST_CHECK_THROW(forge::net::http::serialize_request(request(), malformed),
                     forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_wire_parser_preserves_binary_body_and_custom_response_headers) {
   auto wire = std::string{"HTTP/1.1 201 Created\r\nX-Task: id-42\r\nContent-Length: 3\r\n\r\n"};
   wire.append("a\0b", 3);
   const auto parsed = forge::net::http::parse_response(bytes(wire));
   BOOST_TEST(parsed.result_int() == 201U);
   BOOST_TEST(parsed["X-Task"] == "id-42");
   BOOST_TEST(parsed.body() == std::string("a\0b", 3));
}

BOOST_AUTO_TEST_CASE(http_wire_parser_supports_complete_chunked_and_eof_delimited_responses) {
   const auto chunked = bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nab\r\n1\r\nc\r\n0\r\n\r\n");
   BOOST_TEST(forge::net::http::parse_response(chunked).body() == "abc");
   const auto closed = bytes("HTTP/1.0 200 OK\r\nConnection: close\r\n\r\nabc");
   BOOST_TEST(forge::net::http::parse_response(closed).body() == "abc");
   const auto request_bytes = bytes("POST / HTTP/1.1\r\nHost: source.test\r\nTransfer-Encoding: chunked\r\n\r\n"
                                    "3\r\nabc\r\n0\r\n\r\n");
   BOOST_TEST(forge::net::http::parse_request(request_bytes).body() == "abc");
}

BOOST_AUTO_TEST_CASE(http_wire_parser_rejects_every_truncation_of_a_framed_response) {
   const auto complete = bytes("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
   for (auto size = std::size_t{}; size < complete.size(); ++size) {
      BOOST_CHECK_THROW(forge::net::http::parse_response(std::span{complete}.first(size)),
                        forge::net::http::exceptions::bad_request);
   }
   const auto truncated = bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n");
   BOOST_CHECK_THROW(forge::net::http::parse_response(truncated), forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_wire_parser_rejects_trailing_pipelined_or_interim_messages) {
   const auto cases = std::array{
       "HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nab",
       "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\nHTTP/1.1 200 OK\r\n\r\n", "HTTP/1.1 100 Continue\r\n\r\n",
       "HTTP/1.1 100 Continue\r\n\r\nHTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n",
       "HTTP/1.1 101 Switching Protocols\r\nConnection: Upgrade\r\nUpgrade: websocket\r\n\r\n"};
   for (const auto value : cases) {
      BOOST_CHECK_THROW(forge::net::http::parse_response(bytes(value)), forge::net::http::exceptions::bad_request);
   }
   BOOST_CHECK_THROW(forge::net::http::parse_request(bytes("GET / HTTP/1.1\r\nHost: source.test\r\n\r\nextra")),
                     forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_wire_parser_rejects_duplicate_framing_and_unsupported_transfer_codings) {
   const auto fields = std::array{"Content-Length: 0\r\nContent-Length: 0\r\n",
                                  "Content-Length: 0\r\ncontent-length: 1\r\n",
                                  "Content-Length: 0, 0\r\n",
                                  "Content-Length: +0\r\n",
                                  "Transfer-Encoding: chunked\r\nContent-Length: 0\r\n",
                                  "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
                                  "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked\r\n",
                                  "Transfer-Encoding: gzip, chunked\r\n",
                                  "Transfer-Encoding: gzip\r\n"};
   for (const auto field : fields) {
      BOOST_CHECK_THROW(forge::net::http::parse_response(bytes(std::string{"HTTP/1.1 200 OK\r\n"} + field + "\r\n")),
                        forge::net::http::exceptions::bad_request);
      BOOST_CHECK_THROW(forge::net::http::parse_request(
                            bytes(std::string{"POST / HTTP/1.1\r\nHost: source.test\r\n"} + field + "\r\n")),
                        forge::net::http::exceptions::bad_request);
   }
}

BOOST_AUTO_TEST_CASE(http_wire_parser_rejects_obs_fold_bare_lf_bad_host_and_lossy_method_or_version) {
   const auto cases = std::array{"GET / HTTP/1.1\nHost: source.test\n\n",
                                 "GET / HTTP/1.1\r\nHost: source.test\r\nX-Field: first\r\n second\r\n\r\n",
                                 "GET / HTTP/1.1\r\nHost: source.test\nX-Evil: value\r\n\r\n",
                                 "GET / HTTP/1.1\r\n\r\n",
                                 "GET / HTTP/1.1\r\nHost: \t\r\n\r\n",
                                 "GET / HTTP/1.1\r\nHost: source.test\r\nHOST: other.test\r\n\r\n",
                                 "CONNECT source.test:443 HTTP/1.1\r\nHost: source.test\r\n\r\n",
                                 "GET / HTTP/2.0\r\nHost: source.test\r\n\r\n"};
   for (const auto value : cases) {
      BOOST_CHECK_THROW(forge::net::http::parse_request(bytes(value)), forge::net::http::exceptions::bad_request);
   }
}

BOOST_AUTO_TEST_CASE(http_wire_response_head_and_no_body_semantics_are_explicit) {
   const auto head = bytes("HTTP/1.1 200 OK\r\nContent-Length: 9999\r\n\r\n");
   BOOST_TEST(
       forge::net::http::parse_response(head, forge::net::http::method::head, {.max_body_bytes = 0}).body().empty());
   BOOST_CHECK_THROW(forge::net::http::parse_response(head), forge::net::http::exceptions::bad_request);
   BOOST_CHECK_THROW(forge::net::http::parse_response(bytes("HTTP/1.1 200 OK\r\nContent-Length: 1\r\n\r\nx"),
                                                      forge::net::http::method::head),
                     forge::net::http::exceptions::bad_request);
   BOOST_TEST(forge::net::http::parse_response(bytes("HTTP/1.1 204 No Content\r\n\r\n")).body().empty());
   BOOST_CHECK_THROW(forge::net::http::parse_response(bytes("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n")),
                     forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_wire_parser_rejects_nonempty_chunk_trailers) {
   BOOST_CHECK_THROW(forge::net::http::parse_response(
                         bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nX-Trailer: value\r\n\r\n")),
                     forge::net::http::exceptions::bad_request);
   BOOST_CHECK_THROW(forge::net::http::parse_response(
                         bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-Hidden\r\n\r\n"
                               "0\r\nX-Hidden: ignored-by-default-parser\r\n\r\n")),
                     forge::net::http::exceptions::bad_request);
   BOOST_CHECK_THROW(forge::net::http::parse_response(bytes(
                         "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nETag: original\r\nTrailer: ETag\r\n\r\n"
                         "0\r\nETag: replacement\r\n\r\n")),
                     forge::net::http::exceptions::bad_request);
}

BOOST_AUTO_TEST_CASE(http_wire_limits_bound_complete_message_header_count_and_decoded_body) {
   const auto wire = bytes("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc");
   BOOST_TEST(forge::net::http::parse_response(wire, forge::net::http::method::get,
                                               {.max_wire_bytes = wire.size(), .max_body_bytes = 3})
                  .body() == "abc");
   BOOST_CHECK_THROW(
       forge::net::http::parse_response(wire, forge::net::http::method::get, {.max_wire_bytes = wire.size() - 1}),
       forge::net::http::exceptions::payload_too_large);
   BOOST_CHECK_THROW(forge::net::http::parse_response(wire, forge::net::http::method::get, {.max_body_bytes = 2}),
                     forge::net::http::exceptions::payload_too_large);
   BOOST_CHECK_THROW(forge::net::http::parse_response(wire, forge::net::http::method::get, {.max_header_bytes = 8}),
                     forge::net::http::exceptions::request_header_fields_too_large);
   BOOST_CHECK_THROW(forge::net::http::parse_response(bytes("HTTP/1.1 200 OK\r\nX-A: 1\r\nContent-Length: 0\r\n\r\n"),
                                                      forge::net::http::method::get, {.max_headers = 1}),
                     forge::net::http::exceptions::request_header_fields_too_large);
   BOOST_CHECK_THROW(forge::net::http::parse_response(
                         bytes("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n"),
                         forge::net::http::method::get, {.max_body_bytes = 2}),
                     forge::net::http::exceptions::payload_too_large);
}

BOOST_AUTO_TEST_CASE(http_wire_serializer_limits_cover_generated_headers_and_exact_wire_boundary) {
   const auto source = request();
   const auto encoded = forge::net::http::serialize_request(source);
   BOOST_TEST(forge::net::http::serialize_request(source, {}, {.max_wire_bytes = encoded.bytes.size()}).bytes.size() ==
              encoded.bytes.size());
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source, {}, {.max_wire_bytes = encoded.bytes.size() - 1}),
                     forge::net::http::exceptions::payload_too_large);
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source, {}, {.max_body_bytes = 6}),
                     forge::net::http::exceptions::payload_too_large);
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source, {}, {.max_headers = 2}),
                     forge::net::http::exceptions::request_header_fields_too_large);
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source, {}, {.max_header_bytes = 16}),
                     forge::net::http::exceptions::request_header_fields_too_large);
}

BOOST_AUTO_TEST_CASE(http_wire_serializer_checks_combined_sizes_and_generated_fields_before_copy) {
   auto source = request();
   source.insert("X-Large", std::string(1024U * 1024U, 'x'));
   BOOST_CHECK_THROW(forge::net::http::serialize_request(
                         source, {}, {.max_wire_bytes = 1024, .max_header_bytes = 2U * 1024U * 1024U}),
                     forge::net::http::exceptions::payload_too_large);
   source = request();
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source, {}, {.max_headers = 3}),
                     forge::net::http::exceptions::request_header_fields_too_large);
   const auto encoded = forge::net::http::serialize_request(source);
   BOOST_CHECK_THROW(forge::net::http::serialize_request(source, {}, {.max_header_bytes = encoded.body_begin - 1}),
                     forge::net::http::exceptions::request_header_fields_too_large);
}
