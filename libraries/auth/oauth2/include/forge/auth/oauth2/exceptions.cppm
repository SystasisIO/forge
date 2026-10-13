module;

#include <cstdint>
#include <optional>
#include <string_view>
#include <system_error>

#include <forge/exceptions/macros.hpp>

export module forge.auth.oauth2.exceptions;

export import forge.exceptions;

export namespace forge::auth::oauth2::exceptions {

enum class code : std::uint16_t {
   invalid_request = 1,
   setup_required = 2,
   token_unavailable = 3,
   unsupported_request = 4,
   canceled = 5,
   invalid_token = 6,
   policy_denied = 7,
   issuer_unavailable = 8,
   input_too_large = 9,
   response_too_large = 10,
   busy = 11,
   stopped = 12,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.auth.oauth2")

using invalid_request = forge::exceptions::coded_exception<code, code::invalid_request>;
using setup_required = forge::exceptions::coded_exception<code, code::setup_required>;
using token_unavailable = forge::exceptions::coded_exception<code, code::token_unavailable>;
using unsupported_request = forge::exceptions::coded_exception<code, code::unsupported_request>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;
using invalid_token = forge::exceptions::coded_exception<code, code::invalid_token>;
using policy_denied = forge::exceptions::coded_exception<code, code::policy_denied>;
using issuer_unavailable = forge::exceptions::coded_exception<code, code::issuer_unavailable>;
using input_too_large = forge::exceptions::coded_exception<code, code::input_too_large>;
using response_too_large = forge::exceptions::coded_exception<code, code::response_too_large>;
using busy = forge::exceptions::coded_exception<code, code::busy>;
using stopped = forge::exceptions::coded_exception<code, code::stopped>;

[[nodiscard]] inline std::optional<code> code_of(const forge::exceptions::base& value) noexcept {
   const auto& actual = value.code();
   if (!actual || std::string_view{actual.category().name()} != "forge.auth.oauth2") {
      return std::nullopt;
   }
   switch (actual.value()) {
   case static_cast<int>(code::invalid_request):
      return code::invalid_request;
   case static_cast<int>(code::setup_required):
      return code::setup_required;
   case static_cast<int>(code::token_unavailable):
      return code::token_unavailable;
   case static_cast<int>(code::unsupported_request):
      return code::unsupported_request;
   case static_cast<int>(code::canceled):
      return code::canceled;
   case static_cast<int>(code::invalid_token):
      return code::invalid_token;
   case static_cast<int>(code::policy_denied):
      return code::policy_denied;
   case static_cast<int>(code::issuer_unavailable):
      return code::issuer_unavailable;
   case static_cast<int>(code::input_too_large):
      return code::input_too_large;
   case static_cast<int>(code::response_too_large):
      return code::response_too_large;
   case static_cast<int>(code::busy):
      return code::busy;
   case static_cast<int>(code::stopped):
      return code::stopped;
   default:
      return std::nullopt;
   }
}

[[nodiscard]] inline bool is(const forge::exceptions::base& value, code expected) noexcept {
   return value.code() == forge::exceptions::make_error_code(expected);
}

} // namespace forge::auth::oauth2::exceptions
