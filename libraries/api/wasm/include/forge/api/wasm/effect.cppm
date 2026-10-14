module;

#include <boost/describe.hpp>
#include <cstdint>
#include <string>

export module forge.api.wasm.effect;

export import forge.api.wasm.payload;

export namespace forge::api::wasm {

struct effect {
   std::string binding;
   payload request;
   std::string response_type;
   std::uint32_t after_ms = 0;

   bool operator==(const effect&) const = default;
};

BOOST_DESCRIBE_STRUCT(effect, (), (binding, request, response_type, after_ms))

} // namespace forge::api::wasm
