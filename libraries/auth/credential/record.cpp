module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.auth.credential.record;

namespace forge::auth::credential {

void validate_id(const id& value) {
   if (value.value.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::credential_id_invalid, "credential ID must not be empty");
   }
}

void validate_identity(std::string_view identity) {
   if (identity.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::identity_invalid, "credential identity must not be empty");
   }
}

void validate_scopes(const scope_set& scopes) {
   if (scopes.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::scope_invalid, "credential scopes must not be empty");
   }
   for (const auto& scope : scopes) {
      if (scope.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::scope_invalid, "credential scope must not be empty");
      }
   }
   if (!std::is_sorted(scopes.begin(), scopes.end()) ||
       std::adjacent_find(scopes.begin(), scopes.end()) != scopes.end()) {
      FORGE_THROW_EXCEPTION(exceptions::scope_invalid, "credential scopes must be canonical");
   }
}

scope_set canonicalize_scopes(scope_set scopes) {
   for (const auto& scope : scopes) {
      if (scope.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::scope_invalid, "credential scope must not be empty");
      }
   }
   std::ranges::sort(scopes);
   scopes.erase(std::unique(scopes.begin(), scopes.end()), scopes.end());
   validate_scopes(scopes);
   return scopes;
}

bool scopes_subset_of(const scope_set& requested, const scope_set& baseline) {
   validate_scopes(requested);
   validate_scopes(baseline);
   return std::includes(baseline.begin(), baseline.end(), requested.begin(), requested.end());
}

void validate_record(const record& value) {
   validate_id(value.id);
   validate_identity(value.identity);
   validate_scopes(value.scopes);
   if (value.generation == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "credential record is invalid");
   }
   switch (value.state) {
   case state::active:
      if (value.revoked_at.has_value()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "active credential has a revocation time");
      }
      break;
   case state::revoked:
      if (!value.revoked_at.has_value()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "revoked credential has no revocation time");
      }
      if (*value.revoked_at != value.updated_at || *value.revoked_at < value.issued_at) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_state, "revoked credential has an invalid revocation time");
      }
      break;
   default:
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "credential record has an unknown state");
   }
   if (value.updated_at < value.issued_at) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "credential record update precedes issuance");
   }
}

void validate_active(const record& value, time_point now) {
   validate_record(value);
   if (now < value.updated_at || value.state != state::active) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "credential is not active");
   }
}

void rotate_downscope(record& value, scope_set scopes, time_point now) {
   validate_active(value, now);
   scopes = canonicalize_scopes(std::move(scopes));
   if (!scopes_subset_of(scopes, value.scopes)) {
      FORGE_THROW_EXCEPTION(exceptions::scope_invalid, "credential rotation cannot escalate scopes");
   }
   if (scopes == value.scopes) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state, "credential rotation must reduce scopes");
   }
   if (value.generation == std::numeric_limits<std::uint64_t>::max()) {
      FORGE_THROW_EXCEPTION(exceptions::generation_exhausted, "credential generation is exhausted");
   }
   value.scopes = std::move(scopes);
   ++value.generation;
   value.updated_at = now;
}

void revoke(record& value, time_point now) {
   validate_active(value, now);
   value.state = state::revoked;
   value.updated_at = now;
   value.revoked_at = now;
}

} // namespace forge::auth::credential
