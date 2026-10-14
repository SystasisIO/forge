module;

#include <boost/describe.hpp>
#include <optional>
#include <string>
#include <vector>

export module forge.api.wasm.context;

export import forge.api.wasm.payload;

export namespace forge::api::wasm {

// Host-supplied data, not a verified credential.
struct subject_view {
   std::string issuer;
   std::string subject;

   bool operator==(const subject_view&) const = default;
};

struct context {
   std::optional<subject_view> subject;
   std::vector<payload> facts;

   bool operator==(const context&) const = default;
};

BOOST_DESCRIBE_STRUCT(subject_view, (), (issuer, subject))
BOOST_DESCRIBE_STRUCT(context, (), (subject, facts))

} // namespace forge::api::wasm
