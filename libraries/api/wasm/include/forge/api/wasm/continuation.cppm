module;

#include <boost/describe.hpp>
#include <cstdint>

export module forge.api.wasm.continuation;

export import forge.api.wasm.payload;

export namespace forge::api::wasm {

struct continuation {
   std::uint32_t version = 0;
   payload state;

   bool operator==(const continuation&) const = default;
};

BOOST_DESCRIBE_STRUCT(continuation, (), (version, state))

} // namespace forge::api::wasm
