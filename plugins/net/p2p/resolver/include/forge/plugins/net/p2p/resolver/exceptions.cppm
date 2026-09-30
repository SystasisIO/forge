module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>

export module forge.plugins.net.p2p.resolver.exceptions;

import forge.exceptions;

export namespace forge::plugins::net::p2p::resolver {

class exceptions {
 public:
   enum class code : std::uint16_t {
      plugin_not_initialized = 1,
      invalid_config = 2,
      duplicate_api = 3,
      not_found = 4,
      incompatible_api = 5,
      protocol_error = 6,
      invalid_remote = 7,
      remote_unavailable = 8,
      remote_stopped = 9,
   };

   using plugin_not_initialized = forge::exceptions::coded_exception<code, code::plugin_not_initialized>;
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using duplicate_api = forge::exceptions::coded_exception<code, code::duplicate_api>;
   using not_found = forge::exceptions::coded_exception<code, code::not_found>;
   using incompatible_api = forge::exceptions::coded_exception<code, code::incompatible_api>;
   using protocol_error = forge::exceptions::coded_exception<code, code::protocol_error>;
   using invalid_remote = forge::exceptions::coded_exception<code, code::invalid_remote>;
   using remote_unavailable = forge::exceptions::coded_exception<code, code::remote_unavailable>;
   using remote_stopped = forge::exceptions::coded_exception<code, code::remote_stopped>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.p2p.resolver")

} // namespace forge::plugins::net::p2p::resolver
