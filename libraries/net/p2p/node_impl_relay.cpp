module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
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
#include <stop_token>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

module forge.net.p2p.node;

import :lifecycle_stop_listener;

import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.dht;
import forge.net.p2p.discovery;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.hole_punch;
import forge.net.p2p.identify;
import forge.net.p2p.identity;
import forge.net.p2p.lifecycle;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.reachability;
import forge.net.p2p.relay;
import forge.net.p2p.rendezvous;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.p2p.topology;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

void node::impl::record_relay_bytes(std::uint64_t bytes) noexcept {
   auto lock = std::scoped_lock{mutex};
   const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
   metrics_value.relay_bytes =
       bytes > maximum - metrics_value.relay_bytes ? maximum : metrics_value.relay_bytes + bytes;
}

void node::impl::record_path_open(path::kind kind) {
   auto lock = std::scoped_lock{mutex};
   if (kind == path::kind::direct) {
      ++metrics_value.path_direct_opens;
   } else {
      ++metrics_value.path_relay_opens;
   }
}

void node::impl::record_path_attempt(path::kind kind) {
   auto lock = std::scoped_lock{mutex};
   if (kind == path::kind::direct) {
      ++metrics_value.path_direct_attempts;
   } else {
      ++metrics_value.path_relay_attempts;
   }
}

void node::impl::record_hole_punch_result(hole_punch::status status) {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.hole_punch_attempts;
   if (status == hole_punch::status::succeeded) {
      ++metrics_value.hole_punch_successes;
   } else if (status == hole_punch::status::failed) {
      ++metrics_value.hole_punch_failures;
   }
}

void node::impl::record_direct_failure(const peer_id& peer) {
   store.mark_failure(peer);
   increment_direct_failure();
}

void node::impl::increment_direct_failure() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.direct_failures;
}

std::chrono::system_clock::time_point node::impl::endpoint_backoff_until(const peer_id& peer,
                                                                         const forge::net::p2p::endpoint& endpoint,
                                                                         path::kind kind) const {
   return endpoint_backoff_until(peer, endpoint.to_multiaddr(), kind);
}

std::chrono::system_clock::time_point node::impl::endpoint_backoff_until(
    const peer_id& peer, const forge::multiformats::multiaddr& address, path::kind kind) const {
   auto failures = std::uint64_t{1};
   if (auto record = store.find(peer)) {
      const auto endpoint_string = address.to_string();
      for (const auto& current : record->endpoints) {
         if (current.kind == kind && current.address.to_string() == endpoint_string) {
            failures = current.failures + 1;
            break;
         }
      }
   }
   const auto base = options.limits.dial_backoff_base;
   const auto step = options.limits.dial_backoff_step;
   const auto cap = options.limits.dial_backoff_max;
   const auto cap_count = cap.count() > base.count() ? cap.count() - base.count() : 0;
   const auto step_count = step.count();
   const auto max_square =
       cap_count > 0 && step_count > 0 ? static_cast<std::uint64_t>(cap_count / step_count) : std::uint64_t{0};
   const auto square = failures > std::numeric_limits<std::uint64_t>::max() / failures
                           ? std::numeric_limits<std::uint64_t>::max()
                           : failures * failures;
   const auto extra = std::chrono::milliseconds{
       static_cast<std::chrono::milliseconds::rep>(std::min(square, max_square) * step_count)};
   return std::chrono::system_clock::now() + std::min(base + extra, cap);
}

void node::impl::record_relay_failure() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.relay_failures;
}


} // namespace forge::net::p2p
