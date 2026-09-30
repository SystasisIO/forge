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
   };

   using plugin_not_initialized = forge::exceptions::coded_exception<code, code::plugin_not_initialized>;
   using route_conflict = forge::exceptions::coded_exception<code, code::route_conflict>;
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.p2p.node")

} // namespace forge::plugins::net::p2p::node
