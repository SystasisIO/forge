#include <boost/test/unit_test.hpp>

#include <chrono>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

import forge.auth.http.exceptions;
import forge.auth.http.bearer;
import forge.auth.http.policy;
import forge.auth.session.exceptions;
import forge.auth.session.session;
import forge.auth.credential.record;
import forge.crypto.core.secret_string;
import forge.exceptions;
import forge.net.http.cookie;
import forge.net.http.exceptions;
import forge.net.http.types;

namespace auth_http = forge::auth::http;
namespace credentials = forge::auth::credential;
namespace session = forge::auth::session;
namespace net_http = forge::net::http;

namespace {

const auto test_now = session::time_point{std::chrono::seconds{1'700'000'000}};

[[nodiscard]] credentials::record credential(credentials::scope_set scopes = {"admin.read"}) {
   return {
       .id = {.value = "credential-owner"},
       .identity = "owner-device",
       .scopes = std::move(scopes),
       .generation = 1,
       .issued_at = test_now,
       .updated_at = test_now,
   };
}

[[nodiscard]] session::session_issuance issue_session(const credentials::record& owner = credential()) {
   return session::issue_session(owner, {
                                            .now = test_now,
                                            .absolute_expires_at = test_now + std::chrono::minutes{10},
                                            .idle_timeout = std::chrono::minutes{2},
                                        });
}

[[nodiscard]] auth_http::authorization_options
authorize_at(std::string scope = "admin.read", session::time_point now = test_now + std::chrono::seconds{1}) {
   return {
       .origins = auth_http::make_origin_policy({"https://admin.example"}),
       .required_scope = std::move(scope),
       .now = now,
   };
}

[[nodiscard]] auth_http::browser_request_evidence evidence(net_http::method method,
                                                           const session::session_issuance& issuance,
                                                           std::vector<std::string> origins = {},
                                                           bool include_csrf = false) {
   auto cookies = std::string{"__Host-forge-session="} + std::string{issuance.session_token.view()};
   auto csrf_headers = std::vector<std::string>{};
   if (include_csrf) {
      cookies += "; __Host-forge-csrf=" + std::string{issuance.csrf_secret.view()};
      csrf_headers.emplace_back(issuance.csrf_secret.view());
   }
   return {
       .method = method,
       .cookie_headers = {std::move(cookies)},
       .origin_headers = std::move(origins),
       .csrf_headers = std::move(csrf_headers),
   };
}

template <typename Exception, typename Function> void require_exception(Function&& function) {
   BOOST_CHECK_THROW(std::forward<Function>(function)(), Exception);
}

[[nodiscard]] std::size_t count_headers(const net_http::response& response, std::string_view name) {
   auto count = std::size_t{};
   for (const auto& header : response.headers()) {
      if (net_http::header_name_equal(header.name, name)) {
         ++count;
      }
   }
   return count;
}

void check_headers_unchanged(const net_http::response& response, const std::vector<net_http::header_entry>& before) {
   const auto after = response.headers();
   BOOST_REQUIRE_EQUAL(after.size(), before.size());
   for (auto index = std::size_t{}; index < before.size(); ++index) {
      BOOST_TEST(after[index].name == before[index].name);
      BOOST_TEST(after[index].text == before[index].text);
   }
}

} // namespace

BOOST_AUTO_TEST_SUITE(auth_http_policy)

BOOST_AUTO_TEST_CASE(authorization_enforces_safe_and_mutating_origin_matrix) {
   const auto owner = credential();
   const auto issuance = issue_session(owner);
   const auto cookies = auth_http::cookie_policy{};

   const auto safe = auth_http::extract_session_evidence(evidence(net_http::method::get, issuance), cookies);
   const auto principal = auth_http::authorize(safe, issuance.record, owner, authorize_at());
   BOOST_TEST(principal.identity == "owner-device");

   const auto safe_wrong_origin = auth_http::extract_session_evidence(
       evidence(net_http::method::head, issuance, {"https://other.example"}), cookies);
   require_exception<auth_http::exceptions::origin_mismatch>(
       [&] { return auth_http::authorize(safe_wrong_origin, issuance.record, owner, authorize_at()); });

   const auto options_request =
       auth_http::extract_session_evidence(evidence(net_http::method::options, issuance), cookies);
   static_cast<void>(auth_http::authorize(options_request, issuance.record, owner, authorize_at()));

   const auto mutating_without_origin =
       auth_http::extract_session_evidence(evidence(net_http::method::post, issuance, {}, true), cookies);
   require_exception<auth_http::exceptions::origin_mismatch>(
       [&] { return auth_http::authorize(mutating_without_origin, issuance.record, owner, authorize_at()); });

   const auto mutating_wrong_origin = auth_http::extract_session_evidence(
       evidence(net_http::method::put, issuance, {"https://other.example"}, true), cookies);
   require_exception<auth_http::exceptions::origin_mismatch>(
       [&] { return auth_http::authorize(mutating_wrong_origin, issuance.record, owner, authorize_at()); });

   const auto mutating = auth_http::extract_session_evidence(
       evidence(net_http::method::patch, issuance, {"https://admin.example"}, true), cookies);
   static_cast<void>(auth_http::authorize(mutating, issuance.record, owner, authorize_at()));

   require_exception<auth_http::exceptions::method_not_allowed>(
       [&] { return auth_http::extract_session_evidence(evidence(net_http::method::unknown, issuance), cookies); });
}

BOOST_AUTO_TEST_CASE(extraction_rejects_missing_duplicate_and_malformed_browser_evidence) {
   const auto issuance = issue_session();
   const auto cookies = auth_http::cookie_policy{};

   require_exception<auth_http::exceptions::missing_evidence>(
       [&] { return auth_http::extract_session_evidence({.method = net_http::method::get}, cookies); });
   require_exception<auth_http::exceptions::duplicate_evidence>([&] {
      return auth_http::extract_session_evidence(
          {
              .method = net_http::method::get,
              .cookie_headers = {"__Host-forge-session=" + std::string{issuance.session_token.view()},
                                 "__Host-forge-session=" + std::string{issuance.session_token.view()}},
          },
          cookies);
   });
   require_exception<auth_http::exceptions::duplicate_evidence>([&] {
      return auth_http::extract_session_evidence(
          evidence(net_http::method::get, issuance, {"https://admin.example", "https://admin.example"}), cookies);
   });
   require_exception<auth_http::exceptions::duplicate_evidence>([&] {
      auto duplicate = evidence(net_http::method::post, issuance, {"https://admin.example"}, true);
      duplicate.csrf_headers.push_back(std::string{issuance.csrf_secret.view()});
      return auth_http::extract_session_evidence(duplicate, cookies);
   });
   require_exception<auth_http::exceptions::missing_evidence>([&] {
      return auth_http::extract_session_evidence(
          evidence(net_http::method::delete_, issuance, {"https://admin.example"}), cookies);
   });
   require_exception<auth_http::exceptions::malformed_evidence>([&] {
      return auth_http::extract_session_evidence(
          evidence(net_http::method::get, issuance, {"https://admin.example/path"}), cookies);
   });
   require_exception<auth_http::exceptions::invalid_options>([] { return auth_http::make_origin_policy({"*"}); });
}

BOOST_AUTO_TEST_CASE(origin_policy_requires_canonical_browser_origins) {
   static_cast<void>(auth_http::make_origin_policy({"http://127.0.0.1:8080", "https://[2001:db8::1]:8443"}));

   for (const auto origin : {"https://[2001:db8::zz]", "https://user@admin.example", "https://admin.example/path",
                             "https://admin.example?query=value", "https://admin.example#fragment",
                             "HTTPS://admin.example", "https://ADMIN.example", "https://admin.example:443",
                             "http://admin.example:80", "https://admin.example:08443"}) {
      require_exception<auth_http::exceptions::invalid_options>(
          [origin] { return auth_http::make_origin_policy({origin}); });
   }

   const auto issuance = issue_session();
   require_exception<auth_http::exceptions::malformed_evidence>([&] {
      return auth_http::extract_session_evidence(
          evidence(net_http::method::get, issuance, {"https://admin.example:443"}), auth_http::cookie_policy{});
   });
}

BOOST_AUTO_TEST_CASE(authorization_propagates_session_binding_and_rejects_csrf_or_scope_escalation) {
   const auto owner = credential();
   auto issuance = issue_session(owner);
   const auto cookies = auth_http::cookie_policy{};
   auto mutating = auth_http::extract_session_evidence(
       evidence(net_http::method::post, issuance, {"https://admin.example"}, true), cookies);
   *mutating.csrf_header = forge::crypto::core::secret_string{"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"};
   require_exception<auth_http::exceptions::csrf_mismatch>(
       [&] { return auth_http::authorize(mutating, issuance.record, owner, authorize_at()); });

   const auto safe = auth_http::extract_session_evidence(evidence(net_http::method::get, issuance), cookies);
   require_exception<auth_http::exceptions::scope_denied>(
       [&] { return auth_http::authorize(safe, issuance.record, owner, authorize_at("admin.write")); });

   auto escalated_credential = owner;
   escalated_credential.scopes = {"admin.read", "admin.write"};
   require_exception<session::exceptions::scope_mismatch>(
       [&] { return auth_http::authorize(safe, issuance.record, escalated_credential, authorize_at()); });

   auto rotated = session::rotate_session(issuance.record, owner,
                                          {.now = test_now + std::chrono::seconds{2},
                                           .absolute_expires_at = test_now + std::chrono::minutes{10},
                                           .idle_timeout = std::chrono::minutes{2}});
   const auto rotated_now = test_now + std::chrono::seconds{3};
   require_exception<session::exceptions::replayed>(
       [&] { return auth_http::authorize(safe, issuance.record, owner, authorize_at("admin.read", rotated_now)); });
   const auto rotated_safe = auth_http::extract_session_evidence(evidence(net_http::method::get, rotated), cookies);
   static_cast<void>(
       auth_http::authorize(rotated_safe, rotated.record, owner, authorize_at("admin.read", rotated_now)));

   auto revoked = owner;
   revoked.state = credentials::state::revoked;
   revoked.updated_at = test_now + std::chrono::seconds{2};
   revoked.revoked_at = revoked.updated_at;
   require_exception<session::exceptions::credential_revoked>([&] {
      return auth_http::authorize(rotated_safe, rotated.record, revoked, authorize_at("admin.read", rotated_now));
   });

   BOOST_CHECK(session::identify_session_token(rotated.session_token) == rotated.record.session_digest);
   require_exception<session::exceptions::token_invalid>([&] {
      return auth_http::authorize(
          auth_http::session_evidence{.method = net_http::method::get,
                                      .session_token = forge::crypto::core::secret_string{"not-a-session-token"}},
          rotated.record, owner, authorize_at("admin.read", rotated_now));
   });
}

BOOST_AUTO_TEST_CASE(cookie_policy_preserves_repeated_set_cookie_and_clears_browser_state) {
   const auto owner = credential();
   const auto issuance = issue_session(owner);
   const auto pre_session_token = issue_session(owner).session_token;
   const auto policy = auth_http::cookie_policy{};
   auto response = net_http::response{net_http::status::ok, 11};
   auth_http::append_pre_session_cookie(response, pre_session_token, policy);
   auth_http::append_approved_session_cookies(response, pre_session_token, issuance, policy);

   BOOST_TEST(count_headers(response, "Set-Cookie") == 4U);
   BOOST_CHECK(pre_session_token.view() != issuance.session_token.view());
   BOOST_CHECK(pre_session_token.view() != issuance.csrf_secret.view());
   BOOST_CHECK(issuance.session_token.view() != issuance.csrf_secret.view());
   const auto headers = response.headers();
   const auto session_cookie = net_http::parse_set_cookie_header(headers[1].text);
   const auto csrf_cookie = net_http::parse_set_cookie_header(headers[2].text);
   const auto cleared_pre_session = net_http::parse_set_cookie_header(headers[3].text);
   BOOST_TEST(session_cookie.name == "__Host-forge-session");
   BOOST_TEST(session_cookie.secure);
   BOOST_TEST(session_cookie.http_only);
   BOOST_CHECK(session_cookie.path == std::optional<std::string>{"/"});
   BOOST_CHECK(session_cookie.same_site_value == std::optional<net_http::same_site>{net_http::same_site::strict});
   BOOST_CHECK(!session_cookie.domain.has_value());
   BOOST_TEST(csrf_cookie.name == "__Host-forge-csrf");
   BOOST_TEST(!csrf_cookie.http_only);
   BOOST_TEST(cleared_pre_session.name == "__Host-forge-pre-session");
   BOOST_CHECK(cleared_pre_session.max_age == std::optional<std::chrono::seconds>{std::chrono::seconds::zero()});

   auto logout = net_http::response{net_http::status::no_content, 11};
   auth_http::append_logout_cookies(logout, policy);
   BOOST_TEST(count_headers(logout, "Set-Cookie") == 2U);

   auto invalid_name = policy;
   invalid_name.names.session = "session";
   require_exception<auth_http::exceptions::invalid_options>(
       [&] { return auth_http::make_session_cookie(issuance.session_token, invalid_name); });
   auto invalid_age = policy;
   invalid_age.session_max_age = std::chrono::seconds::zero();
   require_exception<auth_http::exceptions::invalid_options>(
       [&] { return auth_http::make_session_cookie(issuance.session_token, invalid_age); });

   const auto injected = forge::crypto::core::secret_string{"session\r\ninjection"};
   try {
      static_cast<void>(auth_http::make_pre_session_cookie(injected, policy));
      BOOST_FAIL("injected cookie value must fail");
   } catch (const forge::exceptions::base& error) {
      BOOST_TEST(error.message().find(injected.view()) == std::string::npos);
      for (const auto& field : error.context()) {
         BOOST_TEST(field.value.find(injected.view()) == std::string::npos);
      }
   }
}

BOOST_AUTO_TEST_CASE(cookie_issuance_validates_session_integrity_before_atomic_response_update) {
   const auto owner = credential();
   const auto valid = issue_session(owner);
   auto tampered = valid;
   tampered.csrf_secret = issue_session(owner).csrf_secret;
   auto response = net_http::response{net_http::status::ok, 11};
   response.set("X-Existing", "unchanged");
   const auto before = response.headers();

   require_exception<session::exceptions::csrf_invalid>(
       [&] { auth_http::append_session_cookies(response, tampered, auth_http::cookie_policy{}); });
   check_headers_unchanged(response, before);

   require_exception<session::exceptions::csrf_invalid>([&] {
      auth_http::append_approved_session_cookies(response, forge::crypto::core::secret_string{"pre-session"}, tampered,
                                                 auth_http::cookie_policy{});
   });
   check_headers_unchanged(response, before);

   auto revoked = valid;
   session::logout_session(revoked.record, test_now + std::chrono::seconds{1});
   require_exception<session::exceptions::invalid_state>(
       [&] { auth_http::append_session_cookies(response, revoked, auth_http::cookie_policy{}); });
   check_headers_unchanged(response, before);

   auto invalid_logout_policy = auth_http::cookie_policy{};
   invalid_logout_policy.names.csrf = "csrf";
   require_exception<auth_http::exceptions::invalid_options>(
       [&] { auth_http::append_logout_cookies(response, invalid_logout_policy); });
   check_headers_unchanged(response, before);

   auth_http::append_session_cookies(response, valid, auth_http::cookie_policy{});
   BOOST_TEST(count_headers(response, "Set-Cookie") == 2U);
}

BOOST_AUTO_TEST_CASE(security_headers_are_strict_without_global_asset_no_store) {
   auto asset = net_http::response{net_http::status::ok, 11};
   auth_http::apply_security_headers(asset);
   BOOST_TEST(asset["Content-Security-Policy"].find("default-src 'self'") != std::string::npos);
   BOOST_TEST(asset["X-Content-Type-Options"] == "nosniff");
   BOOST_TEST(asset["X-Frame-Options"] == "DENY");
   BOOST_TEST(asset["Referrer-Policy"] == "no-referrer");
   BOOST_TEST(asset["Permissions-Policy"].find("camera=()") != std::string::npos);
   BOOST_CHECK(!asset.header("Cache-Control").has_value());

   auto sensitive = net_http::response{net_http::status::ok, 11};
   auth_http::apply_security_headers(sensitive, {.sensitive_response = true});
   BOOST_TEST(sensitive["Cache-Control"] == "no-store");
}

BOOST_AUTO_TEST_SUITE_END()
BOOST_AUTO_TEST_CASE(bearer_extraction_is_bounded_borrowed_and_rejects_ambiguous_evidence) {
   auto request = net_http::request{net_http::method::get, "/", 11};
   BOOST_TEST(!auth_http::extract_bearer_token(request));
   request.insert("authorization", "bEaReR  opaque-token");
   const auto token = auth_http::extract_bearer_token(request, 12);
   BOOST_REQUIRE(token.has_value());
   BOOST_TEST(*token == "opaque-token");
   BOOST_TEST(token->data() == request.header("Authorization")->data() + 8);
   const auto churn = std::vector<std::string>(128, std::string(1024, 'x'));
   BOOST_TEST(churn.size() == 128U);
   BOOST_TEST(*token == "opaque-token");
   BOOST_CHECK_THROW(auth_http::extract_bearer_token(request, 11), auth_http::exceptions::malformed_evidence);
   BOOST_CHECK_THROW(auth_http::extract_bearer_token(request, 0), auth_http::exceptions::invalid_options);
   request.insert("Authorization", "Bearer another");
   BOOST_CHECK_THROW(auth_http::extract_bearer_token(request), auth_http::exceptions::duplicate_evidence);

   for (const auto value :
        {std::string_view{""}, std::string_view{"Basic opaque"}, std::string_view{"Bearer"},
         std::string_view{"Bearer "}, std::string_view{"Bearer token value"}, std::string_view{"Bearer\ttoken"},
         std::string_view{"Bearer token\t"}, std::string_view{"Bearer token\r"}, std::string_view{"Bearer token\n"},
         std::string_view{"Bearer token\x7f"}, std::string_view{"Bearer token\0tail", 17}}) {
      const auto values = std::array{value};
      BOOST_CHECK_THROW(auth_http::extract_bearer_token(values), auth_http::exceptions::malformed_evidence);
   }
}
