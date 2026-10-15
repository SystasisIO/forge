module;

#include <boost/describe.hpp>
#include <cstdint>
#include <optional>

export module fixture.api_wasm;

export import forge.api.wasm.step;
export import forge.api.wasm.event;
export import forge.api.wasm.context;
export import forge.api.wasm.http_command;
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

BOOST_DESCRIBE_STRUCT(request, (), (item))
BOOST_DESCRIBE_STRUCT(access_result, (), (allowed))

}
