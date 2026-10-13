module;

#include <cstdint>
#include <optional>
#include <string_view>

#include <forge/exceptions/macros.hpp>

export module forge.auth.credential.exceptions;

export import forge.exceptions;

export namespace forge::auth::credential::exceptions {

enum class code : std::uint16_t {
   invalid_state = 6,
   scope_invalid = 7,
   identity_invalid = 8,
   generation_exhausted = 9,
   credential_id_invalid = 10,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.auth.credential")

using invalid_state = forge::exceptions::coded_exception<code, code::invalid_state>;
using scope_invalid = forge::exceptions::coded_exception<code, code::scope_invalid>;
using identity_invalid = forge::exceptions::coded_exception<code, code::identity_invalid>;
using generation_exhausted = forge::exceptions::coded_exception<code, code::generation_exhausted>;
using credential_id_invalid = forge::exceptions::coded_exception<code, code::credential_id_invalid>;

[[nodiscard]] inline std::optional<code> code_of(const forge::exceptions::base& value) noexcept {
   const auto& actual = value.code();
   if (!actual || std::string_view{actual.category().name()} != "forge.auth.credential") {
      return std::nullopt;
   }
   switch (actual.value()) {
   case static_cast<int>(code::invalid_state):
      return code::invalid_state;
   case static_cast<int>(code::scope_invalid):
      return code::scope_invalid;
   case static_cast<int>(code::identity_invalid):
      return code::identity_invalid;
   case static_cast<int>(code::generation_exhausted):
      return code::generation_exhausted;
   case static_cast<int>(code::credential_id_invalid):
      return code::credential_id_invalid;
   default:
      return std::nullopt;
   }
}

[[nodiscard]] inline bool is(const forge::exceptions::base& value, code expected) noexcept {
   return value.code() == forge::exceptions::make_error_code(expected);
}

} // namespace forge::auth::credential::exceptions
