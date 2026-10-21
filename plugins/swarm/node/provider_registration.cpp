module;

#include <forge/exceptions/macros.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <vector>

module forge.plugins.swarm.node.plugin;

import forge.plugins.swarm.node.exceptions;
import forge.plugins.net.p2p.node.api;
import forge.plugins.net.p2p.node.dht_api;
import forge.net.p2p.provider_registration;
import forge.net.p2p.protocol;
import forge.net.p2p.dht;
import forge.asio.notification;
import forge.swarm.runtime.capabilities;
#include "details/capability_context.hxx"
#include "details/capability_call.hxx"
#include "details/provider_registration.hxx"

namespace forge::plugins::swarm::node {

boost::asio::awaitable<std::unique_ptr<forge::swarm::runtime::capabilities::registration>> provider_registration::open(
   std::shared_ptr<const capability_context> context, forge::net::p2p::dht::key key,
   std::stop_token stop, std::chrono::steady_clock::time_point deadline) {
   auto result = std::make_unique<provider_registration>();
   (void)co_await capability_call<int>::run(open_native(result.get(), context, std::move(key), deadline),
      stop, context->lifetime, deadline, [owner = result.get()] { return owner->async_close(); });
   co_return result;
}
boost::asio::awaitable<int> provider_registration::open_native(provider_registration* owner,
   std::shared_ptr<const capability_context> context, forge::net::p2p::dht::key key,
   std::chrono::steady_clock::time_point deadline) {
   auto options = forge::net::p2p::dht::query_options{};
   options.timeout = std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
   if (options.timeout <= std::chrono::milliseconds::zero()) options.timeout = std::chrono::milliseconds{1};
   owner->_native = co_await context->dht->provide(context->profile, std::move(key), options);
   co_return 0;
}
void provider_registration::cancel() noexcept { _native.request_stop(); }
boost::asio::awaitable<void> provider_registration::async_close() { return _native.async_withdraw(); }

} // namespace forge::plugins::swarm::node
