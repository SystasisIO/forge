module;

#include <boost/asio/awaitable.hpp>
#include <memory>
#include <mutex>
#include <forge/exceptions/macros.hpp>

module forge.plugins.net.tlsnotary.session.plugin;

import forge.app.plugin_lifecycle;
import forge.asio.compute;
import forge.asio.runtime;
import forge.net.tlsnotary.engine;
import forge.plugins.net.tlsnotary.session.config;
import forge.plugins.net.tlsnotary.session.exceptions;
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::tlsnotary::session {

plugin::impl::impl(forge::app::plugin_lifecycle state) : lifecycle{std::move(state)} {}
std::shared_ptr<forge::net::tlsnotary::engine> plugin::impl::require_engine() const {
   auto result = std::shared_ptr<forge::net::tlsnotary::engine>{};
   const auto state = lifecycle.snapshot();
   if (state.phase != forge::app::lifecycle_phase::started || state.stop_requested)
      FORGE_THROW_EXCEPTION(exceptions::not_ready, "TLSNotary plugin is not running");
   const auto lock = std::scoped_lock{mutex};
   if (!engine)
      FORGE_THROW_EXCEPTION(exceptions::not_ready, "TLSNotary engine is unavailable");
   return engine;
}
} // namespace forge::plugins::net::tlsnotary::session
