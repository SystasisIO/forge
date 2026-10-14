module;

#include <boost/describe.hpp>
#include <variant>

export module forge.api.wasm.event;

export import forge.api.core.types;
export import forge.api.wasm.payload;

export namespace forge::api::wasm {

struct event {
   std::variant<payload, core::error_payload> value;

   bool operator==(const event&) const = default;
};

BOOST_DESCRIBE_STRUCT(event, (), (value))

} // namespace forge::api::wasm
