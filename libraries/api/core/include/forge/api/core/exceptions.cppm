module;

#include <boost/describe.hpp>
#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.api.core.exceptions;

export import forge.exceptions;

export namespace forge::api::core::exceptions {

enum class code : std::uint16_t {
   method_not_found = 1,
   incompatible_version = 2,
   codec_failed = 3,
   deadline_exceeded = 4,
   cancelled = 5,
   remote_internal = 6,
   protocol_error = 7,
   resource_exhausted = 8,
   server_supplied_unavailable = 9,
   invocation_denied = 10,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.api")

using method_not_found = forge::exceptions::coded_exception<code, code::method_not_found>;
using incompatible_version = forge::exceptions::coded_exception<code, code::incompatible_version>;
using codec_failed = forge::exceptions::coded_exception<code, code::codec_failed>;
using deadline_exceeded = forge::exceptions::coded_exception<code, code::deadline_exceeded>;
using cancelled = forge::exceptions::coded_exception<code, code::cancelled>;
using remote_internal = forge::exceptions::coded_exception<code, code::remote_internal>;
using protocol_error = forge::exceptions::coded_exception<code, code::protocol_error>;
using resource_exhausted = forge::exceptions::coded_exception<code, code::resource_exhausted>;
using server_supplied_unavailable = forge::exceptions::coded_exception<code, code::server_supplied_unavailable>;
using invocation_denied = forge::exceptions::coded_exception<code, code::invocation_denied>;

BOOST_DESCRIBE_ENUM(code, method_not_found, incompatible_version, codec_failed, deadline_exceeded, cancelled,
                    remote_internal, protocol_error, resource_exhausted, server_supplied_unavailable, invocation_denied)

} // namespace forge::api::core::exceptions
