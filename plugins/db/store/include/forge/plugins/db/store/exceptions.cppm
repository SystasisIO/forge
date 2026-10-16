module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>

export module forge.plugins.db.store.exceptions;

import forge.exceptions;

export namespace forge::plugins::db::store {

class exceptions {
 public:
   enum class code : std::uint16_t {
      invalid_config = 1,
      duplicate_store = 2,
      unknown_store = 3,
      stopped = 4,
      startup_failed = 5,
      invalid_argument = 6,
      unavailable_layer = 7,
      initialize_failed = 8,
      unsupported_activation = 9,
      activation_conflict = 10,
      activation_incomplete = 11,
      internal_error = 255,
   };

   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using duplicate_store = forge::exceptions::coded_exception<code, code::duplicate_store>;
   using unknown_store = forge::exceptions::coded_exception<code, code::unknown_store>;
   using stopped = forge::exceptions::coded_exception<code, code::stopped>;
   using startup_failed = forge::exceptions::coded_exception<code, code::startup_failed>;
   using invalid_argument = forge::exceptions::coded_exception<code, code::invalid_argument>;
   using unavailable_layer = forge::exceptions::coded_exception<code, code::unavailable_layer>;
   using initialize_failed = forge::exceptions::coded_exception<code, code::initialize_failed>;
   using unsupported_activation = forge::exceptions::coded_exception<code, code::unsupported_activation>;
   using activation_conflict = forge::exceptions::coded_exception<code, code::activation_conflict>;
   using activation_incomplete = forge::exceptions::coded_exception<code, code::activation_incomplete>;
   using internal_error = forge::exceptions::coded_exception<code, code::internal_error>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.db.store")

} // namespace forge::plugins::db::store
