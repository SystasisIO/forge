module;

#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.auth.oauth2.token_verifier;

namespace forge::auth::oauth2 {

verified_token::verified_token(claims value) : _claims{std::move(value)} {}

verified_token::verified_token(verified_token&& other) noexcept : _claims{std::move(other._claims)} {
   other._claims.valid_until = other._claims.verified_at;
}

const std::string& verified_token::issuer() const noexcept {
   return _claims.issuer;
}

const std::string& verified_token::subject() const noexcept {
   return _claims.subject;
}

const std::vector<std::string>& verified_token::audiences() const noexcept {
   return _claims.audiences;
}

const scope_set& verified_token::scopes() const noexcept {
   return _claims.scopes;
}

time_point verified_token::expires_at() const noexcept {
   return _claims.expires_at;
}

time_point verified_token::verified_at() const noexcept {
   return _claims.verified_at;
}

time_point verified_token::valid_until() const noexcept {
   return _claims.valid_until;
}

const std::string& verified_token::policy_version() const noexcept {
   return _claims.policy_version;
}

const std::optional<std::string>& verified_token::client_id() const noexcept {
   return _claims.client_id;
}

token_profile verified_token::profile() const noexcept {
   return _claims.profile;
}

bool verified_token::valid_at(time_point now) const noexcept {
   return now >= _claims.verified_at && now < _claims.valid_until;
}

} // namespace forge::auth::oauth2
