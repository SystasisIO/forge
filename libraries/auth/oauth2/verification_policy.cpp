module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <string_view>
#include <vector>

module forge.auth.oauth2.verification_policy;

import forge.auth.oauth2.exceptions;

namespace forge::auth::oauth2 {
namespace {

bool valid_text(std::string_view text) {
   return !text.empty() &&
          std::ranges::none_of(text, [](unsigned char character) { return character < 0x20 || character == 0x7f; });
}

bool valid_algorithm(signature_algorithm algorithm) {
   switch (algorithm) {
   case signature_algorithm::rs256:
   case signature_algorithm::rs384:
   case signature_algorithm::rs512:
   case signature_algorithm::ps256:
   case signature_algorithm::ps384:
   case signature_algorithm::ps512:
   case signature_algorithm::es256:
   case signature_algorithm::es384:
   case signature_algorithm::es512:
      return true;
   }
   return false;
}

} // namespace

void validate_policy(const verification_policy& policy) {
   const auto max_duration = std::chrono::duration_cast<std::chrono::seconds>(time_point::duration::max());
   if (!valid_text(policy.issuer) || !valid_text(policy.audience) || !valid_text(policy.version) ||
       policy.clock_skew < std::chrono::seconds::zero() || policy.clock_skew > max_duration ||
       policy.freshness <= std::chrono::seconds::zero() || policy.freshness > max_duration) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "token verification policy is invalid");
   }
   switch (policy.profile) {
   case token_profile::jwt_access_token:
      if (policy.algorithms.empty() || !std::ranges::all_of(policy.algorithms, valid_algorithm)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_request, "JWT verification requires an algorithm allowlist");
      }
      break;
   case token_profile::opaque_introspection:
      if (!policy.algorithms.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_request, "introspection does not accept JWT algorithms");
      }
      break;
   default:
      FORGE_THROW_EXCEPTION(exceptions::invalid_request, "token verification profile is invalid");
   }
   for (const auto& scope : policy.required_scopes) {
      if (scope.empty() || !std::ranges::all_of(scope, [](unsigned char character) {
             return character == 0x21 || (character >= 0x23 && character <= 0x5b) ||
                    (character >= 0x5d && character <= 0x7e);
          })) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_request, "token verification scope is invalid");
      }
   }
}

} // namespace forge::auth::oauth2
