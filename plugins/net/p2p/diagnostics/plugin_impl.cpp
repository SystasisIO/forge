module;

#include <forge/exceptions/macros.hpp>

#include <memory>

module forge.plugins.net.p2p.diagnostics.plugin;

import forge.net.p2p.diagnostics;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.diagnostics.exceptions;
import forge.plugins.net.p2p.diagnostics.types;

#include "details/config.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::diagnostics {

forge::plugins::net::p2p::node::diagnostics_source& plugin::impl::require_source() const {
   if (!initialized || !source) {
      FORGE_THROW_EXCEPTION(exceptions::plugin_not_initialized, "P2P diagnostics plugin is not initialized");
   }
   return *source;
}

forge::net::p2p::diagnostics::snapshot plugin::impl::snapshot() const {
   return require_source().snapshot(configured_options(settings));
}

} // namespace forge::plugins::net::p2p::diagnostics
