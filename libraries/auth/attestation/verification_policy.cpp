module;

#include <forge/exceptions/policy.hpp>

#include <cstddef>
#include <string_view>

module forge.auth.attestation.verification_policy;

namespace forge::auth::attestation {
namespace detail {

bool valid_uri_identifier(std::string_view value) noexcept {
   if (value.empty() || value.size() > max_identifier_bytes) {
      return false;
   }
   const auto alpha = [](char ch) { return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'); };
   const auto hex = [](char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F');
   };
   const auto separator = value.find(':');
   if (separator == std::string_view::npos || separator == 0 || !alpha(value.front())) {
      return false;
   }
   for (std::size_t index = 1; index < separator; ++index) {
      const auto ch = value[index];
      if (!alpha(ch) && !(ch >= '0' && ch <= '9') && ch != '+' && ch != '-' && ch != '.') {
         return false;
      }
   }
   constexpr auto punctuation = std::string_view{"-._~:/?#[]@!$&'()*+,;="};
   for (std::size_t index = separator + 1; index < value.size(); ++index) {
      const auto ch = value[index];
      if (ch == '%') {
         if (value.size() - index < 3 || !hex(value[index + 1]) || !hex(value[index + 2])) {
            return false;
         }
         index += 2;
      } else if (!alpha(ch) && !(ch >= '0' && ch <= '9') && punctuation.find(ch) == std::string_view::npos) {
         return false;
      }
   }
   return true;
}

bool valid_purpose(std::string_view value) noexcept {
   if (value.empty() || value.size() > max_purpose_bytes || value.front() < 'a' || value.front() > 'z') {
      return false;
   }
   for (const auto ch : value) {
      if (!(ch >= 'a' && ch <= 'z') && !(ch >= '0' && ch <= '9') && ch != '.' && ch != '_' && ch != '-') {
         return false;
      }
   }
   return true;
}

bool valid_document_id(std::string_view value) noexcept {
   if (value.size() != document_id_bytes || !value.starts_with("g_")) {
      return false;
   }
   for (const auto ch : value.substr(2)) {
      if (!(ch >= '0' && ch <= '9') && !(ch >= 'a' && ch <= 'f')) {
         return false;
      }
   }
   return true;
}

} // namespace detail

void verification_policy::validate() const {
   if (!detail::valid_uri_identifier(issuer) || !detail::valid_uri_identifier(audience) ||
       !detail::valid_uri_identifier(schema) || !detail::valid_purpose(purpose) || version != profile_version ||
       max_lifetime.count() <= 0 || max_lifetime.count() > max_numeric_date || clock_skew.count() < 0 ||
       clock_skew.count() > max_numeric_date) {
      FORGE_POLICY_THROW_EXCEPTION(exceptions::invalid_profile, "attestation verification profile is invalid");
   }
}

} // namespace forge::auth::attestation
