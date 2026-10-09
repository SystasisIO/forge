module;

#include <boost/describe.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module forge.api.core.types;

export namespace forge::api::core {

using bytes = std::vector<std::uint8_t>;

struct metadata_entry {
   std::string key;
   std::string value;

   bool operator==(const metadata_entry&) const = default;
};

using metadata = std::vector<metadata_entry>;

struct api_id {
   std::string value;

   bool operator==(const api_id&) const = default;
};

struct api_version {
   std::uint16_t major = 1;
   std::uint16_t revision = 0;

   bool operator==(const api_version&) const = default;
};

struct api_ref {
   api_id id;
   std::uint16_t major = 1;
   std::uint16_t min_revision = 0;

   bool operator==(const api_ref&) const = default;
};

struct codec_id {
   std::string value;

   bool operator==(const codec_id&) const = default;
};

struct call_id {
   std::uint64_t value = 0;

   bool operator==(const call_id&) const = default;
};

enum class frame_kind : std::uint8_t {
   request = 1,
   response = 2,
   error = 3,
   cancel = 4,
   stream_item = 5,
   stream_end = 6,
   session_hello = 7,
   stream_window = 8,
};

enum class method_kind : std::uint8_t {
   unary = 1,
   server_stream = 2,
   client_stream = 3,
   bidirectional_stream = 4,
};

enum class stream_direction : std::uint8_t {
   input = 1,
   output = 2,
};

struct protocol_version {
   std::uint16_t major = 2;
   std::uint16_t minor = 0;

   bool operator==(const protocol_version&) const = default;
};

enum class capability : std::uint64_t {
   unary = 1ULL << 0U,
   server_stream = 1ULL << 1U,
   client_stream = 1ULL << 2U,
   bidirectional_stream = 1ULL << 3U,
   stream_window = 1ULL << 4U,
};

struct capability_set {
   std::uint64_t bits =
      static_cast<std::uint64_t>(capability::unary) |
      static_cast<std::uint64_t>(capability::server_stream) |
      static_cast<std::uint64_t>(capability::client_stream) |
      static_cast<std::uint64_t>(capability::bidirectional_stream) |
      static_cast<std::uint64_t>(capability::stream_window);

   [[nodiscard]] constexpr bool supports(capability value) const noexcept {
      const auto mask = static_cast<std::uint64_t>(value);
      return (bits & mask) == mask;
   }

   bool operator==(const capability_set&) const = default;
};

struct session_limits {
   std::uint32_t max_frame_bytes = 2U * 1024U * 1024U;
   std::uint32_t max_item_bytes = 1024U * 1024U;
   std::uint32_t max_inflight_calls = 128;
   std::uint32_t initial_window_items = 16;
   std::uint64_t initial_window_bytes = 1024U * 1024U;
   std::uint64_t max_buffered_bytes = 16U * 1024U * 1024U;
   std::uint32_t idle_timeout_ms = 60U * 1000U;
   std::uint32_t shutdown_grace_ms = 5U * 1000U;

   bool operator==(const session_limits&) const = default;
};

struct session_hello {
   protocol_version version;
   capability_set capabilities;
   codec_id codec{.value = "forge.raw"};
   session_limits limits;

   bool operator==(const session_hello&) const = default;
};

struct stream_window {
   stream_direction direction = stream_direction::input;
   std::uint64_t max_items = 0;
   std::uint64_t max_bytes = 0;

   bool operator==(const stream_window&) const = default;
};

struct stream_end {
   stream_direction direction = stream_direction::input;

   bool operator==(const stream_end&) const = default;
};

enum class surface : std::uint8_t {
   none = 0,
   local = 1,
   remote = 2,
};

[[nodiscard]] constexpr surface operator|(surface lhs, surface rhs) noexcept {
   return static_cast<surface>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr surface operator&(surface lhs, surface rhs) noexcept {
   return static_cast<surface>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

[[nodiscard]] constexpr bool supports(surface value, surface expected) noexcept {
   return static_cast<std::uint8_t>(value & expected) == static_cast<std::uint8_t>(expected);
}

enum class status : std::uint16_t {
   ok = 0,
   invalid_argument = 400,
   unauthenticated = 401,
   permission_denied = 403,
   not_found = 404,
   conflict = 409,
   failed_precondition = 412,
   resource_exhausted = 429,
   deadline_exceeded = 504,
   unavailable = 503,
   internal = 500,
   bad_gateway = 502,
};

struct error_identity {
   std::string category;
   std::uint32_t code = 0;

   bool operator==(const error_identity&) const = default;
};

struct error_payload {
   std::string error;
   std::string message;
   bool retryable = false;
   status status_code = status::internal;
   error_identity identity;
   std::optional<codec_id> details_codec;
   std::optional<bytes> details;

   bool operator==(const error_payload&) const = default;
};

struct request {
   api_ref api;
   std::string method;
   metadata meta;
   codec_id codec;
   bytes body;

   bool operator==(const request&) const = default;
};

struct response {
   api_ref api;
   std::string method;
   metadata meta;
   codec_id codec;
   bytes body;
   std::optional<error_payload> error;

   bool operator==(const response&) const = default;
};

struct frame {
   frame_kind kind = frame_kind::request;
   call_id id;
   api_ref api;
   std::string method;
   metadata meta;
   codec_id codec;
   bytes payload;

   bool operator==(const frame&) const = default;
};

BOOST_DESCRIBE_ENUM(frame_kind, request, response, error, cancel, stream_item, stream_end, session_hello, stream_window)
BOOST_DESCRIBE_ENUM(method_kind, unary, server_stream, client_stream, bidirectional_stream)
BOOST_DESCRIBE_ENUM(stream_direction, input, output)
BOOST_DESCRIBE_ENUM(capability, unary, server_stream, client_stream, bidirectional_stream, stream_window)
BOOST_DESCRIBE_ENUM(surface, none, local, remote)
BOOST_DESCRIBE_ENUM(status, ok, invalid_argument, unauthenticated, permission_denied, not_found, conflict,
                    failed_precondition, resource_exhausted, deadline_exceeded, unavailable, internal, bad_gateway)
BOOST_DESCRIBE_STRUCT(api_id, (), (value))
BOOST_DESCRIBE_STRUCT(api_version, (), (major, revision))
BOOST_DESCRIBE_STRUCT(api_ref, (), (id, major, min_revision))
BOOST_DESCRIBE_STRUCT(codec_id, (), (value))
BOOST_DESCRIBE_STRUCT(call_id, (), (value))
BOOST_DESCRIBE_STRUCT(protocol_version, (), (major, minor))
BOOST_DESCRIBE_STRUCT(capability_set, (), (bits))
BOOST_DESCRIBE_STRUCT(session_limits, (),
                      (max_frame_bytes, max_item_bytes, max_inflight_calls,
                       initial_window_items, initial_window_bytes,
                       max_buffered_bytes, idle_timeout_ms, shutdown_grace_ms))
BOOST_DESCRIBE_STRUCT(session_hello, (), (version, capabilities, codec, limits))
BOOST_DESCRIBE_STRUCT(stream_window, (), (direction, max_items, max_bytes))
BOOST_DESCRIBE_STRUCT(stream_end, (), (direction))
BOOST_DESCRIBE_STRUCT(metadata_entry, (), (key, value))
BOOST_DESCRIBE_STRUCT(error_identity, (), (category, code))
BOOST_DESCRIBE_STRUCT(error_payload, (), (error, message, retryable, status_code, identity, details_codec, details))
BOOST_DESCRIBE_STRUCT(request, (), (api, method, meta, codec, body))
BOOST_DESCRIBE_STRUCT(response, (), (api, method, meta, codec, body, error))
BOOST_DESCRIBE_STRUCT(frame, (), (kind, id, api, method, meta, codec, payload))

} // namespace forge::api::core
