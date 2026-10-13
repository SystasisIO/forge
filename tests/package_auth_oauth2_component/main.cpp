#include <concepts>

import forge.auth.oauth2.access_token;
import forge.auth.oauth2.token_verifier;
import forge.auth.oauth2.verification_policy;

int main() {
   static_assert(!std::copy_constructible<forge::auth::oauth2::access_token>);
   static_assert(!std::default_initializable<forge::auth::oauth2::verified_token>);
   forge::auth::oauth2::validate_policy(
       {.issuer = "https://issuer.example", .audience = "package-test", .version = "1",
        .algorithms = {forge::auth::oauth2::signature_algorithm::rs256}});
}
