module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.auth.api.exceptions;

import forge.api.core.descriptor;
export import forge.auth.attestation.exceptions;
export import forge.exceptions;

export namespace forge::auth::api::exceptions {

enum class code : std::uint16_t {
   admission_denied = 1,
   ambiguous_profile = 2,
   key_unavailable = 3,
   invalid_backend_signature = 4,
   busy = 5,
   canceled = 6,
   stopped = 7,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.auth.api")

using invalid_document = forge::auth::attestation::exceptions::invalid_document;
using invalid_profile = forge::auth::attestation::exceptions::invalid_profile;
using admission_denied = forge::exceptions::coded_exception<code, code::admission_denied>;
using ambiguous_profile = forge::exceptions::coded_exception<code, code::ambiguous_profile>;
using key_unavailable = forge::exceptions::coded_exception<code, code::key_unavailable>;
using invalid_backend_signature = forge::exceptions::coded_exception<code, code::invalid_backend_signature>;
using busy = forge::exceptions::coded_exception<code, code::busy>;
using canceled = forge::exceptions::coded_exception<code, code::canceled>;
using stopped = forge::exceptions::coded_exception<code, code::stopped>;

namespace descriptor {

template <typename Builder> void declare_signing(Builder& method) {
   namespace core = forge::api::core;
   method.template error<invalid_document>("invalid_document", {.status_code = core::status::invalid_argument});
   method.template error<invalid_profile>("invalid_profile", {.status_code = core::status::failed_precondition});
   method.template error<admission_denied>("admission_denied", {.status_code = core::status::permission_denied});
   method.template error<ambiguous_profile>("ambiguous_profile", {.status_code = core::status::failed_precondition});
   method.template error<key_unavailable>("key_unavailable", {.status_code = core::status::unavailable, .retryable = true});
   method.template error<invalid_backend_signature>("invalid_backend_signature", {.status_code = core::status::internal});
   method.template error<busy>("busy", {.status_code = core::status::resource_exhausted, .retryable = true});
   method.template error<canceled>("canceled", {.status_code = core::status::unavailable});
   method.template error<stopped>("stopped", {.status_code = core::status::unavailable, .retryable = true});
   method.template error<forge::auth::attestation::exceptions::unsupported_version>(
       "unsupported_version", {.status_code = core::status::invalid_argument});
   method.template error<forge::auth::attestation::exceptions::profile_mismatch>(
       "profile_mismatch", {.status_code = core::status::invalid_argument});
   method.template error<forge::auth::attestation::exceptions::expired>(
       "expired", {.status_code = core::status::invalid_argument});
   method.template error<forge::auth::attestation::exceptions::not_yet_valid>(
       "not_yet_valid", {.status_code = core::status::invalid_argument});
   method.template error<forge::auth::attestation::exceptions::limit_exceeded>(
       "limit_exceeded", {.status_code = core::status::invalid_argument});
}

} // namespace descriptor

} // namespace forge::auth::api::exceptions
