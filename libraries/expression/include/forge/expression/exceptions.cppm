module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.expression.exceptions;

export import forge.exceptions;

export namespace forge::expression::exceptions {

enum class code : std::uint16_t {
   invalid_profile = 1,
   invalid_program = 2,
   invalid_input = 3,
   resource_exhausted = 4,
   evaluation_failed = 5,
   internal = 6,
};

using invalid_profile = forge::exceptions::coded_exception<code, code::invalid_profile>;
using invalid_program = forge::exceptions::coded_exception<code, code::invalid_program>;
using invalid_input = forge::exceptions::coded_exception<code, code::invalid_input>;
using resource_exhausted = forge::exceptions::coded_exception<code, code::resource_exhausted>;
using evaluation_failed = forge::exceptions::coded_exception<code, code::evaluation_failed>;
using internal = forge::exceptions::coded_exception<code, code::internal>;

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.expression")

} // namespace forge::expression::exceptions
