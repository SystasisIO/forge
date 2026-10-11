module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>

export module forge.plugins.net.p2p.node.diagnostics_exceptions;

import forge.exceptions;

export namespace forge::plugins::net::p2p::node {

class diagnostics_exceptions {
 public:
   enum class code : std::uint16_t {
      plugin_not_initialized = 1,
      invalid_config = 2,
      not_found = 3,
   };

   using plugin_not_initialized = forge::exceptions::coded_exception<code, code::plugin_not_initialized>;
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using not_found = forge::exceptions::coded_exception<code, code::not_found>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(diagnostics_exceptions::code, "forge.plugins.net.p2p.node.diagnostics")

} // namespace forge::plugins::net::p2p::node
