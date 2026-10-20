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
   invalid_checkpoint = 8,
   storage_closed = 9,
   storage_busy = 10,
   unverified_piece = 11,
   invalid_write = 12,
   integrity_failure = 13,
   invalid_transfer = 14,
};

using unsupported_version = forge::exceptions::coded_exception<code, code::unsupported_version>;
using invalid_descriptor = forge::exceptions::coded_exception<code, code::invalid_descriptor>;
using invalid_path = forge::exceptions::coded_exception<code, code::invalid_path>;
using limit_exceeded = forge::exceptions::coded_exception<code, code::limit_exceeded>;
using invalid_geometry = forge::exceptions::coded_exception<code, code::invalid_geometry>;
using invalid_content = forge::exceptions::coded_exception<code, code::invalid_content>;
using invalid_encoding = forge::exceptions::coded_exception<code, code::invalid_encoding>;
using invalid_checkpoint = forge::exceptions::coded_exception<code, code::invalid_checkpoint>;
using storage_closed = forge::exceptions::coded_exception<code, code::storage_closed>;
using storage_busy = forge::exceptions::coded_exception<code, code::storage_busy>;
using unverified_piece = forge::exceptions::coded_exception<code, code::unverified_piece>;
using invalid_write = forge::exceptions::coded_exception<code, code::invalid_write>;
using integrity_failure = forge::exceptions::coded_exception<code, code::integrity_failure>;
using invalid_transfer = forge::exceptions::coded_exception<code, code::invalid_transfer>;

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.swarm.core")

} // namespace forge::swarm::core::exceptions
