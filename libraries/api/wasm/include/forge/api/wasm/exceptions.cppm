module;

#include <cstdint>

#if !defined(FORGE_CONTRACT_GUEST)
#include <forge/exceptions/macros.hpp>
#endif

export module forge.api.wasm.exceptions;

#if !defined(FORGE_CONTRACT_GUEST)
import forge.exceptions;
#endif

export namespace forge::api::wasm::exceptions {

enum class code : std::uint16_t {
   invalid_value = 1,
   limit_exceeded = 2,
   wrong_type = 3,
   unsupported_version = 4,
   forbidden_effect = 5,
   invalid_capability = 6,
   invalid_http_response = 7,
};

#if defined(FORGE_CONTRACT_GUEST)
struct invalid_value final {};
struct limit_exceeded final {};
struct wrong_type final {};
struct unsupported_version final {};
struct forbidden_effect final {};
struct invalid_capability final {};
struct invalid_http_response final {};
#else
using invalid_value = forge::exceptions::coded_exception<code, code::invalid_value>;
using limit_exceeded = forge::exceptions::coded_exception<code, code::limit_exceeded>;
using wrong_type = forge::exceptions::coded_exception<code, code::wrong_type>;
using unsupported_version = forge::exceptions::coded_exception<code, code::unsupported_version>;
using forbidden_effect = forge::exceptions::coded_exception<code, code::forbidden_effect>;
using invalid_capability = forge::exceptions::coded_exception<code, code::invalid_capability>;
using invalid_http_response = forge::exceptions::coded_exception<code, code::invalid_http_response>;

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.api.wasm")
#endif

} // namespace forge::api::wasm::exceptions
