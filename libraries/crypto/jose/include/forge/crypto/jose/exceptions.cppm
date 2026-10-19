module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.crypto.jose.exceptions;

export import forge.exceptions;

export namespace forge::crypto::jose::exceptions {

enum class code : std::uint16_t {
   invalid_policy = 1,
   invalid_key = 2,
   invalid_compact = 3,
   size_limit_exceeded = 4,
   invalid_header = 5,
   unknown_key = 6,
   invalid_signature = 7,
   invalid_state = 8,
   backend_error = 9,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.jose")

using invalid_policy = forge::exceptions::coded_exception<code, code::invalid_policy>;
using invalid_key = forge::exceptions::coded_exception<code, code::invalid_key>;
using invalid_compact = forge::exceptions::coded_exception<code, code::invalid_compact>;
using size_limit_exceeded = forge::exceptions::coded_exception<code, code::size_limit_exceeded>;
using invalid_header = forge::exceptions::coded_exception<code, code::invalid_header>;
using unknown_key = forge::exceptions::coded_exception<code, code::unknown_key>;
using invalid_signature = forge::exceptions::coded_exception<code, code::invalid_signature>;
using invalid_state = forge::exceptions::coded_exception<code, code::invalid_state>;
using backend_error = forge::exceptions::coded_exception<code, code::backend_error>;

} // namespace forge::crypto::jose::exceptions
