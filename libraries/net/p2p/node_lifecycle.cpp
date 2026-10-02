module;

#include <forge/exceptions/macros.hpp>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/cancellation_type.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

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
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.exceptions;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.identify;
import forge.net.p2p.lifecycle;
import forge.net.p2p.negotiation;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.p2p.relay;
import forge.net.p2p.resource_manager;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/bootstrap_service.hxx"
#include "details/node_impl.hxx"

namespace forge::net::p2p {

lifecycle_status node::lifecycle_state() const {
   const auto configured = impl_->bootstrap->configured_count();
   const auto connected = impl_->bootstrap->connected_count();
   return lifecycle_status{
       .phase = impl_->lifecycle.phase(),
       .requirement = impl_->options.lifecycle.requirement,
       .configured_bootstrap = configured,
       .connected_bootstrap = connected,
       .degraded = configured != 0 && connected == 0,
       .last_bootstrap_failure = impl_->bootstrap->last_failure(),
   };
}

boost::asio::awaitable<lifecycle_status> node::async_start() {
   auto self = impl_;
   if (!self->lifecycle.begin_start()) {
      if (self->lifecycle.phase() == lifecycle_phase::stopping || self->lifecycle.phase() == lifecycle_phase::stopped) {
         FORGE_THROW_EXCEPTION(exceptions::closed, "cannot start P2P node after shutdown");
      }
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P node lifecycle has already started");
   }

   auto operation = self->lifecycle.track();
   if (!operation.active()) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "cannot start P2P node after shutdown");
   }
   const auto executor = operation.executor();
   auto failure = std::exception_ptr{};
   auto result = lifecycle_status{};
   try {
      result = co_await boost::asio::co_spawn(executor, self->async_start_lifecycle(), boost::asio::use_awaitable);
   } catch (...) {
      failure = std::current_exception();
   }
   operation.release();

   if (!failure) {
      co_return result;
   }
   const auto phase = self->lifecycle.phase();
   if (phase != lifecycle_phase::stopping && phase != lifecycle_phase::stopped) {
      try {
         co_await async_stop();
      } catch (...) {
         forge::exceptions::capture_and_log("P2P lifecycle rollback failed");
      }
   }
   std::rethrow_exception(failure);
}

boost::asio::awaitable<void> node::async_set_bootstrap(std::vector<bootstrap_peer> peers) {
   auto self = impl_;
   validate_bootstrap(peers, false, self->options.dns_resolution, self->private_network_enabled());
   co_await self->bootstrap->async_set_bootstrap(std::move(peers));
}

} // namespace forge::net::p2p
