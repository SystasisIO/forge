module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.crypto.keystore.exceptions;

export import forge.exceptions;

export namespace forge::crypto::keystore::exceptions {

enum class code : std::uint16_t {
   invalid_file = 1,
   size_limit_exceeded = 2,
   io_error = 3,
   invalid_options = 4,
   duplicate_key = 5,
   unknown_key = 6,
   password_unavailable = 7,
   durability_unknown = 8,
   in_use = 9,
   ownership_lost = 10,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.keystore")

using invalid_file = forge::exceptions::coded_exception<code, code::invalid_file>;
using size_limit_exceeded = forge::exceptions::coded_exception<code, code::size_limit_exceeded>;
using io_error = forge::exceptions::coded_exception<code, code::io_error>;
using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using duplicate_key = forge::exceptions::coded_exception<code, code::duplicate_key>;
using unknown_key = forge::exceptions::coded_exception<code, code::unknown_key>;
using password_unavailable = forge::exceptions::coded_exception<code, code::password_unavailable>;
using durability_unknown = forge::exceptions::coded_exception<code, code::durability_unknown>;
using in_use = forge::exceptions::coded_exception<code, code::in_use>;
using ownership_lost = forge::exceptions::coded_exception<code, code::ownership_lost>;

} // namespace forge::crypto::keystore::exceptions
