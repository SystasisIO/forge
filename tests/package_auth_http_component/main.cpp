#include <chrono>

import forge.auth.http.exceptions;
import forge.auth.http.bearer;
import forge.auth.http.policy;
import forge.net.http.types;

int main() {
   namespace auth_http = forge::auth::http;
   namespace credentials = forge::auth::credential;
   namespace session = forge::auth::session;

   const auto now = session::time_point{std::chrono::seconds{1}};
   const auto credential = credentials::record{
       .id = {.value = "credential-consumer"},
       .identity = "consumer",
       .scopes = {"admin.read"},
       .generation = 1,
       .issued_at = now,
       .updated_at = now,
   };
   const auto issuance = session::issue_session(
       credential,
       {.now = now, .absolute_expires_at = now + std::chrono::minutes{1}, .idle_timeout = std::chrono::seconds{30}});
   session::validate_issuance(issuance);
   const auto origin = auth_http::make_origin_policy({"https://admin.example"});
   const auto cookie = auth_http::make_session_cookie(issuance.session_token, {});
   const auto digest = session::identify_session_token(issuance.session_token);
   const auto exception_import = auth_http::exceptions::code::csrf_mismatch;
   auto request = forge::net::http::request{forge::net::http::method::get, "/", 11};
   request.set("Authorization", "Bearer package-test");
   const auto token = auth_http::extract_bearer_token(request);

   return origin.allowed_origins.size() == 1U && cookie.name == "__Host-forge-session" &&
                  digest == issuance.record.session_digest &&
                  exception_import == auth_http::exceptions::code::csrf_mismatch && token && *token == "package-test"
              ? 0
              : 1;
}
