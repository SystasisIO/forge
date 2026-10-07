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
#include <functional>
#include <iterator>
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
#include <boost/asio/ip/address.hpp>
#include <random>
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
import forge.net.p2p.discovery;
import forge.net.p2p.envelope;
import forge.net.p2p.rendezvous;
import forge.net.p2p.peer_store;
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/certified_peer_record.hxx"
#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"

#include "details/pubsub_peer_score.hxx"
#include "details/pubsub_router.hxx"

namespace forge::net::p2p {

bool node::impl::record_pubsub_subscription_locked(const peer_id& peer, const std::string& topic) {
   if (!pubsub_peer_live_locked(peer)) {
      return false;
   }
   const auto& limits = options.limits.pubsub.limits;
   const auto existing = pubsub_value.peer_topics.find(peer);
   if (existing != pubsub_value.peer_topics.end() && existing->second.contains(topic)) {
      return true;
   }
   if (pubsub_value.remote_topic_entries >= limits.max_remote_topic_entries ||
       (existing != pubsub_value.peer_topics.end() && existing->second.size() >= limits.max_remote_topics_per_peer)) {
      return false;
   }
   if (static_cast<std::size_t>(std::ranges::count_if(pubsub_value.peer_topics,
          [&](const auto& row) { return row.second.contains(topic); })) >= limits.max_peers_per_topic) {
      return false;
   }
   const auto [row, inserted] = pubsub_value.peer_topics.try_emplace(peer);
   try {
      row->second.insert(topic);
   } catch (...) {
      if (inserted) {
         pubsub_value.peer_topics.erase(row);
      }
      throw;
   }
   ++pubsub_value.remote_topic_entries;
   return true;
}

boost::asio::awaitable<void> node::impl::handle_pubsub_control(
    std::shared_ptr<session_state> session, const pubsub::control& value, const protocol_id& protocol) {
   {
      const auto lock = std::scoped_lock{mutex};
      if (!pubsub_session_live_locked(session)) { co_return; }
      ++metrics_value.pubsub_control_messages;
   }
   const auto& peer = session->info.remote_peer;
   auto missing = std::vector<std::vector<std::uint8_t>>{};
   auto cached = std::vector<pubsub::message>{};
   auto rejected = false;
   auto prune_peers = std::vector<pubsub::peer_info>{};
   if (!value.grafts.empty()) {
      try { prune_peers = prepare_pubsub_prune_peers(); } catch (const std::bad_alloc&) { co_return; }
   }
   auto px = std::vector<pubsub::peer_info>{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (!pubsub_session_live_locked(session)) { co_return; }
      const auto row = pubsub_value.peers.find(peer);
      if (row == pubsub_value.peers.end() || !row->second.connected) {
         co_return;
      }
      const auto now = std::chrono::steady_clock::now();
      const auto& limits = options.limits.pubsub.limits;
      const auto score = pubsub_score_locked(peer);
      const auto gossip_threshold = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.gossip_threshold : 0.0;
      const auto px_threshold = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.accept_px_threshold : 0.0;
      const auto backoff_limit = detail::pubsub_backoff::limit_for(limits.max_topics, options.limits.max_sessions);
      pubsub_value.backoffs.expire(now);
      const auto reject = [&](const pubsub::topic& subject) {
         try {
            const auto committed = admit_pubsub_control_locked(make_pubsub_control_locked(peer, subject,
                detail::pubsub_control_queue::kind::prune, limits.prune_backoff, prune_peers), now);
            rejected = rejected || committed;
            return committed;
         } catch (const std::bad_alloc&) { return false; }
      };
      for (const auto& graft : value.grafts) {
         if (!pubsub_value.handlers.contains(graft.subject.value)) {
            continue;
         }
         const auto local = pubsub_value.backoffs.local_status(graft.subject.value, peer, now);
         const auto remote = pubsub_value.backoffs.remote_status(graft.subject.value, peer, now);
         if (local != detail::pubsub_backoff::status::none || remote != detail::pubsub_backoff::status::none) {
            if (reject(graft.subject) &&
                (local == detail::pubsub_backoff::status::exact || remote == detail::pubsub_backoff::status::exact) &&
                pubsub_value.scoring) {
               static_cast<void>(pubsub_value.scoring->add_behaviour_penalty(peer, 1.0, now));
            }
            continue;
         }
         auto& mesh = pubsub_value.mesh[graft.subject.value];
         if (score < 0 || (!mesh.contains(peer) && mesh.size() >= limits.max_peers_per_topic) ||
             (!mesh.contains(peer) && mesh.size() >= limits.mesh_n_high && !pubsub_peer_outbound_locked(peer)) ||
             !record_pubsub_subscription_locked(peer, graft.subject.value)) {
            reject(graft.subject);
            continue;
         }
         graft_pubsub_peer_locked(graft.subject.value, peer);
         pubsub_value.controls->discard(peer, graft.subject.value, detail::pubsub_control_queue::kind::prune);
      }
      for (const auto& prune : value.prunes) {
         const auto duration = protocol == builtins::meshsub_v10 ? limits.prune_backoff
             : detail::pubsub_backoff::remote_duration(prune.backoff, limits.prune_backoff);
         pubsub_value.backoffs.record_remote(prune.subject.value, peer, duration, now, backoff_limit);
         prune_pubsub_peer_locked(prune.subject.value, peer);
         pubsub_value.controls->discard(peer, prune.subject.value, detail::pubsub_control_queue::kind::graft);
         if (protocol != builtins::meshsub_v10 && options.limits.pubsub.peer_exchange && score >= px_threshold) {
            for (const auto& candidate : prune.peers) {
               if (px.size() >= limits.max_peers_per_topic) {
                  break;
               }
               px.push_back(candidate);
            }
         }
      }
      if (score >= gossip_threshold) {
         for (const auto& have : value.have) {
            if (++row->second.have > limits.max_ihave_per_peer) {
               row->second.have = limits.max_ihave_per_peer;
               break;
            }
            if (!pubsub_value.handlers.contains(have.subject.value)) {
               continue;
            }
            for (const auto& id : have.message_ids) {
               if (row->second.requested + missing.size() >= limits.max_message_ids) {
                  break;
               }
               const auto key = bytes_key(id);
               const auto stored = pubsub_value.cache.find(key);
               if (stored != pubsub_value.cache.end() && stored->second.subject != have.subject) {
                  continue;
               }
               if (should_request_pubsub_message_locked(key, peer, now) && std::ranges::find(missing, id) == missing.end()) {
                  missing.push_back(id);
               }
            }
         }
         row->second.requested += missing.size();
         auto wants = std::size_t{};
         for (const auto& want : value.want) {
            if (++wants > limits.max_iwant_per_peer) {
               break;
            }
            for (const auto& id : want.message_ids) {
               if (cached.size() >= limits.max_messages) {
                  break;
               }
               const auto key = bytes_key(id);
               const auto stored = pubsub_value.cache.find(key);
               const auto validation = pubsub_value.validations.find(key);
               if (stored == pubsub_value.cache.end() || validation == pubsub_value.validations.end() ||
                   !can_serve_pubsub_message_locked(key)) {
                  continue;
               }
               const auto [delivery, inserted] = validation->second.retransmissions.try_emplace(peer, 0);
               if (inserted && validation->second.retransmissions.size() > options.limits.max_sessions) {
                  validation->second.retransmissions.erase(delivery);
                  continue;
               }
               if (delivery->second < limits.gossip_retransmission) {
                  cached.push_back(stored->second);
                  ++delivery->second;
               }
            }
         }
      }
   }
   // Signed PX verification and persistent store work never run under the node mutex.
   for (const auto& candidate : px) {
      if (candidate.peer == local || candidate.signed_peer_record.empty()) {
         continue;
      }
      try {
         const auto record = detail::certified_peer_record::open(signed_envelope::decode(candidate.signed_peer_record), candidate.peer);
         if (const auto previous = store.find(candidate.peer); previous && !previous->signed_peer_record.empty()) {
            const auto known = detail::certified_peer_record::open(signed_envelope::decode(previous->signed_peer_record), candidate.peer);
            if (known.sequence > record.sequence) {
               continue;
            }
         }
         auto update = peer_store::identify_update{};
         update.signed_peer_record = candidate.signed_peer_record;
         update.signed_endpoints = host_addresses::sanitize_discovered_addresses(record.endpoints, candidate.peer,
            host_addresses::learning_context{.source = host_addresses::source_kind::third_party});
         static_cast<void>(store.apply_identify(candidate.peer, std::move(update)));
      } catch (const forge::exceptions::base&) {
         // A PX record is third-party input, not a P4 message or authenticated direct address receipt.
      }
   }
   if (rejected || !missing.empty()) {
      auto ephemeral = std::map<peer_id, pubsub::control>{};
      if (!missing.empty()) {
         auto control = pubsub::control{};
         control.want.push_back(pubsub::control::iwant{.message_ids = std::move(missing)});
         ephemeral.emplace(peer, std::move(control));
      }
      flush_pubsub_controls(peer, std::move(ephemeral), session);
   }
   for (auto& message : cached) {
      auto response = pubsub::rpc{};
      response.messages.push_back(std::move(message));
      auto send_generation = std::optional<std::uint64_t>{};
      try {
         if (!co_await send_pubsub_rpc(peer, std::move(response), send_generation, {}, true, session)) { co_return; }
      } catch (const forge::exceptions::base& error) {
         record_pubsub_send_failure(peer, error, send_generation, session);
         co_return;
      }
   }
}

} // namespace forge::net::p2p
