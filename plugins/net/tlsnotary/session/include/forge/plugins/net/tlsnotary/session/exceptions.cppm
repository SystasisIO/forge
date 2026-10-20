module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.plugins.net.tlsnotary.session.exceptions;

import forge.exceptions;

export namespace forge::plugins::net::tlsnotary::session {

class exceptions {
 public:
   enum class code : std::uint16_t { invalid_config = 1, not_ready, startup_failed };
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using not_ready = forge::exceptions::coded_exception<code, code::not_ready>;
   using startup_failed = forge::exceptions::coded_exception<code, code::startup_failed>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.plugins.net.tlsnotary.session")
} // namespace forge::plugins::net::tlsnotary::session
