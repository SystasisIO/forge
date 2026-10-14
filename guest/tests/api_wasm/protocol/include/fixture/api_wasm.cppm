module;

#include <boost/describe.hpp>
#include <cstdint>

export module fixture.api_wasm;

export import forge.api.wasm.step;
export import forge.api.wasm.event;
export import forge.api.wasm.context;
export import forge.api.wasm.http_command;

export namespace fixture {

struct request {
   std::uint32_t item = 0;
};

struct access_result {
   bool allowed = false;
};

BOOST_DESCRIBE_STRUCT(request, (), (item))
BOOST_DESCRIBE_STRUCT(access_result, (), (allowed))

}
