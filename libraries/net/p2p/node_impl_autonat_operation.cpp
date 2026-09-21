module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/compat/move_only_function.hpp>

module forge.net.p2p.node;

import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.dns.resolver;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.message;
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

#include "details/cancellation_latch.hxx"
#include "details/node_impl.hxx"
#include "details/node_impl_autonat_operation.hxx"
#include "details/worker_stop_bridge.hxx"

namespace forge::net::p2p {

node::impl::autonat_operation::autonat_operation()
    : cancellation{std::make_shared<cancellation_latch>()},
      stop{std::make_shared<detail::worker_stop_bridge>()} {}

node::impl::autonat_operation::~autonat_operation() {
   if (admitted) {
      auto lock = std::scoped_lock{owner->mutex};
      --owner->autonat_handlers_active;
      if (service) {
         owner->autonat_service_active.erase(peer);
      }
   }
}

void node::impl::autonat_operation::cancel() noexcept {
   cancellation->request_stop();
   request.request_cancel();
   dial_back.request_cancel();
   if (dial_back_resource) {
      dial_back_resource->request_cancel();
   }
   attempt.connection.session.request_cancel();
   if (delay) {
      try {
         delay->cancel();
      } catch (...) {
         // Cancellation cannot escape the terminal owner's callback.
      }
   }
}

void node::impl::autonat_operation::check() const {
   if (std::chrono::steady_clock::now() >= deadline) {
      FORGE_THROW_EXCEPTION(exceptions::timeout, "AutoNAT operation timed out");
   }
   if (stop->stop_requested() || cancellation->stop_requested()) {
      FORGE_THROW_EXCEPTION(exceptions::canceled, "AutoNAT operation canceled");
   }
}

} // namespace forge::net::p2p
