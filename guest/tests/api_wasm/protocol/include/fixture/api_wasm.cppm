module;

#include <boost/describe.hpp>
#include <cstdint>
#include <optional>

export module fixture.api_wasm;

export import forge.api.wasm.step;
export import forge.api.wasm.event;
export import forge.api.wasm.context;
export import forge.api.wasm.http_command;
export import forge.api.wasm.http_stream;
export import forge.codec.json.document;

export namespace fixture {

struct request {
   std::uint32_t item = 0;
};

struct access_result {
   bool allowed = false;
};

struct json_request {
   forge::codec::json::document parameters;
   std::optional<forge::codec::json::document> choice;
};

struct stream_request {
   std::uint32_t limit = 2;
};

struct stream_item {
   std::uint32_t value = 0;
};

struct stream_state {
   std::uint32_t limit = 0;
   std::uint32_t received = 0;
   std::uint32_t sum = 0;
   bool finishing = false;
};

struct stream_summary {
   std::uint32_t count = 0;
   std::uint32_t sum = 0;
};

BOOST_DESCRIBE_STRUCT(request, (), (item))
BOOST_DESCRIBE_STRUCT(access_result, (), (allowed))
BOOST_DESCRIBE_STRUCT(stream_request, (), (limit))
BOOST_DESCRIBE_STRUCT(stream_item, (), (value))
BOOST_DESCRIBE_STRUCT(stream_state, (), (limit, received, sum, finishing))
BOOST_DESCRIBE_STRUCT(stream_summary, (), (count, sum))
}
