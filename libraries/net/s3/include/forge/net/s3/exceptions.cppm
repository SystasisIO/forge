module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.net.s3.exceptions;

export import forge.exceptions;

export namespace forge::net::s3::exceptions {

enum class code : std::uint16_t {
   invalid_options = 1,
   busy,
   stopped,
   canceled,
   deadline,
   not_found,
   denied,
   conflict,
   transport,
   service,
   unknown_outcome,
   limit,
   io,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.net.s3")

using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using busy = forge::exceptions::coded_exception<code, code::busy>;
using stopped = forge::exceptions::coded_exception<code, code::stopped>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;
using deadline = forge::exceptions::coded_exception<code, code::deadline>;
using not_found = forge::exceptions::coded_exception<code, code::not_found>;
using denied = forge::exceptions::coded_exception<code, code::denied>;
using conflict = forge::exceptions::coded_exception<code, code::conflict>;
using transport = forge::exceptions::coded_exception<code, code::transport>;
using service = forge::exceptions::coded_exception<code, code::service>;
using unknown_outcome = forge::exceptions::coded_exception<code, code::unknown_outcome>;
using limit = forge::exceptions::coded_exception<code, code::limit>;
using io = forge::exceptions::coded_exception<code, code::io>;

} // namespace forge::net::s3::exceptions
