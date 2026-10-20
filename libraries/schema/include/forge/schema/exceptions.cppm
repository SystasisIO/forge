module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.schema.exceptions;

export import forge.exceptions;

export namespace forge::schema::exceptions {

enum class code : std::uint16_t {
   invalid_value = 1,
   unsupported_export = 2,
   constraint_limit = 3,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.schema")

using invalid_value = forge::exceptions::coded_exception<code, code::invalid_value>;
using unsupported_export = forge::exceptions::coded_exception<code, code::unsupported_export>;
using constraint_limit = forge::exceptions::coded_exception<code, code::constraint_limit>;

} // namespace forge::schema::exceptions
