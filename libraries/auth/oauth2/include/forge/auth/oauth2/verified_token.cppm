module;

#include <chrono>
#include <optional>
#include <string>
#include <vector>

export module forge.auth.oauth2.token_verifier:verified_token;

export import forge.auth.oauth2.verification_policy;

export namespace forge::auth::oauth2 {

class token_verifier;

class verified_token {
 public:
   // Normalized backend evidence, not a public construction or serialization API.
   struct claims {
      std::string issuer;
      std::string subject;
      std::vector<std::string> audiences;
      scope_set scopes;
      time_point expires_at;
      time_point verified_at;
      time_point valid_until;
      std::string policy_version;
      std::optional<std::string> client_id;
      token_profile profile = token_profile::jwt_access_token;
   };

   verified_token(const verified_token&) = default;
   verified_token(verified_token&& other) noexcept;
   verified_token& operator=(const verified_token&) = delete;
   verified_token& operator=(verified_token&&) = delete;
   ~verified_token() = default;

   [[nodiscard]] const std::string& issuer() const noexcept;
   [[nodiscard]] const std::string& subject() const noexcept;
   [[nodiscard]] const std::vector<std::string>& audiences() const noexcept;
   [[nodiscard]] const scope_set& scopes() const noexcept;
   [[nodiscard]] time_point expires_at() const noexcept;
   [[nodiscard]] time_point verified_at() const noexcept;
   [[nodiscard]] time_point valid_until() const noexcept;
   [[nodiscard]] const std::string& policy_version() const noexcept;
   [[nodiscard]] const std::optional<std::string>& client_id() const noexcept;
   [[nodiscard]] token_profile profile() const noexcept;
   [[nodiscard]] bool valid_at(time_point now) const noexcept;

 private:
   explicit verified_token(claims value);

   claims _claims;

   friend class token_verifier;
};

} // namespace forge::auth::oauth2
