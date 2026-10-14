module;

#include <boost/describe.hpp>
#include <variant>

export module forge.api.wasm.step;

export import forge.api.core.types;
export import forge.api.wasm.continuation;
export import forge.api.wasm.effect;

export namespace forge::api::wasm {

struct awaiting {
   effect command;
   continuation next;

   bool operator==(const awaiting&) const = default;
};

struct completed {
   payload output;

   bool operator==(const completed&) const = default;
};

struct step {
   std::variant<awaiting, completed, core::error_payload> result;

   bool operator==(const step&) const = default;
};

BOOST_DESCRIBE_STRUCT(awaiting, (), (command, next))
BOOST_DESCRIBE_STRUCT(completed, (), (output))
BOOST_DESCRIBE_STRUCT(step, (), (result))

} // namespace forge::api::wasm
