module;

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

export module forge.net.http.wire;

export import forge.net.http.types;
export import forge.net.http.exceptions;

export namespace forge::net::http {

struct wire_limits {
   std::size_t max_wire_bytes = 1024U * 1024U;
   std::size_t max_header_bytes = 16U * 1024U;
   std::size_t max_body_bytes = 1024U * 1024U;
   std::size_t max_headers = 128U;
};

// Half-open offsets into the exact serialized bytes, including every value
// byte and excluding the field name, colon, separating space and CRLF.
struct wire_range {
   std::size_t begin = 0;
   std::size_t end = 0;

   bool operator==(const wire_range&) const = default;
};

struct serialized_request {
   std::vector<std::uint8_t> bytes;
   std::vector<wire_range> sensitive;
   std::size_t body_begin = 0;
};

// Buffered requests use canonical Content-Length framing. Host must already
// be set. Sensitive names are case-insensitive and must be present.
// Reserves the complete measured byte size once before copying, avoiding
// segment-by-segment byte-vector growth.
serialized_request serialize_request(const request& source, std::span<const std::string_view> sensitive_headers = {},
                                     wire_limits limits = {});

// Parse exactly one complete, contiguous HTTP/1.0 or HTTP/1.1 message. These
// functions neither reconstruct gaps nor authenticate undisclosed bytes.
request parse_request(std::span<const std::uint8_t> bytes, wire_limits limits = {});
response parse_response(std::span<const std::uint8_t> bytes, method request_method = method::get,
                        wire_limits limits = {});

} // namespace forge::net::http
