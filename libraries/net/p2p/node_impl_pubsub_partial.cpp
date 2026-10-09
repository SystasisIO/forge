module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <random>
#include <ranges>
#include <set>
#include <span>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>

module forge.net.p2p.node;
import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.peer_store;
import forge.net.p2p.protocol;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/pubsub_router.hxx"

namespace forge::net::p2p {

bool node::impl::partial_peer_supported_locked(const peer_id& peer, const pubsub::topic& subject,
                                               bool data, std::uint64_t session_id) const {
   if (!options.limits.pubsub.partial_messages || stopped || !pubsub_peer_live_locked(peer)) { return false; }
   const auto inbound = pubsub_value.inbound.find(peer);
   if (inbound == pubsub_value.inbound.end() || inbound->second.empty()) { return false; }
   const auto& generation = inbound->second.rbegin()->second;
   const auto session = sessions.find(generation.session_id);
   if (generation.protocol != builtins::meshsub_v13 || generation.first_rpc || !generation.partial_messages ||
       (session_id != 0 && generation.session_id != session_id) || session == sessions.end() ||
       !pubsub_session_live_locked(session->second)) { return false; }
   const auto topic = generation.partial_topics.find(subject.value);
   if (topic == generation.partial_topics.end()) { return false; }
   const auto requests = topic->second.requests_partial.value_or(false);
   // The protobuf contract and Go imply support from requests, even with explicit false; retain raw flags.
   const auto supports = requests || topic->second.supports_sending_partial.value_or(false);
   return supports && (!data || requests);
}

bool node::impl::prefer_pubsub_partial_locked(const peer_id& peer, const pubsub::topic& subject,
                                             std::uint64_t session_id) const {
   return pubsub_value.partial.find(subject) && partial_peer_supported_locked(peer, subject, true, session_id);
}

std::vector<peer_id> node::impl::partial_peers_locked(const pubsub::partial_topic& registration, bool off_mesh,
                                                    std::size_t* byte_charge) {
   static_cast<void>(pubsub_value.partial.require(registration));
   auto peers = std::vector<peer_id>{};
   if (byte_charge) {
      if (pubsub_value.inbound.size() > ((std::numeric_limits<std::size_t>::max)() - *byte_charge) / sizeof(peer_id)) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "Partial peer snapshot byte size overflow");
      }
      *byte_charge += pubsub_value.inbound.size() * sizeof(peer_id);
   } else { peers.reserve(pubsub_value.inbound.size()); }
   const auto mesh = pubsub_value.mesh.find(registration.subject().value);
   const auto threshold = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.gossip_threshold : 0.0;
   for (const auto& [peer, _] : pubsub_value.inbound) {
      if ((off_mesh && mesh != pubsub_value.mesh.end() && mesh->second.contains(peer)) ||
          pubsub_score_locked(peer) < threshold || !partial_peer_supported_locked(peer, registration.subject(), false)) { continue; }
      if (byte_charge) {
         const auto remaining = (std::numeric_limits<std::size_t>::max)() - *byte_charge;
         if (peer.value.size() >= remaining) {
            FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "Partial peer snapshot byte size overflow");
         }
         *byte_charge += peer.value.size() + 1;
      } else { peers.push_back(peer); }
   }
   if (byte_charge) { return peers; }
   pubsub_value.router->shuffle(peers);
   const auto& limits = options.limits.pubsub.limits;
   auto count = std::min(peers.size(), limits.max_partial_gossip_peers);
   if (off_mesh) {
      count = std::min(count, std::max(limits.gossip_lazy,
          static_cast<std::size_t>(std::ceil(limits.gossip_factor * static_cast<double>(peers.size())))));
   }
   peers.resize(count);
   return peers;
}

void node::impl::receive_pubsub_partial(const std::shared_ptr<session_state>& session, const protocol_id& protocol,
                                       std::uint64_t generation, const pubsub::partial_message& value) {
   if (!options.limits.pubsub.partial_messages || protocol != builtins::meshsub_v13 || !value.subject) { return; }
   try {
      auto registration = std::shared_ptr<detail::pubsub_partial::registration>{};
      auto lease = std::shared_ptr<detail::pubsub_partial::callback>{};
      {
         const auto lock = std::scoped_lock{mutex};
         if (!pubsub_session_live_locked(session)) { return; }
         const auto inbound = pubsub_value.inbound.find(session->info.remote_peer);
         if (inbound == pubsub_value.inbound.end() || inbound->second.empty() ||
             inbound->second.rbegin()->first != generation) { return; }
         const auto& remote = inbound->second.rbegin()->second;
         // Receive authority is the remote first RPC on this stream, not our outbound advertisement.
         if (remote.session_id != session->id || remote.protocol != builtins::meshsub_v13 ||
             remote.first_rpc || !remote.partial_messages) { return; }
         registration = pubsub_value.partial.find(*value.subject);
         if (!registration || !registration->options.receive ||
             (value.data && !registration->options.requests_partial)) { return; }
         const auto bytes = sizeof(pubsub::partial_event) + 2 * value.subject->value.size() + session->info.remote_peer.value.size() +
             (value.group_id ? value.group_id->size() : 0) +
             (value.data ? value.data->size() : 0) + (value.metadata ? value.metadata->size() : 0);
         lease = pubsub_value.partial.admit(registration, bytes, false, options.limits.pubsub.limits);
      }
      if (!lease) { return; }
      auto event = pubsub::partial_event{.registration = registration->token, .source = session->info.remote_peer, .value = value};
      const auto self = shared_from_this();
      static_cast<void>(launch_tracked([self, registration, lease, event = std::move(event)]() mutable -> boost::asio::awaitable<void> {
         if (!self->pubsub_value.partial.current(registration->token) || registration->stop.stop_requested()) { co_return; }
         try { co_await registration->options.receive(std::move(event), registration->stop.get_token()); }
         catch (...) { self->pubsub_value.partial.failed(); } // Local callback failure is not a peer protocol fault.
      }));
   } catch (const std::bad_alloc&) { pubsub_value.partial.failed(); }
}

void node::impl::gossip_pubsub_partial() {
   if (!options.limits.pubsub.partial_messages) { return; }
   try {
      for (const auto& registration : pubsub_value.partial.registrations()) {
         try {
            auto lease = std::shared_ptr<detail::pubsub_partial::callback>{};
            auto event = pubsub::partial_gossip_event{};
            {
               const auto lock = std::scoped_lock{mutex};
               if (stopped || !pubsub_value.partial.current(registration->token)) { continue; }
               auto bytes = sizeof(event) + registration->token.subject().value.size();
               static_cast<void>(partial_peers_locked(registration->token, true, &bytes));
               lease = pubsub_value.partial.admit(registration, bytes, true, options.limits.pubsub.limits);
               if (!lease) { continue; }
               event.registration = registration->token;
               event.groups = lease->take_groups();
               event.peers = partial_peers_locked(registration->token, true);
               if (event.peers.empty()) { continue; }
            }
            const auto self = shared_from_this();
            static_cast<void>(launch_tracked([self, registration, lease, event = std::move(event)]() mutable -> boost::asio::awaitable<void> {
               if (!self->pubsub_value.partial.current(registration->token) || registration->stop.stop_requested()) { co_return; }
               try { co_await registration->options.gossip(std::move(event), registration->stop.get_token()); }
               catch (...) { self->pubsub_value.partial.failed(); }
            }));
         } catch (const forge::exceptions::base& error) {
            if (!exceptions::is(error, exceptions::code::closed)) { throw; }
         }
      }
   } catch (const std::bad_alloc&) { pubsub_value.partial.failed(); }
}

} // namespace forge::net::p2p
