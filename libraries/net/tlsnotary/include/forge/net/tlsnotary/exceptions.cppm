module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.net.tlsnotary.exceptions;

import forge.exceptions;

export namespace forge::net::tlsnotary {

class exceptions {
 public:
   enum class code : std::uint16_t {
      invalid_config = 1,
      invalid_request,
      overloaded,
      stopped,
      cancelled,
      deadline_exceeded,
      transport_failed,
      worker_failed,
      protocol_error,
      verification_failed,
      limit_exceeded
   };
   using failure = forge::exceptions::runtime_coded_exception<code>;
   using invalid_config = forge::exceptions::coded_exception<code, code::invalid_config>;
   using invalid_request = forge::exceptions::coded_exception<code, code::invalid_request>;
   using overloaded = forge::exceptions::coded_exception<code, code::overloaded>;
   using stopped = forge::exceptions::coded_exception<code, code::stopped>;
   using cancelled = forge::exceptions::coded_exception<code, code::cancelled>;
   using deadline_exceeded = forge::exceptions::coded_exception<code, code::deadline_exceeded>;
   using transport_failed = forge::exceptions::coded_exception<code, code::transport_failed>;
   using worker_failed = forge::exceptions::coded_exception<code, code::worker_failed>;
   using protocol_error = forge::exceptions::coded_exception<code, code::protocol_error>;
   using verification_failed = forge::exceptions::coded_exception<code, code::verification_failed>;
   using limit_exceeded = forge::exceptions::coded_exception<code, code::limit_exceeded>;
};

FORGE_DECLARE_EXCEPTION_CATEGORY(exceptions::code, "forge.net.tlsnotary")
} // namespace forge::net::tlsnotary
