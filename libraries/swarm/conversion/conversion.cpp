module;

#include <string>
#include <utility>
#include <forge/exceptions/macros.hpp>

module forge.swarm.conversion.conversion;

namespace forge::core {
namespace protocol = forge::swarm::protocol;
namespace db = forge::swarm::db;
namespace core = forge::swarm::core;

conversion<protocol::metadata>::conversion(const db::content& value) {
   if (value.locator.empty() || value.locator.size() > 4096U || value.locator.find('\0') != std::string::npos ||
       value.expires_at < 0) {
      FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_metadata, "local metadata locator or expiry is invalid");
   }
   switch (value.state) {
   case db::job_state::paused:
   case db::job_state::downloading:
   case db::job_state::seeding:
   case db::job_state::failed:
      break;
   default:
      FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_metadata, "local metadata job state is invalid");
   }
   try {
      if (core::calculate_swarm_id(value.manifest) != value.swarm) {
         FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_metadata,
                               "local metadata identity does not match descriptor");
      }
      if (value.checkpoint)
         core::validate_checkpoint(*value.checkpoint, value.manifest);
   } catch (const forge::exceptions::context_error& error) {
      if (error.code().category() !=
          core::exceptions::make_error_code(core::exceptions::code::invalid_descriptor).category())
         throw;
      FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_metadata, "local descriptor or checkpoint is inconsistent");
   }
   _value.emplace(protocol::metadata{value.swarm, value.manifest});
}

protocol::metadata conversion<protocol::metadata>::finish() && {
   if (!_value)
      FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_metadata, "metadata conversion was already consumed");
   auto result = std::move(*_value);
   _value.reset();
   return result;
}

conversion<core::descriptor>::conversion(protocol::metadata value) {
   protocol::validate(value);
   _value.emplace(std::move(value.manifest));
}

core::descriptor conversion<core::descriptor>::finish() && {
   if (!_value)
      FORGE_THROW_EXCEPTION(protocol::exceptions::invalid_metadata, "descriptor conversion was already consumed");
   auto result = std::move(*_value);
   _value.reset();
   return result;
}

} // namespace forge::core
