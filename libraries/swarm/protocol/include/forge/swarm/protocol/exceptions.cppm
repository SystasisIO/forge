module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.swarm.protocol.exceptions;

export import forge.exceptions;

export namespace forge::swarm::protocol::exceptions {

enum class code : std::uint16_t {
   invalid_metadata = 1,
   invalid_message = 2,
   invalid_encoding = 3,
   unsupported_version = 4,
};

using invalid_metadata = forge::exceptions::coded_exception<code, code::invalid_metadata>;
using invalid_message = forge::exceptions::coded_exception<code, code::invalid_message>;
using invalid_encoding = forge::exceptions::coded_exception<code, code::invalid_encoding>;
using unsupported_version = forge::exceptions::coded_exception<code, code::unsupported_version>;

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.swarm.protocol")

} // namespace forge::swarm::protocol::exceptions
