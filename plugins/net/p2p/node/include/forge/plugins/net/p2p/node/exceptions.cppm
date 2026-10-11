module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>

export module forge.plugins.net.p2p.node.exceptions;

import forge.exceptions;

export namespace forge::plugins::net::p2p::node {

class exceptions {
 public:
   enum class code : std::uint16_t {
      plugin_not_initialized = 1,
      route_conflict = 2,
      invalid_config = 3,
      topic_not_allowed = 4,
      subscription_not_found = 5,
      handler_limit = 6,
      message_too_large = 7,
      not_found = 8,
   };

   using plugin_not_initialized = forge::exceptions::coded_exception<code, code::plugin_not_initialized>;
   using route_conflict = forge::exceptions::coded_exception<code, code::route_conflict>;
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using topic_not_allowed = forge::exceptions::coded_exception<code, code::topic_not_allowed>;
   using subscription_not_found = forge::exceptions::coded_exception<code, code::subscription_not_found>;
   using handler_limit = forge::exceptions::coded_exception<code, code::handler_limit>;
   using message_too_large = forge::exceptions::coded_exception<code, code::message_too_large>;
   using not_found = forge::exceptions::coded_exception<code, code::not_found>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.p2p.node")

} // namespace forge::plugins::net::p2p::node
