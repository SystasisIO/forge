module;

#include <forge/exceptions/macros.hpp>
#include <cstdint>

export module forge.crypto.wallet.exceptions;

export import forge.exceptions;
import forge.api.core.descriptor;

export namespace forge::crypto::wallet::exceptions {

enum class code : std::uint16_t {
   invalid_request = 1,
   permission_denied = 2,
   not_found = 3,
   already_exists = 4,
   locked = 5,
   invalid_password = 6,
   resource_exhausted = 7,
   unavailable = 8,
   storage_error = 9,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.crypto.wallet")
using invalid_request = forge::exceptions::coded_exception<code, code::invalid_request>;
using permission_denied = forge::exceptions::coded_exception<code, code::permission_denied>;
using not_found = forge::exceptions::coded_exception<code, code::not_found>;
using already_exists = forge::exceptions::coded_exception<code, code::already_exists>;
using locked = forge::exceptions::coded_exception<code, code::locked>;
using invalid_password = forge::exceptions::coded_exception<code, code::invalid_password>;
using resource_exhausted = forge::exceptions::coded_exception<code, code::resource_exhausted>;
using unavailable = forge::exceptions::coded_exception<code, code::unavailable>;
using storage_error = forge::exceptions::coded_exception<code, code::storage_error>;

template <typename Builder> void declare(Builder& method) {
   method.template error<invalid_request>(
       "invalid_request", {.status_code = forge::api::core::status::invalid_argument, .retryable = false});
   method.template error<permission_denied>(
       "permission_denied", {.status_code = forge::api::core::status::permission_denied, .retryable = false});
   method.template error<not_found>("not_found",
                                    {.status_code = forge::api::core::status::not_found, .retryable = false});
   method.template error<already_exists>("already_exists",
                                         {.status_code = forge::api::core::status::conflict, .retryable = false});
   method.template error<locked>("locked",
                                 {.status_code = forge::api::core::status::failed_precondition, .retryable = false});
   method.template error<invalid_password>(
       "invalid_password", {.status_code = forge::api::core::status::permission_denied, .retryable = false});
   method.template error<resource_exhausted>(
       "resource_exhausted", {.status_code = forge::api::core::status::resource_exhausted, .retryable = false});
   method.template error<unavailable>("unavailable",
                                      {.status_code = forge::api::core::status::unavailable, .retryable = false});
   method.template error<storage_error>("storage_error",
                                        {.status_code = forge::api::core::status::internal, .retryable = false});
}

} // namespace forge::crypto::wallet::exceptions
