module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.tooling.testing.exceptions;

export import forge.exceptions;

export namespace forge::tooling::testing::exceptions {

enum class code : std::uint16_t {
   assertion_failure = 1,
   database_error = 2,
   invalid_iterator = 3,
   invalid_options = 4,
   execution_timeout = 5,
   expression_unavailable = 6,
   execution_cancelled = 7,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.tooling.testing")

using assertion_failure = forge::exceptions::coded_exception<code, code::assertion_failure>;
using database_error = forge::exceptions::coded_exception<code, code::database_error>;
using invalid_iterator = forge::exceptions::coded_exception<code, code::invalid_iterator>;
using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using execution_timeout = forge::exceptions::coded_exception<code, code::execution_timeout>;
using expression_unavailable = forge::exceptions::coded_exception<code, code::expression_unavailable>;
using execution_cancelled = forge::exceptions::coded_exception<code, code::execution_cancelled>;

} // namespace forge::tooling::testing::exceptions
