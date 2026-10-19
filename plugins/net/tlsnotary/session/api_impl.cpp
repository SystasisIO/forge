module;

#include <boost/asio/awaitable.hpp>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

module forge.plugins.net.tlsnotary.session.plugin;

import forge.app.plugin_lifecycle;
import forge.asio.runtime;
import forge.asio.compute;
import forge.net.tlsnotary.engine;
import forge.plugins.net.tlsnotary.session.api;
import forge.plugins.net.tlsnotary.session.config;
#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::tlsnotary::session {

plugin::api_impl::api_impl(std::shared_ptr<plugin::impl> impl) : _impl{std::move(impl)} {}
boost::asio::awaitable<forge::net::tlsnotary::result>
plugin::api_impl::prove(forge::net::tlsnotary::prover_request request) {
   return _impl->require_engine()->prove(std::move(request));
}

boost::asio::awaitable<forge::net::tlsnotary::result>
plugin::api_impl::verify(forge::net::tlsnotary::verifier_request request) {
   return _impl->require_engine()->verify(std::move(request));
}

bool plugin::api_impl::cancel(std::uint64_t id) noexcept {
   auto owner = std::shared_ptr<forge::net::tlsnotary::engine>{};
   {
      const auto lock = std::scoped_lock{_impl->mutex};
      owner = _impl->engine;
   }

   return owner && owner->cancel(id);
}

forge::net::tlsnotary::metrics plugin::api_impl::snapshot() const {
   return _impl->require_engine()->snapshot();
}
} // namespace forge::plugins::net::tlsnotary::session
