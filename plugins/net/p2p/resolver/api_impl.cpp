module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/awaitable.hpp>

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.descriptor;
import forge.api.p2p.publication;
import forge.api.transport.connection;
import forge.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.protocol;
import forge.plugins.net.p2p.resolver.api;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.types;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.types;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"

namespace forge::plugins::net::p2p::resolver {

plugin::api_impl::api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

forge::api::p2p::publication
plugin::api_impl::publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
                              publish_options options) {
   return impl_->add_local(std::move(plan), std::move(protocol), std::move(options));
}

std::vector<entry> plugin::api_impl::local_apis() const {
   auto p2p = impl_->require_p2p();
   static_cast<void>(p2p);
   return impl_->local_snapshot();
}

boost::asio::awaitable<std::vector<entry>> plugin::api_impl::peer_apis(forge::net::p2p::peer_id peer,
                                                                       resolve_options options) {
   auto p2p = impl_->require_p2p();
   static_cast<void>(p2p);
   if (auto cached = impl_->cached_peer(peer, options)) {
      co_return *cached;
   }

   auto entries = co_await impl_->query_remote_apis(peer, options);
   impl_->validate_response(entries);
   impl_->store_peer(peer, entries);
   co_return entries;
}

boost::asio::awaitable<resolution> plugin::api_impl::resolve(forge::net::p2p::peer_id peer,
                                                             forge::api::core::api_ref api, resolve_options options) {
   auto p2p = impl_->require_p2p();
   static_cast<void>(p2p);
   co_return co_await impl_->resolve_remote(std::move(peer), std::move(api), options);
}

boost::asio::awaitable<resolved_connection>
plugin::api_impl::open_resolved_connection(forge::net::p2p::peer_id peer, forge::api::core::api_ref api,
                                           forge::api::core::descriptor descriptor, resolve_options options) {
   co_return co_await impl_->open_resolved_connection(std::move(peer), std::move(api), std::move(descriptor), options);
}

} // namespace forge::plugins::net::p2p::resolver
