module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>

export module forge.plugins.net.p2p.pubsub.exceptions;

import forge.exceptions;

export namespace forge::plugins::net::p2p::pubsub {

class exceptions {
 public:
   enum class code : std::uint16_t {
      plugin_not_initialized = 1,
      invalid_config = 2,
      topic_not_allowed = 3,
      subscription_not_found = 4,
      handler_limit = 5,
      message_too_large = 6,
   };

   using plugin_not_initialized = forge::exceptions::coded_exception<code, code::plugin_not_initialized>;
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using topic_not_allowed = forge::exceptions::coded_exception<code, code::topic_not_allowed>;
   using subscription_not_found = forge::exceptions::coded_exception<code, code::subscription_not_found>;
   using handler_limit = forge::exceptions::coded_exception<code, code::handler_limit>;
   using message_too_large = forge::exceptions::coded_exception<code, code::message_too_large>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.p2p.pubsub")

} // namespace forge::plugins::net::p2p::pubsub
