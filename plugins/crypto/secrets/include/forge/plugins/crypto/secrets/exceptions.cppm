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
      invalid_request = 10,
      version_not_found = 11,
      revision_conflict = 12,
      idempotency_conflict = 13,
      version_revoked = 14,
      store_unavailable = 15,
      busy = 16,
      capacity_exceeded = 17,
      ownership_lost = 18,
      durability_unknown = 19,
      invalid_store = 20,
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
   using invalid_request = forge::exceptions::coded_exception<code, code::invalid_request>;
   using version_not_found = forge::exceptions::coded_exception<code, code::version_not_found>;
   using revision_conflict = forge::exceptions::coded_exception<code, code::revision_conflict>;
   using idempotency_conflict = forge::exceptions::coded_exception<code, code::idempotency_conflict>;
   using version_revoked = forge::exceptions::coded_exception<code, code::version_revoked>;
   using store_unavailable = forge::exceptions::coded_exception<code, code::store_unavailable>;
   using busy = forge::exceptions::coded_exception<code, code::busy>;
   using capacity_exceeded = forge::exceptions::coded_exception<code, code::capacity_exceeded>;
   using ownership_lost = forge::exceptions::coded_exception<code, code::ownership_lost>;
   using durability_unknown = forge::exceptions::coded_exception<code, code::durability_unknown>;
   using invalid_store = forge::exceptions::coded_exception<code, code::invalid_store>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.crypto.secrets")

} // namespace forge::plugins::crypto::secrets
