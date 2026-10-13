module;

#include <boost/describe.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module forge.auth.credential.record;

export import forge.auth.credential.exceptions;

export namespace forge::auth::credential {

// The owner supplies trusted, non-decreasing times and owns source expiry/freshness.
using time_point = std::chrono::system_clock::time_point;
using scope_set = std::vector<std::string>;

struct id {
   std::string value;

   bool operator==(const id&) const = default;
};

struct binding {
   forge::auth::credential::id id;
   std::uint64_t generation = 0;

   bool operator==(const binding&) const = default;
};

enum class state {
   active = 0,
   revoked = 1,
};

struct record {
   forge::auth::credential::id id;
   std::string identity;
   scope_set scopes;
   std::uint64_t generation = 1;
   time_point issued_at{};
   time_point updated_at{};
   forge::auth::credential::state state = forge::auth::credential::state::active;
   std::optional<time_point> revoked_at;

   bool operator==(const record&) const = default;
};

void validate_id(const id& value);
void validate_identity(std::string_view identity);
void validate_scopes(const scope_set& scopes);
[[nodiscard]] scope_set canonicalize_scopes(scope_set scopes);
[[nodiscard]] bool scopes_subset_of(const scope_set& requested, const scope_set& baseline);
void validate_record(const record& value);
void validate_active(const record& value, time_point now);
void rotate_downscope(record& value, scope_set scopes, time_point now);
void revoke(record& value, time_point now);

BOOST_DESCRIBE_ENUM(state, active, revoked)

} // namespace forge::auth::credential
