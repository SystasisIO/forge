module;

#include <forge/exceptions/macros.hpp>

#include <memory>
#include <mutex>
#include <optional>

module forge.plugins.net.p2p.diagnostics.plugin;

import forge.net.p2p.diagnostics;
import forge.net.p2p.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.host_event_source;
import forge.plugins.net.p2p.diagnostics.exceptions;
import forge.plugins.net.p2p.diagnostics.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::diagnostics {

std::shared_ptr<forge::plugins::net::p2p::node::host_event_source> plugin::impl::require_events_source() const {
   const auto lock = std::scoped_lock{mutex};
   if (stopping) {
      FORGE_THROW_EXCEPTION(forge::net::p2p::exceptions::canceled, "P2P diagnostics event admission is closed");
   }
   if (!initialized || !events_source) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P diagnostics plugin is not initialized");
   }
   return events_source;
}

forge::net::p2p::diagnostics::snapshot
plugin::impl::snapshot(std::optional<forge::net::p2p::diagnostics::options> options) const {
   auto current = std::shared_ptr<forge::plugins::net::p2p::node::diagnostics_source>{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (!initialized || !source) {
         FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P diagnostics plugin is not initialized");
      }
      current = source;
      if (!options) {
         options = configured_options(settings);
      }
   }
   return current->snapshot(*options);
}

} // namespace forge::plugins::net::p2p::diagnostics
