#include <concepts>

import forge.asio.compute;
import forge.auth.liboauth2.verifier;
import forge.auth.oauth2.token_verifier;
import forge.auth.oauth2.verification_policy;

int main() {
   static_assert(std::derived_from<forge::auth::liboauth2::verifier, forge::auth::oauth2::token_verifier>);
   auto blocking = forge::asio::compute::pool{{.worker_threads = 1, .max_pending_tasks = 1}};
   auto verifier = forge::auth::liboauth2::verifier::create(
       {.policy = {.issuer = "https://issuer.example", .audience = "package-test", .version = "1",
                   .algorithms = {forge::auth::oauth2::signature_algorithm::rs256}},
        .source = forge::auth::liboauth2::verifier::jwks{.endpoint = "https://issuer.example/keys"}},
       blocking.get_executor());
   verifier->request_stop();
   blocking.request_stop();
   return verifier ? 0 : 1;
}
