module;

#include <boost/describe.hpp>
#include <string>
#include <vector>

export module forge.api.wasm.payload;

export import forge.raw.codec;

export namespace forge::api::wasm {

struct payload {
   std::string type;
   raw::bytes data;

   bool operator==(const payload&) const = default;
};

BOOST_DESCRIBE_STRUCT(payload, (), (type, data))

} // namespace forge::api::wasm
