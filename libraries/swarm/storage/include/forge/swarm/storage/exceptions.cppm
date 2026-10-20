module;

#include <cstdint>
#include <forge/exceptions/macros.hpp>

export module forge.swarm.storage.exceptions;

export import forge.exceptions;

export namespace forge::swarm::storage::exceptions {

enum class code : std::uint16_t {
   io_error = 1,
   unsafe_path = 2,
   conflict = 3,
   missing_data = 4,
   cross_device = 5,
   invalid_state = 6,
   invalid_options = 7,
   corrupt_checkpoint = 8,
   schema_mismatch = 9,
   stale_checkpoint = 10,
   missing_metadata = 11,
   invalid_metadata = 12,
};

using io_error = forge::exceptions::coded_exception<code, code::io_error>;
using unsafe_path = forge::exceptions::coded_exception<code, code::unsafe_path>;
using conflict = forge::exceptions::coded_exception<code, code::conflict>;
using missing_data = forge::exceptions::coded_exception<code, code::missing_data>;
using cross_device = forge::exceptions::coded_exception<code, code::cross_device>;
using invalid_state = forge::exceptions::coded_exception<code, code::invalid_state>;
using invalid_options = forge::exceptions::coded_exception<code, code::invalid_options>;
using corrupt_checkpoint = forge::exceptions::coded_exception<code, code::corrupt_checkpoint>;
using schema_mismatch = forge::exceptions::coded_exception<code, code::schema_mismatch>;
using stale_checkpoint = forge::exceptions::coded_exception<code, code::stale_checkpoint>;
using missing_metadata = forge::exceptions::coded_exception<code, code::missing_metadata>;
using invalid_metadata = forge::exceptions::coded_exception<code, code::invalid_metadata>;

FORGE_DECLARE_EXCEPTION_CATEGORY(code, "forge.swarm.storage")

} // namespace forge::swarm::storage::exceptions
