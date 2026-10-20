module;

#include "details/beast_message.hxx"
#include "details/complete_parser.hxx"

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <boost/asio/buffer.hpp>
#include <boost/beast/core/buffers_range.hpp>
#include <boost/beast/http.hpp>
#include <boost/system/error_code.hpp>

module forge.net.http.wire;

namespace forge::net::http {
namespace {

std::string_view as_text(std::span<const std::uint8_t> bytes) noexcept {
   if (bytes.empty()) {
      return {};
   }
   return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

void validate_limits(const wire_limits& limits) {
   if (limits.max_header_bytes == 0 || limits.max_header_bytes > std::numeric_limits<std::uint32_t>::max() ||
       limits.max_headers == 0) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid HTTP wire limits");
   }
}

bool token_character(unsigned char value) noexcept {
   return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
          value == '!' || value == '#' || value == '$' || value == '%' || value == '&' || value == '\'' ||
          value == '*' || value == '+' || value == '-' || value == '.' || value == '^' || value == '_' ||
          value == '`' || value == '|' || value == '~';
}

bool valid_name(std::string_view name) noexcept {
   return !name.empty() && std::ranges::all_of(name, [](unsigned char byte) { return token_character(byte); });
}

bool valid_value(std::string_view value) noexcept {
   return std::ranges::none_of(value, [](unsigned char byte) { return (byte < 0x20 && byte != '\t') || byte == 0x7f; });
}

std::string_view trim_space(std::string_view value) noexcept {
   const auto begin = value.find_first_not_of(" \t");
   if (begin == std::string_view::npos) {
      return {};
   }
   return value.substr(begin, value.find_last_not_of(" \t") - begin + 1);
}

std::uint64_t content_length(std::string_view value) {
   value = trim_space(value);
   if (value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid HTTP Content-Length");
   }
   auto length = std::uint64_t{};
   const auto converted = std::from_chars(value.data(), value.data() + value.size(), length);
   if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid HTTP Content-Length");
   }
   return length;
}

struct head_layout {
   std::size_t body_begin = 0;
   std::size_t fields = 0;
};

// Beast owns message parsing. This contiguous-header check rejects obsolete
// folding and duplicate framing which Beast may otherwise normalize, and maps
// offsets only in bytes actually produced by the local serializer.
head_layout inspect_head(std::string_view bytes, const wire_limits& limits, bool is_request,
                         std::span<const std::string_view> sensitive_headers = {},
                         std::vector<wire_range>* sensitive = nullptr) {
   const auto end = bytes.find("\r\n\r\n");
   if (end == std::string_view::npos) {
      if (bytes.size() > limits.max_header_bytes) {
         FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire header exceeds its limit");
      }
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Incomplete HTTP wire header");
   }
   if (end + 4 > limits.max_header_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire header exceeds its limit");
   }

   const auto first = bytes.find("\r\n");
   if (first == 0 || first == std::string_view::npos ||
       bytes.substr(0, first).find_first_of("\r\n") != std::string_view::npos) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid HTTP wire start line");
   }
   auto layout = head_layout{.body_begin = end + 4};
   auto lengths = std::size_t{};
   auto transfers = std::size_t{};
   auto hosts = std::size_t{};
   for (auto position = first + 2; position < end + 2;) {
      const auto line_end = bytes.find("\r\n", position);
      const auto line = bytes.substr(position, line_end - position);
      const auto colon = line.find(':');
      if (line.empty() || line.front() == ' ' || line.front() == '\t' || colon == std::string_view::npos ||
          !valid_name(line.substr(0, colon)) || !valid_value(line.substr(colon + 1))) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid HTTP wire field");
      }
      if (++layout.fields > limits.max_headers) {
         FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire field count exceeds its limit");
      }
      const auto name = line.substr(0, colon);
      const auto value = trim_space(line.substr(colon + 1));
      if (header_name_equal(name, "Content-Length")) {
         if (++lengths != 1 || transfers != 0) {
            FORGE_THROW_EXCEPTION(exceptions::bad_request, "Ambiguous HTTP wire framing");
         }
         (void)content_length(value);
      }
      if (header_name_equal(name, "Transfer-Encoding")) {
         if (++transfers != 1 || lengths != 0 || !header_name_equal(value, "chunked")) {
            FORGE_THROW_EXCEPTION(exceptions::bad_request, "Unsupported or ambiguous HTTP transfer framing");
         }
      }
      if (is_request && header_name_equal(name, "Host")) {
         if (++hosts != 1 || value.empty()) {
            FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP wire request requires exactly one nonempty Host");
         }
      }
      if (sensitive && std::ranges::any_of(sensitive_headers,
                                           [&](std::string_view entry) { return header_name_equal(name, entry); })) {
         auto begin = position + colon + 1;
         if (begin < line_end && bytes[begin] == ' ') {
            ++begin;
         }
         sensitive->push_back({begin, line_end});
      }
      position = line_end + 2;
   }
   if (is_request && hosts != 1) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP wire request requires exactly one nonempty Host");
   }
   return layout;
}

void check_parser_error(const boost::system::error_code& error) {
   if (error == boost::beast::http::error::body_limit) {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP wire body exceeds its limit");
   }
   if (error == boost::beast::http::error::header_limit) {
      FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire header exceeds its limit");
   }
   if (error) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid or incomplete HTTP wire message");
   }
}

template <bool IsRequest>
boost::beast::http::message<IsRequest, boost::beast::http::string_body>
parse_message(std::span<const std::uint8_t> bytes, const wire_limits& limits, bool skip_body = false) {
   validate_limits(limits);
   if (bytes.size() > limits.max_wire_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP wire message exceeds its limit");
   }
   const auto layout = inspect_head(as_text(bytes), limits, IsRequest);
   auto parser = detail::complete_parser<IsRequest>{};
   parser.header_limit(static_cast<std::uint32_t>(limits.max_header_bytes));
   parser.body_limit(limits.max_body_bytes);
   parser.eager(true);
   parser.skip(skip_body);
   auto error = boost::system::error_code{};
   const auto used = parser.put(boost::asio::buffer(bytes.data(), bytes.size()), error);
   if (error == boost::beast::http::error::need_more) {
      error.clear();
   }
   check_parser_error(error);
   if (!parser.is_done()) {
      parser.put_eof(error);
      check_parser_error(error);
   }
   if (!parser.is_done() || used != bytes.size()) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP wire message contains trailing or incomplete data");
   }
   const auto& message = parser.get();
   if (message.version() != 10 && message.version() != 11) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Unsupported HTTP wire version");
   }
   if (static_cast<std::size_t>(std::distance(message.begin(), message.end())) != layout.fields) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP wire trailers are unsupported");
   }
   return parser.release();
}

void validate_serialization(const request& source, std::span<const std::string_view> sensitive_headers,
                            const wire_limits& limits) {
   validate_limits(limits);
   if (source.body().size() > limits.max_body_bytes || source.body().size() > limits.max_wire_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP wire body exceeds its limit");
   }
   if (source.target().size() > limits.max_header_bytes || sensitive_headers.size() > limits.max_headers) {
      FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire header exceeds its limit");
   }
   if (source.target().size() > limits.max_wire_bytes - source.body().size()) {
      FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP wire message exceeds its limit");
   }
   if (source.target().empty() || detail::to_beast_method(source.method()) == boost::beast::http::verb::unknown ||
       std::ranges::any_of(source.target(), [](unsigned char byte) { return byte <= 0x20 || byte == 0x7f; }) ||
       (source.version() != 10 && source.version() != 11)) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid HTTP wire request line");
   }
   const auto headers = source.header_fields();
   if (headers.size() > limits.max_headers) {
      FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire field count exceeds its limit");
   }
   auto header_remaining = limits.max_header_bytes;
   auto wire_remaining = limits.max_wire_bytes - source.body().size();
   const auto consume = [&](std::size_t size) {
      if (size > wire_remaining) {
         FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP wire message exceeds its limit");
      }
      if (size > header_remaining) {
         FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire header exceeds its limit");
      }
      wire_remaining -= size;
      header_remaining -= size;
   };
   consume(boost::beast::http::to_string(detail::to_beast_method(source.method())).size());
   consume(source.target().size());
   consume(14); // Two spaces, HTTP/1.x, start-line CRLF and final header CRLF.
   auto lengths = std::size_t{};
   auto connections = std::size_t{};
   auto hosts = std::size_t{};
   for (const auto& header : headers) {
      consume(header.name.size());
      consume(header.text.size());
      consume(4); // Colon, separating space and CRLF.
      if (!valid_name(header.name) || !valid_value(header.text)) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid HTTP wire field");
      }
      if (header_name_equal(header.name, "Transfer-Encoding")) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "Buffered HTTP wire serialization requires Content-Length");
      }
      if (header_name_equal(header.name, "Content-Length") &&
          (++lengths != 1 || content_length(header.text) != source.body().size())) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid buffered HTTP wire framing");
      }
      if (header_name_equal(header.name, "Host") && (++hosts != 1 || trim_space(header.text).empty())) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP wire request requires exactly one nonempty Host");
      }
      if (header_name_equal(header.name, "Connection")) {
         ++connections;
         // Beast may normalize a caller-supplied token list and add its
         // keep-alive/close token. Bound that growth before copying any field.
         // This deliberately conservative bound does not parse/reimplement
         // Beast's Connection rewriting rules.
         consume(header.text.size());
         consume(12);
      }
   }
   if (hosts != 1) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP wire request requires exactly one nonempty Host");
   }
   auto fields = headers.size();
   if (lengths == 0 && (!source.body().empty() || source.method() == method::options ||
                        source.method() == method::put || source.method() == method::post)) {
      consume(18); // Content-Length, colon, space and CRLF.
      consume(std::to_string(source.body().size()).size());
      ++fields;
   }
   if (connections == 0 &&
       ((source.version() == 11 && !source.keep_alive()) || (source.version() == 10 && source.keep_alive()))) {
      consume(source.keep_alive() ? 24 : 19); // Complete generated Connection field.
      ++fields;
   }
   if (fields > limits.max_headers) {
      FORGE_THROW_EXCEPTION(exceptions::request_header_fields_too_large, "HTTP wire field count exceeds its limit");
   }
   for (auto index = std::size_t{}; index < sensitive_headers.size(); ++index) {
      const auto name = sensitive_headers[index];
      if (name.size() > limits.max_header_bytes || !valid_name(name) ||
          std::ranges::none_of(headers, [&](const auto& header) { return header_name_equal(header.name, name); }) ||
          std::ranges::any_of(sensitive_headers.first(index),
                              [&](std::string_view prior) { return header_name_equal(prior, name); })) {
         FORGE_THROW_EXCEPTION(exceptions::bad_request, "Invalid or missing sensitive HTTP wire field");
      }
   }
}

} // namespace

serialized_request serialize_request(const request& source, std::span<const std::string_view> sensitive_headers,
                                     wire_limits limits) {
   validate_serialization(source, sensitive_headers, limits);
   auto message = detail::to_beast_request(source);
   auto serializer = boost::beast::http::request_serializer<boost::beast::http::string_body>{message};
   auto output = serialized_request{};
   while (!serializer.is_done()) {
      auto copied = std::size_t{};
      auto error = boost::system::error_code{};
      serializer.next(error, [&](boost::system::error_code&, const auto& buffers) {
         for (const auto& buffer : boost::beast::buffers_range_ref(buffers)) {
            if (buffer.size() > limits.max_wire_bytes - output.bytes.size()) {
               FORGE_THROW_EXCEPTION(exceptions::payload_too_large, "HTTP wire message exceeds its limit");
            }
            const auto* begin = static_cast<const std::uint8_t*>(buffer.data());
            output.bytes.insert(output.bytes.end(), begin, begin + buffer.size());
            copied += buffer.size();
         }
      });
      check_parser_error(error);
      serializer.consume(copied);
   }
   // Inspect generated framing and header offsets without parsing/copying the
   // already bounded body a second time.
   output.body_begin =
       inspect_head(as_text(output.bytes), limits, true, sensitive_headers, &output.sensitive).body_begin;
   return output;
}

request parse_request(std::span<const std::uint8_t> bytes, wire_limits limits) {
   auto message = parse_message<true>(bytes, limits);
   if (detail::to_http_method(message.method()) == method::unknown) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Unsupported HTTP wire method");
   }
   return detail::to_http_request(std::move(message));
}

response parse_response(std::span<const std::uint8_t> bytes, method request_method, wire_limits limits) {
   if (detail::to_beast_method(request_method) == boost::beast::http::verb::unknown) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "Unknown HTTP wire response request method");
   }
   auto message = parse_message<false>(bytes, limits, request_method == method::head);
   if (message.result_int() < 200 || message.result_int() > 599) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP wire response requires a final status");
   }
   if (message.result_int() == 204 && (message.count(boost::beast::http::field::content_length) != 0 ||
                                       message.count(boost::beast::http::field::transfer_encoding) != 0)) {
      FORGE_THROW_EXCEPTION(exceptions::bad_request, "HTTP no-content response contains prohibited framing");
   }
   return detail::to_http_response(std::move(message));
}

} // namespace forge::net::http
