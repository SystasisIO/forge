module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/bind_cancellation_slot.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.diagnostics;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.scoring;
import forge.net.p2p.stream;
import forge.multiformats.multiaddr;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/dht_exchange.hxx"
#include "details/host_addresses.hxx"
#include "details/node_impl.hxx"

namespace forge::net::p2p {

void node::impl::initialize_dht_routing_refresh() {
   auto profiles = std::vector<detail::dht_routing_refresh::profile>{};
   profiles.reserve(dht_profiles.size());
   for (auto& [protocol, state] : dht_profiles) {
      if (!state->profile.capabilities.peers) {
         continue;
      }
      profiles.push_back(detail::dht_routing_refresh::profile{
          .protocol = protocol,
          .routing = &state->routing,
          .interval = state->profile.limits.refresh_interval,
          .query_timeout = state->profile.limits.query_timeout,
      });
   }
   if (profiles.empty()) {
      return;
   }

   const auto weak = weak_from_this();
   routing_refresh = std::make_shared<detail::dht_routing_refresh>(
       local, std::move(profiles),
       [weak](protocol_id protocol, dht::key target, std::chrono::milliseconds timeout,
              std::shared_ptr<cancellation_latch> cancellation) -> boost::asio::awaitable<bool> {
          const auto self = weak.lock();
          if (!self) {
             co_return false;
          }
          co_return co_await self->async_refresh_dht_routing(std::move(protocol), std::move(target), timeout,
                                                              std::move(cancellation));
       });
   const auto self = shared_from_this();
   const auto service = routing_refresh;
   if (!launch_tracked([self, service]() -> boost::asio::awaitable<void> {
          static_cast<void>(self);
          co_await service->async_run();
       })) {
      routing_refresh.reset();
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P lifecycle rejected DHT routing refresh service");
   }
}

void node::impl::notify_dht_routing_refresh() noexcept {
   if (routing_refresh) {
      routing_refresh->notify_verified_server();
   }
}

} // namespace forge::net::p2p
