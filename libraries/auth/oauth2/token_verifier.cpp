module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.auth.oauth2.token_verifier;

namespace forge::auth::oauth2 {
namespace {

bool valid_text(std::string_view text) {
   return !text.empty() &&
          std::ranges::none_of(text, [](unsigned char character) { return character < 0x20 || character == 0x7f; });
}

bool valid_scope(std::string_view scope) {
   return !scope.empty() && std::ranges::all_of(scope, [](unsigned char character) {
      return character == 0x21 || (character >= 0x23 && character <= 0x5b) || (character >= 0x5d && character <= 0x7e);
   });
}

void canonicalize(std::vector<std::string>& values) {
   std::ranges::sort(values);
   values.erase(std::unique(values.begin(), values.end()), values.end());
}

} // namespace

token_verifier::~token_verifier() = default;

verified_token token_verifier::make_verified_token(verified_token::claims claims) {
   if (!valid_text(claims.issuer) || !valid_text(claims.subject) || !valid_text(claims.policy_version) ||
       claims.audiences.empty() || !std::ranges::all_of(claims.audiences, valid_text) ||
       !std::ranges::all_of(claims.scopes, valid_scope) || (claims.client_id && !valid_text(*claims.client_id)) ||
       claims.valid_until <= claims.verified_at || claims.valid_until > claims.expires_at ||
       (claims.profile != token_profile::jwt_access_token && claims.profile != token_profile::opaque_introspection)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_token, "verifier evidence is invalid");
   }
   canonicalize(claims.audiences);
   canonicalize(claims.scopes);
   return verified_token{std::move(claims)};
}

} // namespace forge::auth::oauth2
