module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/steady_timer.hpp>

#include <chrono>
#include <cstddef>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

module forge.plugins.net.p2p.resolver.plugin;

import forge.api.core.connection;
import forge.api.core.descriptor;
import forge.api.core.types;
import forge.api.transport.connection;
import forge.asio.notification;
import forge.net.p2p.identity;
import forge.plugins.net.p2p.resolver.exceptions;
import forge.plugins.net.p2p.resolver.managed_api;
import forge.plugins.net.p2p.resolver.types;
import forge.plugins.net.p2p.node.api;

#include "details/plugin_impl.hxx"
#include "details/managed_api_impl.hxx"
#include "details/managed_remote_invoker.hxx"

namespace forge::plugins::net::p2p::resolver {

plugin::managed_api_impl::managed_api_impl(std::shared_ptr<plugin::impl> impl) : impl_{std::move(impl)} {}

boost::asio::awaitable<std::shared_ptr<forge::api::core::remote_invoker>>
plugin::managed_api_impl::open_managed_remote(std::vector<forge::net::p2p::peer_id> ordered_peers,
                                              forge::api::core::api_ref requested,
                                              forge::api::core::descriptor descriptor, managed_remote_options options) {
   if (ordered_peers.size() > impl_->settings.max_managed_peers) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_remote, "managed remote peer limit exceeded");
   }
   auto value = std::make_shared<detail::managed_remote_invoker>(
       impl_, std::move(ordered_peers), std::move(requested), std::move(descriptor), options,
       static_cast<std::size_t>(impl_->settings.max_managed_waiters));
   impl_->register_managed(value);
   co_await value->connect_initial();
   co_return value;
}

} // namespace forge::plugins::net::p2p::resolver
