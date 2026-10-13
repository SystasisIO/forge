module;

#include <boost/describe.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module forge.auth.pairing.types;

export import forge.auth.credential.record;
export import forge.crypto.core.secret_string;
export import forge.crypto.digest.sha256;

export namespace forge::auth::pairing {

// Callers supply trusted, non-decreasing wall-clock values; persisted records retain them across restart.
using time_point = std::chrono::system_clock::time_point;

struct token_digest {
   forge::crypto::digest::sha256 value;

   bool operator==(const token_digest&) const = default;
};

struct bootstrap_options {
   time_point now{};
   time_point expires_at{};
   forge::auth::credential::scope_set scope_baseline;
};

struct bootstrap_record {
   token_digest digest;
   forge::auth::credential::scope_set scope_baseline;
   time_point created_at{};
   time_point expires_at{};
   bool consumed = false;

   bool operator==(const bootstrap_record&) const = default;
};

struct bootstrap_issuance {
   bootstrap_record record;
   forge::crypto::core::secret_string token;
};

struct pairing_request {
   std::string identity;
   forge::auth::credential::scope_set requested_scopes;
};

struct consume_options {
   time_point now{};
   time_point request_expires_at{};
   std::size_t pending_count = 0;
   std::size_t max_pending_requests = 0;
};

enum class pending_state {
   pending,
   approved,
   rejected,
   superseded,
};

struct pending_request {
   std::string identity;
   forge::auth::credential::scope_set requested_scopes;
   forge::auth::credential::scope_set scope_baseline;
   token_digest pre_session_digest;
   time_point created_at{};
   time_point expires_at{};
   pending_state state = pending_state::pending;
   std::optional<time_point> resolved_at;
   std::optional<forge::auth::credential::binding> approved_credential;
   bool pre_session_consumed = false;

   bool operator==(const pending_request&) const = default;
};

struct pending_issuance {
   pending_request record;
   forge::crypto::core::secret_string pre_session_token;
};

struct approval_options {
   forge::auth::credential::id id;
   time_point now{};
};

} // namespace forge::auth::pairing

export namespace forge::auth::pairing {
BOOST_DESCRIBE_ENUM(pending_state, pending, approved, rejected, superseded)
} // namespace forge::auth::pairing
