module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.app.exceptions;

export import forge.exceptions;

export namespace forge::app::exceptions {

enum class code : std::uint16_t {
   invalid_state = 1,
   config_failed = 2,
   plugin_dependency_missing = 3,
   api_missing = 4,
   api_version_mismatch = 5,
   startup_failed = 6,
   shutdown_failed = 7,
   initialize_failed = 8,
   invalid_service = 9,
   service_missing = 10,
   service_already_published = 11,
   service_publication_closed = 12,
   service_registry_detached = 13,
};

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.app")

using invalid_state = forge::exceptions::coded_exception<code, code::invalid_state>;
using config_failed = forge::exceptions::coded_exception<code, code::config_failed>;
using plugin_dependency_missing = forge::exceptions::coded_exception<code, code::plugin_dependency_missing>;
using api_missing = forge::exceptions::coded_exception<code, code::api_missing>;
using api_version_mismatch = forge::exceptions::coded_exception<code, code::api_version_mismatch>;
using startup_failed = forge::exceptions::coded_exception<code, code::startup_failed>;
using shutdown_failed = forge::exceptions::coded_exception<code, code::shutdown_failed>;
using initialize_failed = forge::exceptions::coded_exception<code, code::initialize_failed>;
using invalid_service = forge::exceptions::coded_exception<code, code::invalid_service>;
using service_missing = forge::exceptions::coded_exception<code, code::service_missing>;
using service_already_published = forge::exceptions::coded_exception<code, code::service_already_published>;
using service_publication_closed = forge::exceptions::coded_exception<code, code::service_publication_closed>;
using service_registry_detached = forge::exceptions::coded_exception<code, code::service_registry_detached>;

// Framework normalization of a successful hook whose admission was closed.
// Preserve invalid_state code/catch compatibility without late-phase inference.
class operation_stopped final : public invalid_state {
 public:
   using invalid_state::invalid_state;
};

} // namespace forge::app::exceptions
