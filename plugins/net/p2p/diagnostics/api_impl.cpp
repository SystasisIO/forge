module;

#include <forge/exceptions/macros.hpp>

#include <memory>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.diagnostics.plugin;

import forge.net.p2p.diagnostics;
import forge.net.p2p.identity;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.diagnostics.api;
import forge.plugins.net.p2p.diagnostics.exceptions;
import forge.plugins.net.p2p.diagnostics.types;

#include "details/config.hxx"
#include "details/api_impl.hxx"
#include "details/plugin_impl.hxx"

namespace forge::plugins::net::p2p::diagnostics {

plugin::api_impl::api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

forge::net::p2p::diagnostics::snapshot plugin::api_impl::snapshot() const {
   return impl_->snapshot();
}

forge::net::p2p::diagnostics::snapshot plugin::api_impl::snapshot(forge::net::p2p::diagnostics::options options) const {
   return impl_->require_source().snapshot(options);
}

forge::net::p2p::diagnostics::network_state plugin::api_impl::network() const {
   return impl_->snapshot().network;
}

forge::net::p2p::resource_manager::snapshot plugin::api_impl::resources() const {
   return impl_->snapshot().resources;
}

forge::net::p2p::pubsub::snapshot plugin::api_impl::pubsub() const {
   return impl_->snapshot().pubsub;
}

std::vector<forge::net::p2p::diagnostics::peer> plugin::api_impl::peers(filter value) const {
   return filter_peers(impl_->snapshot(), value);
}

forge::net::p2p::diagnostics::peer plugin::api_impl::peer(forge::net::p2p::peer_id value) const {
   auto values = peers(filter{.peer = std::move(value), .limit = 1});
   if (values.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::not_found, "P2P diagnostics peer was not found");
   }
   return std::move(values.front());
}

} // namespace forge::plugins::net::p2p::diagnostics
