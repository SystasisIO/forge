module;

#include <forge/exceptions/macros.hpp>

#include <cstdint>

export module forge.plugins.net.http.server.exceptions;

import forge.exceptions;

export namespace forge::plugins::net::http::server {

class exceptions {
 public:
   enum class code : std::uint16_t {
      invalid_config = 1,
      publication_closed = 2,
      startup_failed = 3,
      tls_reload_failed = 4,
   };

   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using publication_closed = forge::exceptions::coded_exception<code, code::publication_closed>;
   using startup_failed = forge::exceptions::coded_exception<code, code::startup_failed>;
   using tls_reload_failed = forge::exceptions::coded_exception<code, code::tls_reload_failed>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.http.server")

} // namespace forge::plugins::net::http::server
