module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>

export module forge.plugins.crypto.secrets.exceptions;

import forge.exceptions;

export namespace forge::plugins::crypto::secrets {

class exceptions {
 public:
   enum class code : std::uint16_t {
      invalid_config = 1,
      secret_not_found = 2,
      purpose_denied = 3,
      operation_denied = 4,
      invalid_secret = 5,
      invalid_source = 6,
      size_limit_exceeded = 7,
      crypto_failed = 8,
      stopped = 9,
   };

   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using secret_not_found = forge::exceptions::coded_exception<code, code::secret_not_found>;
   using purpose_denied = forge::exceptions::coded_exception<code, code::purpose_denied>;
   using operation_denied = forge::exceptions::coded_exception<code, code::operation_denied>;
   using invalid_secret = forge::exceptions::coded_exception<code, code::invalid_secret>;
   using invalid_source = forge::exceptions::coded_exception<code, code::invalid_source>;
   using size_limit_exceeded = forge::exceptions::coded_exception<code, code::size_limit_exceeded>;
   using crypto_failed = forge::exceptions::coded_exception<code, code::crypto_failed>;
   using stopped = forge::exceptions::coded_exception<code, code::stopped>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.crypto.secrets")

} // namespace forge::plugins::crypto::secrets
