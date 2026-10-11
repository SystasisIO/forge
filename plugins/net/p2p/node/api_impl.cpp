module;

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <list>
#include <memory>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.node.plugin;

import forge.api.core.binding;
import forge.api.transport.connection;
import forge.api.transport.options;
import forge.asio.notification;
import forge.asio.runtime;
import forge.asio.task;
import forge.api.p2p.binding;
import forge.api.p2p.publication;
import forge.exceptions;
import forge.net.p2p.dht.record_store;
import forge.net.p2p.endpoint;
import forge.net.p2p.identity;
import forge.net.p2p.node;
import forge.net.p2p.topology;
import forge.multiformats.multiaddr;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.scoring;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.exceptions;
import forge.plugins.net.p2p.node.protocol_registration;
import forge.plugins.net.p2p.node.types;
import forge.plugins.net.p2p.node.peer_contribution;
import forge.plugins.crypto.secrets.api;
import forge.plugins.db.store.api;

#include "details/plugin_impl.hxx"
#include "details/api_impl.hxx"
#include "details/protocol_registry.hxx"
namespace forge::plugins::net::p2p::node {

plugin::api_impl::api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

forge::net::p2p::peer_id plugin::api_impl::local_peer() const {
   return impl_->require_node()->local_peer();
}

std::optional<forge::net::p2p::endpoint> plugin::api_impl::local_endpoint() const {
   return impl_->require_node()->local_endpoint();
}

std::vector<forge::net::p2p::endpoint> plugin::api_impl::local_endpoints() const {
   return impl_->require_node()->local_endpoints();
}

info plugin::api_impl::network_info() const {
   const auto current = impl_->require_node();
   return info{
       .local_peer = current->local_peer(),
       .local_endpoints = current->local_endpoints(),
       .started = impl_->is_started(),
   };
}

forge::api::p2p::publication
plugin::api_impl::publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol) {
   return publish_api(std::move(plan), std::move(protocol), impl_->api_options);
}

forge::api::p2p::publication
plugin::api_impl::publish_api(forge::api::core::binding_plan plan, forge::net::p2p::protocol_id protocol,
                              forge::api::transport::options options) {
   options.max_item_size = std::min(options.max_item_size, options.max_frame_size);
   auto binding =
       forge::api::p2p::api().use(std::move(plan)).protocol_id(protocol).session_options(std::move(options)).build();
   if (impl_->stop_requested.load(std::memory_order_acquire)) {
      FORGE_THROW_EXCEPTION(exceptions::route_conflict, "P2P API publications are closed");
   }
   return detail::publish_api_publication(impl_->protocols, std::move(binding));
}

boost::asio::awaitable<forge::api::transport::connection>
plugin::api_impl::open_api_connection(forge::net::p2p::peer_id peer, forge::net::p2p::protocol_id protocol,
                                      remote_options options) {
   return open_owned(impl_, std::move(peer), std::move(protocol), std::move(options));
}

boost::asio::awaitable<forge::api::transport::connection>
plugin::api_impl::open_owned(std::shared_ptr<plugin::impl> owner, forge::net::p2p::peer_id peer,
                             forge::net::p2p::protocol_id protocol, remote_options options) {
   const auto current = owner->require_running_node();
   auto stream = co_await current->async_open_protocol_stream(std::move(peer), std::move(protocol),
                                                              owner->open_options_for(options));
   co_return forge::api::transport::connection{std::move(stream).into_transport_stream(),
                                               owner->api_options_for(options)};
}

} // namespace forge::plugins::net::p2p::node
