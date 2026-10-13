module;

#include <boost/asio/awaitable.hpp>

#include <forge/exceptions/macros.hpp>
extern "C" {
#include <oauth2/cfg.h>
}
#include <jansson.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.auth.liboauth2.verifier;

import forge.asio.notification;

#include "details/verifier_impl.hxx"

namespace forge::auth::liboauth2 {

oauth2::verified_token::claims verifier::impl::normalize(json_t* payload, oauth2::time_point now) const {
   if (!json_is_object(payload)) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential claims rejected");
   }
   auto bytes = std::size_t{};
   const auto text = [&](json_t* value) -> std::string {
      if (!json_is_string(value) || json_string_length(value) == 0) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential claim type rejected");
      }
      const auto length = json_string_length(value);
      if (length > configuration.max_claim_bytes - bytes) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::response_too_large, "credential claims exceed limit");
      }
      const auto content = std::string_view{json_string_value(value), length};
      if (std::ranges::any_of(content, [](unsigned char c) { return c < 0x20 || c == 0x7f; })) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential claim text rejected");
      }
      bytes += length;
      return std::string{content};
   };
   const auto date = [](json_t* value) -> oauth2::time_point {
      if (!json_is_integer(value)) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential NumericDate type rejected");
      }
      const auto seconds = json_integer_value(value);
      const auto maximum =
          std::chrono::duration_cast<std::chrono::seconds>(oauth2::time_point::duration::max()).count();
      const auto minimum =
          std::chrono::duration_cast<std::chrono::seconds>(oauth2::time_point::duration::min()).count();
      if (seconds < minimum || seconds > maximum) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential NumericDate range rejected");
      }
      return oauth2::time_point{
          std::chrono::duration_cast<oauth2::time_point::duration>(std::chrono::seconds{seconds})};
   };
   const auto& policy = configuration.policy;
   auto result = oauth2::verified_token::claims{
       .issuer = text(json_object_get(payload, "iss")),
       .subject = text(json_object_get(payload, "sub")),
       .expires_at = date(json_object_get(payload, "exp")),
       .verified_at = now,
       .policy_version = policy.version,
       .profile = policy.profile,
   };
   if (result.issuer != policy.issuer || result.expires_at <= now) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential issuer or expiry rejected");
   }
   const auto skew = std::chrono::duration_cast<oauth2::time_point::duration>(policy.clock_skew);
   const auto latest = now > oauth2::time_point::max() - skew ? oauth2::time_point::max() : now + skew;
   if (auto* nbf = json_object_get(payload, "nbf"); nbf && date(nbf) > latest) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential not-before rejected");
   }
   auto* audience = json_object_get(payload, "aud");
   if (json_is_string(audience)) {
      result.audiences.push_back(text(audience));
   } else if (json_is_array(audience) && json_array_size(audience) > 0 &&
              json_array_size(audience) <= configuration.max_audiences) {
      for (std::size_t index = 0; index < json_array_size(audience); ++index) {
         result.audiences.push_back(text(json_array_get(audience, index)));
      }
   } else {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential audience rejected");
   }
   if (std::ranges::find(result.audiences, policy.audience) == result.audiences.end()) {
      FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential resource rejected");
   }
   if (auto* client = json_object_get(payload, "client_id")) {
      result.client_id = text(client);
   }
   if (policy.profile == oauth2::token_profile::jwt_access_token) {
      if (!result.client_id || date(json_object_get(payload, "iat")) > latest ||
          date(json_object_get(payload, "iat")) >= result.expires_at) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential access-token profile rejected");
      }
      (void)text(json_object_get(payload, "jti"));
   } else {
      const auto token_type = text(json_object_get(payload, "token_type"));
      const auto bearer =
          std::ranges::equal(token_type, std::string_view{"Bearer"}, [](unsigned char left, unsigned char right) {
             const auto lower = [](unsigned char value) {
                return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
             };
             return lower(left) == lower(right);
          });
      if (!json_is_true(json_object_get(payload, "active")) || !bearer) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "introspection access-token profile rejected");
      }
   }
   if (auto* scope = json_object_get(payload, "scope")) {
      const auto grant = text(scope);
      auto remaining = std::string_view{grant};
      while (!remaining.empty()) {
         const auto end = remaining.find(' ');
         const auto entry = remaining.substr(0, end);
         if (!entry.empty()) {
            if (result.scopes.size() >= configuration.max_scopes) {
               FORGE_THROW_EXCEPTION(oauth2::exceptions::response_too_large, "credential scope count exceeds limit");
            }
            if (!std::ranges::all_of(entry, [](unsigned char c) {
                   return c == 0x21 || (c >= 0x23 && c <= 0x5b) || (c >= 0x5d && c <= 0x7e);
                })) {
               FORGE_THROW_EXCEPTION(oauth2::exceptions::invalid_token, "credential scope rejected");
            }
            result.scopes.emplace_back(entry);
         }
         if (end == std::string_view::npos) {
            break;
         }
         remaining.remove_prefix(end + 1);
      }
   }
   for (const auto& required : policy.required_scopes) {
      if (std::ranges::find(result.scopes, required) == result.scopes.end()) {
         FORGE_THROW_EXCEPTION(oauth2::exceptions::policy_denied, "credential scope is insufficient");
      }
   }
   const auto freshness = std::chrono::duration_cast<oauth2::time_point::duration>(policy.freshness);
   const auto until = now > oauth2::time_point::max() - freshness ? oauth2::time_point::max() : now + freshness;
   result.valid_until = std::min(result.expires_at, until);
   return result;
}

} // namespace forge::auth::liboauth2
