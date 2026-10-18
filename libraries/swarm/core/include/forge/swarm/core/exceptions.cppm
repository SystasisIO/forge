module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.swarm.core.exceptions;

export import forge.exceptions;

export namespace forge::swarm::core::exceptions {

enum class code : std::uint16_t {
   unsupported_version = 1,
   invalid_descriptor = 2,
   invalid_path = 3,
   limit_exceeded = 4,
   invalid_geometry = 5,
   invalid_content = 6,
   invalid_encoding = 7,
};

using unsupported_version = forge::exceptions::coded_exception<code, code::unsupported_version>;
using invalid_descriptor = forge::exceptions::coded_exception<code, code::invalid_descriptor>;
using invalid_path = forge::exceptions::coded_exception<code, code::invalid_path>;
using limit_exceeded = forge::exceptions::coded_exception<code, code::limit_exceeded>;
using invalid_geometry = forge::exceptions::coded_exception<code, code::invalid_geometry>;
using invalid_content = forge::exceptions::coded_exception<code, code::invalid_content>;
using invalid_encoding = forge::exceptions::coded_exception<code, code::invalid_encoding>;

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.swarm.core")

} // namespace forge::swarm::core::exceptions
