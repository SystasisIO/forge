module;

#include <chrono>
#include <string>
#include <vector>

export module forge.auth.oauth2.verification_policy;

export import forge.auth.oauth2.types;

export namespace forge::auth::oauth2 {

enum class token_profile {
   jwt_access_token,
   opaque_introspection,
};

// Only asymmetric access-token algorithms are supported by this resource profile.
enum class signature_algorithm {
   rs256,
   rs384,
   rs512,
   ps256,
   ps384,
   ps512,
   es256,
   es384,
   es512,
};

struct verification_policy {
   std::string issuer;
   std::string audience;
   std::string version;
   token_profile profile = token_profile::jwt_access_token;
   std::vector<signature_algorithm> algorithms;
   scope_set required_scopes;
   std::chrono::seconds clock_skew{};
   std::chrono::seconds freshness{60};

   bool operator==(const verification_policy&) const = default;
};

// A policy is trusted route configuration, never values obtained from a token.
void validate_policy(const verification_policy& policy);

} // namespace forge::auth::oauth2
