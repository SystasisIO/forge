module;

#include <forge/exceptions/macros.hpp>

#include <boost/url.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.auth.http.policy;

import forge.auth.http.exceptions;
import forge.auth.session.session;
import forge.crypto.core.constant_time;
import forge.crypto.core.secret_bytes;
import forge.exceptions;
import forge.net.http.cookie;
import forge.net.http.types;

namespace forge::auth::http {
namespace {

constexpr auto host_prefix = std::string_view{"__Host-"};
constexpr auto maximum_cookie_age = std::chrono::seconds{31 * 24 * 60 * 60};

enum class request_kind {
   safe,
   mutating,
};

template <typename Function> decltype(auto) translate_unexpected(Function&& function) {
   try {
      return std::forward<Function>(function)();
   } catch (const forge::exceptions::base&) {
      throw;
   } catch (const std::exception&) {
      FORGE_THROW_EXCEPTION(exceptions::internal_failure, "browser authorization operation failed");
   } catch (...) {
      FORGE_THROW_EXCEPTION(exceptions::internal_failure, "browser authorization operation failed");
   }
}

[[nodiscard]] std::span<const std::uint8_t> byte_view(std::string_view value) {
   return {reinterpret_cast<const std::uint8_t*>(value.data()), value.size()};
}

[[nodiscard]] bool has_control(std::string_view value) noexcept {
   return std::any_of(value.begin(), value.end(), [](char character) {
      const auto byte = static_cast<unsigned char>(character);
      return byte <= 0x1fU || byte == 0x7fU;
   });
}

void require_header_value(std::string_view value) {
   if (value.empty() || has_control(value)) {
      FORGE_THROW_EXCEPTION(exceptions::malformed_evidence, "browser security header is malformed");
   }
}

void require_canonical_origin(std::string_view origin, bool evidence) {
   const auto throw_malformed = [evidence] {
      if (evidence) {
         FORGE_THROW_EXCEPTION(exceptions::malformed_evidence, "Origin header is malformed");
      }
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "allowed origin is malformed");
   };
   if (origin.empty() || origin == "*" || has_control(origin)) {
      throw_malformed();
   }
   const auto parsed = boost::urls::parse_uri(origin);
   if (!parsed.has_value()) {
      throw_malformed();
   }

   auto canonical = boost::urls::url{parsed.value()};
   canonical.normalize();
   if ((canonical.scheme() != "http" && canonical.scheme() != "https") || !canonical.has_authority() ||
       canonical.has_userinfo() || canonical.host().empty() || !canonical.encoded_path().empty() ||
       canonical.has_query() || canonical.has_fragment()) {
      throw_malformed();
   }
   switch (canonical.host_type()) {
   case boost::urls::host_type::name:
   case boost::urls::host_type::ipv4:
   case boost::urls::host_type::ipv6:
      break;
   case boost::urls::host_type::none:
   case boost::urls::host_type::ipvfuture:
   default:
      throw_malformed();
   }
   if (canonical.has_port()) {
      const auto port = canonical.port_number();
      if (port == 0U) {
         throw_malformed();
      }
      const auto default_port =
          (canonical.scheme() == "http" && port == 80U) || (canonical.scheme() == "https" && port == 443U);
      if (default_port) {
         canonical.remove_port();
      } else {
         canonical.set_port_number(port);
      }
   }
   const auto serialized = canonical.buffer();
   if (origin != std::string_view{serialized.data(), serialized.size()}) {
      throw_malformed();
   }
}

void require_origin_policy(const origin_policy& policy) {
   if (policy.allowed_origins.empty() ||
       !std::is_sorted(policy.allowed_origins.begin(), policy.allowed_origins.end()) ||
       std::adjacent_find(policy.allowed_origins.begin(), policy.allowed_origins.end()) !=
           policy.allowed_origins.end()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "allowed origins must be canonical");
   }
   for (const auto& origin : policy.allowed_origins) {
      require_canonical_origin(origin, false);
   }
}

[[nodiscard]] request_kind classify_method(forge::net::http::method method) {
   switch (method) {
   case forge::net::http::method::get:
   case forge::net::http::method::head:
   case forge::net::http::method::options:
      return request_kind::safe;
   case forge::net::http::method::post:
   case forge::net::http::method::put:
   case forge::net::http::method::patch:
   case forge::net::http::method::delete_:
      return request_kind::mutating;
   case forge::net::http::method::unknown:
   default:
      FORGE_THROW_EXCEPTION(exceptions::method_not_allowed, "browser request method is not allowed");
   }
}

void require_cookie_policy(const cookie_policy& policy) {
   const auto names =
       std::array<std::string_view, 3>{policy.names.session, policy.names.csrf, policy.names.pre_session};
   for (const auto name : names) {
      if (!name.starts_with(host_prefix) || name.size() == host_prefix.size()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "authentication cookie name must use the __Host- prefix");
      }
      static_cast<void>(forge::net::http::format_set_cookie({
          .name = std::string{name},
          .value = "v",
          .path = "/",
          .same_site_value = forge::net::http::same_site::strict,
          .secure = true,
      }));
   }
   if (names[0] == names[1] || names[0] == names[2] || names[1] == names[2]) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "authentication cookie names must be distinct");
   }
   for (const auto age : {policy.pre_session_max_age, policy.session_max_age}) {
      if (age <= std::chrono::seconds::zero() || age > maximum_cookie_age) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "authentication cookie Max-Age is out of range");
      }
   }
}

[[nodiscard]] forge::net::http::set_cookie make_cookie(std::string name, std::string_view value,
                                                       std::chrono::seconds max_age, bool http_only) {
   auto result = forge::net::http::set_cookie{
       .name = std::move(name),
       .value = std::string{value},
       .path = "/",
       .max_age = max_age,
       .same_site_value = forge::net::http::same_site::strict,
       .secure = true,
       .http_only = http_only,
   };
   static_cast<void>(forge::net::http::format_set_cookie(result));
   return result;
}

[[nodiscard]] forge::net::http::set_cookie make_clearing_cookie(std::string name, bool http_only) {
   return make_cookie(std::move(name), "", std::chrono::seconds::zero(), http_only);
}

[[nodiscard]] std::vector<forge::net::http::cookie> parse_cookies(const std::vector<std::string>& headers) {
   auto result = std::vector<forge::net::http::cookie>{};
   for (const auto& header : headers) {
      auto parsed = forge::net::http::parse_cookie_header(header);
      for (auto& value : parsed) {
         if (std::any_of(result.begin(), result.end(),
                         [&](const auto& existing) { return existing.name == value.name; })) {
            FORGE_THROW_EXCEPTION(exceptions::duplicate_evidence, "browser Cookie header has duplicate evidence");
         }
         result.push_back(std::move(value));
      }
   }
   return result;
}

[[nodiscard]] std::optional<forge::crypto::core::secret_string>
take_cookie_value(std::vector<forge::net::http::cookie>& cookies, std::string_view name) {
   const auto found =
       std::find_if(cookies.begin(), cookies.end(), [name](const auto& value) { return value.name == name; });
   if (found == cookies.end()) {
      return std::nullopt;
   }
   auto result = forge::crypto::core::secret_string{std::move(found->value)};
   forge::crypto::core::secure_erase(found->value);
   return result;
}

[[nodiscard]] std::optional<std::string> single_header(const std::vector<std::string>& headers) {
   if (headers.empty()) {
      return std::nullopt;
   }
   if (headers.size() != 1U) {
      FORGE_THROW_EXCEPTION(exceptions::duplicate_evidence, "browser security header is duplicated");
   }
   require_header_value(headers.front());
   return headers.front();
}

void require_distinct_secrets(const forge::crypto::core::secret_string& first,
                              const forge::crypto::core::secret_string& second) {
   if (forge::crypto::core::constant_time_equal(byte_view(first.view()), byte_view(second.view()))) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "authentication cookie secrets must be distinct");
   }
}

template <typename... Cookies>
void append_cookies_atomically(forge::net::http::response& response, const Cookies&... cookies) {
   auto staged = response;
   (forge::net::http::append_set_cookie(staged, cookies), ...);
   response = std::move(staged);
}

void append_issued_session_cookies(forge::net::http::response& response,
                                   const forge::auth::session::session_issuance& issuance,
                                   const cookie_policy& policy) {
   forge::auth::session::validate_issuance(issuance);
   require_distinct_secrets(issuance.session_token, issuance.csrf_secret);
   const auto session_cookie = make_session_cookie(issuance.session_token, policy);
   const auto csrf_cookie = make_csrf_cookie(issuance.csrf_secret, policy);
   append_cookies_atomically(response, session_cookie, csrf_cookie);
}

} // namespace

origin_policy make_origin_policy(std::vector<std::string> allowed_origins) {
   return translate_unexpected([&] {
      for (const auto& origin : allowed_origins) {
         require_canonical_origin(origin, false);
      }
      std::sort(allowed_origins.begin(), allowed_origins.end());
      allowed_origins.erase(std::unique(allowed_origins.begin(), allowed_origins.end()), allowed_origins.end());
      auto result = origin_policy{.allowed_origins = std::move(allowed_origins)};
      require_origin_policy(result);
      return result;
   });
}

session_evidence extract_session_evidence(const browser_request_evidence& request, const cookie_policy& cookies) {
   return translate_unexpected([&] {
      const auto kind = classify_method(request.method);
      require_cookie_policy(cookies);
      auto parsed_cookies = parse_cookies(request.cookie_headers);
      auto session_token = take_cookie_value(parsed_cookies, cookies.names.session);
      if (!session_token.has_value()) {
         FORGE_THROW_EXCEPTION(exceptions::missing_evidence, "browser session cookie is missing");
      }
      const auto origin = single_header(request.origin_headers);
      if (origin.has_value()) {
         require_canonical_origin(*origin, true);
      }
      const auto csrf_header = single_header(request.csrf_headers);
      auto csrf_cookie = take_cookie_value(parsed_cookies, cookies.names.csrf);
      if (kind == request_kind::mutating && (!csrf_cookie.has_value() || !csrf_header.has_value())) {
         FORGE_THROW_EXCEPTION(exceptions::missing_evidence, "browser CSRF evidence is missing");
      }
      return session_evidence{
          .method = request.method,
          .origin = origin,
          .session_token = std::move(*session_token),
          .csrf_cookie = std::move(csrf_cookie),
          .csrf_header = csrf_header.has_value()
                             ? std::optional<forge::crypto::core::secret_string>{std::in_place, std::move(*csrf_header)}
                             : std::nullopt,
      };
   });
}

forge::auth::session::principal authorize(const session_evidence& evidence,
                                          const forge::auth::session::session_record& session,
                                          const forge::auth::credential::record& credential,
                                          const authorization_options& options) {
   return translate_unexpected([&] {
      const auto kind = classify_method(evidence.method);
      require_origin_policy(options.origins);
      if (evidence.origin.has_value()) {
         require_canonical_origin(*evidence.origin, true);
         if (!std::binary_search(options.origins.allowed_origins.begin(), options.origins.allowed_origins.end(),
                                 *evidence.origin)) {
            FORGE_THROW_EXCEPTION(exceptions::origin_mismatch, "browser Origin does not match the allowed policy");
         }
      } else if (kind == request_kind::mutating) {
         FORGE_THROW_EXCEPTION(exceptions::origin_mismatch, "browser Origin is required for a mutating request");
      }
      if (options.required_scope.empty()) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "browser authorization requires a scope");
      }

      const auto principal =
          forge::auth::session::validate_session(session, evidence.session_token, credential, options.now);
      if (!std::binary_search(principal.scopes.begin(), principal.scopes.end(), options.required_scope)) {
         FORGE_THROW_EXCEPTION(exceptions::scope_denied, "browser session lacks the required scope");
      }
      if (kind == request_kind::mutating) {
         if (!evidence.csrf_cookie.has_value() || !evidence.csrf_header.has_value()) {
            FORGE_THROW_EXCEPTION(exceptions::missing_evidence, "browser CSRF evidence is missing");
         }
         if (!forge::crypto::core::constant_time_equal(byte_view(evidence.csrf_cookie->view()),
                                                       byte_view(evidence.csrf_header->view()))) {
            FORGE_THROW_EXCEPTION(exceptions::csrf_mismatch, "browser CSRF evidence does not match");
         }
         forge::auth::session::verify_csrf_secret(session, *evidence.csrf_cookie, options.now);
      }
      return principal;
   });
}

forge::net::http::set_cookie make_pre_session_cookie(const forge::crypto::core::secret_string& pre_session_token,
                                                     const cookie_policy& policy) {
   return translate_unexpected([&] {
      require_cookie_policy(policy);
      return make_cookie(policy.names.pre_session, pre_session_token.view(), policy.pre_session_max_age, false);
   });
}

forge::net::http::set_cookie make_session_cookie(const forge::crypto::core::secret_string& session_token,
                                                 const cookie_policy& policy) {
   return translate_unexpected([&] {
      require_cookie_policy(policy);
      static_cast<void>(forge::auth::session::identify_session_token(session_token));
      return make_cookie(policy.names.session, session_token.view(), policy.session_max_age, true);
   });
}

forge::net::http::set_cookie make_csrf_cookie(const forge::crypto::core::secret_string& csrf_secret,
                                              const cookie_policy& policy) {
   return translate_unexpected([&] {
      require_cookie_policy(policy);
      return make_cookie(policy.names.csrf, csrf_secret.view(), policy.session_max_age, false);
   });
}

forge::net::http::set_cookie make_clear_pre_session_cookie(const cookie_policy& policy) {
   return translate_unexpected([&] {
      require_cookie_policy(policy);
      return make_clearing_cookie(policy.names.pre_session, false);
   });
}

forge::net::http::set_cookie make_clear_session_cookie(const cookie_policy& policy) {
   return translate_unexpected([&] {
      require_cookie_policy(policy);
      return make_clearing_cookie(policy.names.session, true);
   });
}

forge::net::http::set_cookie make_clear_csrf_cookie(const cookie_policy& policy) {
   return translate_unexpected([&] {
      require_cookie_policy(policy);
      return make_clearing_cookie(policy.names.csrf, false);
   });
}

void append_pre_session_cookie(forge::net::http::response& response,
                               const forge::crypto::core::secret_string& pre_session_token,
                               const cookie_policy& policy) {
   translate_unexpected(
       [&] { forge::net::http::append_set_cookie(response, make_pre_session_cookie(pre_session_token, policy)); });
}

void append_session_cookies(forge::net::http::response& response,
                            const forge::auth::session::session_issuance& issuance, const cookie_policy& policy) {
   translate_unexpected([&] { append_issued_session_cookies(response, issuance, policy); });
}

void append_approved_session_cookies(forge::net::http::response& response,
                                     const forge::crypto::core::secret_string& pre_session_token,
                                     const forge::auth::session::session_issuance& issuance,
                                     const cookie_policy& policy) {
   translate_unexpected([&] {
      forge::auth::session::validate_issuance(issuance);
      require_distinct_secrets(pre_session_token, issuance.session_token);
      require_distinct_secrets(pre_session_token, issuance.csrf_secret);
      const auto session_cookie = make_session_cookie(issuance.session_token, policy);
      const auto csrf_cookie = make_csrf_cookie(issuance.csrf_secret, policy);
      const auto clear_pre_session_cookie = make_clear_pre_session_cookie(policy);
      append_cookies_atomically(response, session_cookie, csrf_cookie, clear_pre_session_cookie);
   });
}

void append_rotated_session_cookies(forge::net::http::response& response,
                                    const forge::auth::session::session_issuance& issuance,
                                    const cookie_policy& policy) {
   append_session_cookies(response, issuance, policy);
}

void append_logout_cookies(forge::net::http::response& response, const cookie_policy& policy) {
   translate_unexpected([&] {
      const auto clear_session_cookie = make_clear_session_cookie(policy);
      const auto clear_csrf_cookie = make_clear_csrf_cookie(policy);
      append_cookies_atomically(response, clear_session_cookie, clear_csrf_cookie);
   });
}

void apply_security_headers(forge::net::http::response& response, security_header_options options) {
   translate_unexpected([&] {
      response.set(
          "Content-Security-Policy",
          "default-src 'self'; base-uri 'self'; object-src 'none'; frame-ancestors 'none'; form-action 'self'");
      response.set("X-Content-Type-Options", "nosniff");
      response.set("X-Frame-Options", "DENY");
      response.set("Referrer-Policy", "no-referrer");
      response.set("Permissions-Policy", "camera=(), geolocation=(), microphone=(), payment=(), usb=()");
      if (options.sensitive_response) {
         response.set("Cache-Control", "no-store");
      }
   });
}

} // namespace forge::auth::http
