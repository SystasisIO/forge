module;

#include <cstdint>

#if !defined(FORGE_CONTRACT_GUEST)
#include <forge/exceptions/macros.hpp>
#endif

export module forge.auth.attestation.exceptions;

#if !defined(FORGE_CONTRACT_GUEST)
export import forge.exceptions;
#endif

export namespace forge::auth::attestation::exceptions {

enum class code : std::uint16_t {
   invalid_document = 1,
   invalid_profile = 2,
   unsupported_version = 3,
   profile_mismatch = 4,
   expired = 5,
   not_yet_valid = 6,
   limit_exceeded = 7,
};

#if defined(FORGE_CONTRACT_GUEST)
struct invalid_document final {};
struct invalid_profile final {};
struct unsupported_version final {};
struct profile_mismatch final {};
struct expired final {};
struct not_yet_valid final {};
struct limit_exceeded final {};
#else
FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.auth.attestation")

using invalid_document = forge::exceptions::coded_exception<code, code::invalid_document>;
using invalid_profile = forge::exceptions::coded_exception<code, code::invalid_profile>;
using unsupported_version = forge::exceptions::coded_exception<code, code::unsupported_version>;
using profile_mismatch = forge::exceptions::coded_exception<code, code::profile_mismatch>;
using expired = forge::exceptions::coded_exception<code, code::expired>;
using not_yet_valid = forge::exceptions::coded_exception<code, code::not_yet_valid>;
using limit_exceeded = forge::exceptions::coded_exception<code, code::limit_exceeded>;
#endif

} // namespace forge::auth::attestation::exceptions
