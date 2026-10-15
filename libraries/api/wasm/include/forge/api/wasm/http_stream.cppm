module;

#include <boost/describe.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

export module forge.api.wasm.http_stream;

export import forge.api.wasm.http_command;

export namespace forge::api::wasm {

inline constexpr std::string_view http_stream_feature = "http.stream.v1";

// Host adapters own transport framing and decoding. Item semantics stay in the guest.
struct http_stream_open {
   http_command request;
   std::string event_type;
   std::string end_marker;

   bool operator==(const http_stream_open&) const = default;
};

// One yield may publish an item and request the next input, or finish the stream.
struct http_stream_read {
   std::optional<payload> emission;
   bool finish = false;

   bool operator==(const http_stream_read&) const = default;
};

struct http_stream_event {
   std::uint16_t status = 0;
   bool ended = false;
   std::optional<payload> item;

   bool operator==(const http_stream_event&) const = default;
};

BOOST_DESCRIBE_STRUCT(http_stream_open, (), (request, event_type, end_marker))
BOOST_DESCRIBE_STRUCT(http_stream_read, (), (emission, finish))
BOOST_DESCRIBE_STRUCT(http_stream_event, (), (status, ended, item))

} // namespace forge::api::wasm
