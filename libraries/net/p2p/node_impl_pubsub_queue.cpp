module;

#include <forge/exceptions/macros.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <set>
#include <stop_token>
#include <span>
#include <string>
#include <string_view>
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
import forge.net.p2p.envelope;
import forge.net.p2p.exceptions;
import forge.net.p2p.peer_store;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/certified_peer_record.hxx"
#include "details/node_impl.hxx"
#include "details/pubsub_peer_score.hxx"

namespace forge::net::p2p {

std::vector<pubsub::peer_info> node::impl::prepare_pubsub_prune_peers() {
   auto records = std::vector<pubsub::peer_info>{};
   if (!options.limits.pubsub.peer_exchange) { return records; }
   auto eligible = std::vector<peer_id>{};
   {
      const auto lock = std::scoped_lock{mutex};
      for (const auto& [peer, state] : pubsub_value.peers) {
         if (state.connected && pubsub_score_locked(peer) >= 0) { eligible.push_back(peer); }
      }
   }
   // Certification and persistent store reads happen once, outside the node mutex.
   // One extra candidate permits excluding the recipient without another selection.
   for (const auto& peer : eligible) {
      if (records.size() > options.limits.pubsub.limits.max_peers_per_topic) { break; }
      const auto stored = store.find(peer);
      if (!stored || stored->signed_peer_record.empty()) { continue; }
      try {
         static_cast<void>(detail::certified_peer_record::open(signed_envelope::decode(stored->signed_peer_record), peer));
         records.push_back(pubsub::peer_info{.peer = peer, .signed_peer_record = stored->signed_peer_record});
      } catch (const forge::exceptions::base&) {
         // Hydration is not certification of a forwarded PX envelope.
      }
   }
   return records;
}

detail::pubsub_control_queue::command node::impl::make_pubsub_control_locked(
    const peer_id& peer, const pubsub::topic& subject, detail::pubsub_control_queue::kind operation,
    std::chrono::seconds backoff, std::span<const pubsub::peer_info> px) {
   auto out = detail::pubsub_control_queue::command{.peer = peer, .operation = operation,
       .args = pubsub::control::prune{.subject = subject, .backoff = backoff}};
   const auto row = pubsub_value.peers.find(peer);
   if (row != pubsub_value.peers.end() && row->second.connected) { out.generation = row->second.generation; }
   if (operation == detail::pubsub_control_queue::kind::prune && pubsub_score_locked(peer) >= 0) {
      for (const auto& candidate : px) {
         if (out.args.peers.size() >= options.limits.pubsub.limits.max_peers_per_topic) { break; }
         if (candidate.peer != peer) { out.args.peers.push_back(candidate); }
      }
   }
   return out;
}

std::optional<node::impl::pubsub_control_change> node::impl::prepare_pubsub_controls_locked(
    std::vector<detail::pubsub_control_queue::command> commands, std::chrono::steady_clock::time_point now) {
   auto out = pubsub_control_change{};
   auto backoffs = std::vector<detail::pubsub_backoff::local_request>{};
   out.actions.reserve(commands.size());
   backoffs.reserve(commands.size());
   for (const auto& command : commands) {
      const auto& topic = command.args.subject.value;
      out.actions.push_back(pubsub_control_change::action{command.peer, topic, command.operation});
      if (command.operation == detail::pubsub_control_queue::kind::graft) {
         const auto mesh = pubsub_value.mesh.find(topic);
         if (mesh == pubsub_value.mesh.end() || !mesh->second.contains(command.peer)) {
            out.mesh[topic].insert(command.peer);
         }
      } else {
         backoffs.push_back(detail::pubsub_backoff::local_request{topic, command.peer, command.args.backoff});
      }
   }
   out.backoff = pubsub_value.backoffs.prepare_local(backoffs, now,
       detail::pubsub_backoff::limit_for(options.limits.pubsub.limits.max_topics, options.limits.max_sessions));
   // Dead former members still leave the local mesh, but have no transport intent.
   std::erase_if(commands, [](const auto& command) { return command.generation == 0; });
   auto queued = pubsub_value.controls->prepare(std::move(commands));
   if (!queued) { return std::nullopt; }
   out.queued = std::move(*queued);
   return out;
}

void node::impl::commit_pubsub_controls_locked(pubsub_control_change change,
                                              std::chrono::steady_clock::time_point now) noexcept {
   pubsub_value.controls->commit(std::move(change.queued));
   pubsub_value.backoffs.commit_local(std::move(change.backoff));
   while (!change.mesh.empty()) {
      auto topic = change.mesh.extract(change.mesh.begin());
      const auto mesh = pubsub_value.mesh.find(topic.key());
      if (mesh == pubsub_value.mesh.end()) {
         pubsub_value.mesh.insert(std::move(topic));
      } else {
         mesh->second.merge(topic.mapped());
      }
   }
   for (const auto& action : change.actions) {
      if (action.operation == detail::pubsub_control_queue::kind::graft) {
         if (pubsub_value.scoring) {
            static_cast<void>(pubsub_value.scoring->graft(action.peer, std::string_view{action.topic}, now));
         }
      } else {
         prune_pubsub_peer_locked(action.topic, action.peer);
      }
   }
}

bool node::impl::admit_pubsub_control_locked(detail::pubsub_control_queue::command command,
                                            std::chrono::steady_clock::time_point now) {
   try {
      auto commands = std::vector<detail::pubsub_control_queue::command>{};
      commands.push_back(std::move(command));
      auto change = prepare_pubsub_controls_locked(std::move(commands), now);
      if (!change) { return false; }
      commit_pubsub_controls_locked(std::move(*change), now);
      return true;
   } catch (const std::bad_alloc&) {
      return false;
   } catch (const forge::exceptions::base& error) {
      if (!exceptions::is(error, exceptions::code::invalid_options)) { throw; }
      return false; // The immutable control cannot fit its configured wire envelope.
   }
}

bool node::impl::validate_pubsub_control_locked(const detail::pubsub_control_queue::batch& batch,
                                               bool check_intents) noexcept {
   const auto peer = pubsub_value.peers.find(batch.peer);
   const auto live = !stopped && peer != pubsub_value.peers.end() && peer->second.connected &&
       peer->second.generation == batch.generation;
   if (!check_intents) { return live; }
   auto valid = live;
   for (const auto& item : batch.items) {
      const auto mesh = pubsub_value.mesh.find(item->args.subject.value);
      const auto member = mesh != pubsub_value.mesh.end() && mesh->second.contains(item->peer);
      const auto policy = item->operation == detail::pubsub_control_queue::kind::graft
          ? member && pubsub_value.handlers.contains(item->args.subject.value) : !member;
      if (!live || !policy) { pubsub_value.controls->discard(*item); }
      if (!live || !policy || !pubsub_value.controls->current(*item)) { valid = false; }
   }
   return valid;
}

node::impl::pubsub_control_dispatch::pubsub_control_dispatch(
    std::shared_ptr<impl> value, std::shared_ptr<const detail::pubsub_control_queue::batch> batch,
    std::shared_ptr<session_state> source)
    : owner(std::move(value)), lease(std::move(batch)), origin(std::move(source)) {}

node::impl::pubsub_control_dispatch::~pubsub_control_dispatch() noexcept {
   if (request) { owner->finish_pubsub_request(*request, false, origin); }
   const auto lock = std::scoped_lock{owner->mutex};
   owner->pubsub_value.controls->finish(lease, false);
}

void node::impl::flush_pubsub_controls(std::optional<peer_id> only, std::map<peer_id, pubsub::control> ephemeral,
                                      std::shared_ptr<session_state> origin) {
   auto peers = std::set<peer_id>{};
   try {
      const auto lock = std::scoped_lock{mutex};
      if (stopped) { return; }
      if (only) { peers.insert(*only); }
      else {
         for (const auto& peer : pubsub_value.controls->peers()) { peers.insert(peer); }
         for (const auto& [peer, _] : ephemeral) { peers.insert(peer); }
      }
   } catch (const std::bad_alloc&) { return; }
   for (const auto& peer : peers) {
      auto batch = std::shared_ptr<const detail::pubsub_control_queue::batch>{};
      auto transferred = false;
      auto release = [this, &batch, &transferred](void*) noexcept {
         if (transferred) { return; }
         const auto lock = std::scoped_lock{mutex};
         pubsub_value.controls->finish(batch, false);
      };
      auto guard = std::unique_ptr<void, decltype(release)>{this, std::move(release)};
      try {
         auto rpc = pubsub::rpc{};
         auto bytes = std::size_t{};
         if (const auto row = ephemeral.find(peer); row != ephemeral.end()) {
            rpc.control_value = std::move(row->second);
            try {
               const auto charge = pubsub::codec::gossip_wire_size(*rpc.control_value, options.limits.pubsub);
               if (charge) { bytes = *charge; }
               else { rpc.control_value.reset(); }
            } catch (const std::bad_alloc&) {
               rpc.control_value.reset();
               bytes = 0; // Local gossip preparation cannot hide an independent durable lease.
            }
         }
         {
            const auto lock = std::scoped_lock{mutex};
            if (stopped) { return; }
            const auto row = pubsub_value.peers.find(peer);
            if (row == pubsub_value.peers.end() || !row->second.connected) { continue; }
            if (origin && (origin->info.remote_peer != peer || !pubsub_session_live_locked(origin))) {
               rpc.control_value.reset();
               bytes = 0; // Stale remote gossip cannot hide independent current durable intents.
            }
            batch = pubsub_value.controls->acquire(peer, row->second.generation, bytes);
         }
         if (!batch) { continue; }
         if (batch->ephemeral_bytes == 0) { rpc.control_value.reset(); }
         const auto self = shared_from_this();
         auto dispatch = std::make_shared<pubsub_control_dispatch>(self, batch, origin);
         // The captured owner settles the lease even when launch_tracked skips a task on stop,
         // or coroutine-frame preparation fails before the worker's body starts.
         transferred = launch_tracked([self, dispatch, rpc = std::move(rpc)]() mutable {
            return self->run_pubsub_control_dispatch(dispatch, std::move(rpc));
         });
      } catch (const std::bad_alloc&) {
         // Current control remains queued; one-shot gossip is deliberately dropped.
      } catch (const forge::exceptions::base& error) {
         record_pubsub_send_failure(peer, error, batch ? std::optional{batch->generation} : std::nullopt, origin);
      }
   }
}

boost::asio::awaitable<void> node::impl::run_pubsub_control_dispatch(
   std::shared_ptr<pubsub_control_dispatch> dispatch, pubsub::rpc ephemeral) {
   auto send_generation = std::optional<std::uint64_t>{};
   auto failure_origin = std::shared_ptr<session_state>{};
   try {
      const auto& batch = dispatch->lease;
      if (!batch->items.empty()) {
         static_cast<void>(co_await send_pubsub_rpc(batch->peer, detail::pubsub_control_queue::rpc(*batch),
                                                  send_generation, batch));
      }
      if (ephemeral.control_value) {
         failure_origin = dispatch->origin;
         auto cursor = pubsub::codec::gossip_cursor{};
         while (auto chunk = pubsub::codec::next_gossip(*ephemeral.control_value, cursor, options.limits.pubsub)) {
            {
               const auto lock = std::scoped_lock{mutex};
               if (!validate_pubsub_control_locked(*batch, false) ||
                   (dispatch->origin && (dispatch->origin->info.remote_peer != batch->peer ||
                       !pubsub_session_live_locked(dispatch->origin)))) { co_return; }
               // next_gossip normalizes all source IWANT rows into one row per
               // chunk. Borrow those exact IDs; stage samples one before any await.
               const auto& wants = chunk->value.control_value->want;
               if (!wants.empty()) {
                  dispatch->request = stage_pubsub_request_locked(batch->peer, wants.front().message_ids);
               }
            }
            const auto written = co_await send_pubsub_rpc(batch->peer, std::move(chunk->value), send_generation, batch, false,
                                                         dispatch->origin);
            if (dispatch->request) {
               finish_pubsub_request(*dispatch->request, written, dispatch->origin);
               dispatch->request.reset();
            }
            if (!written) { co_return; }
         }
      }
   } catch (const std::bad_alloc&) {
      // No peer penalty for local preparation; only GRAFT/PRUNE survive for retry.
   } catch (const forge::exceptions::base& error) {
      record_pubsub_send_failure(dispatch->lease->peer, error, dispatch->lease->generation, failure_origin);
   }
}

} // namespace forge::net::p2p
