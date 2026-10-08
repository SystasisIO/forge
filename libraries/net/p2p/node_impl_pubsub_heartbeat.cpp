module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
#include <new>
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
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/lifecycle_wakeup.hxx"
#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"
#include "details/pubsub_peer_score.hxx"
#include "details/pubsub_router.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

[[nodiscard]] exceptions::code p2p_code(const forge::exceptions::base& error);
[[nodiscard]] bool is_orderly_stream_close(const forge::exceptions::base& error) noexcept;

boost::asio::awaitable<std::vector<std::uint8_t>> async_read_length_delimited(forge::net::p2p::stream& stream,
                                                                              std::vector<std::uint8_t>& buffer,
                                                                              std::size_t max_payload_size);

void node::impl::launch_pubsub_heartbeat() {
   if (!options.capabilities.has(capabilities::pubsub)) {
      return;
   }
   {
      auto lock = std::scoped_lock{mutex};
      if (pubsub_value.heartbeat_started) {
         return;
      }
      pubsub_value.heartbeat_started = true;
   }
   auto self = shared_from_this();
   if (!launch_tracked([self]() -> asio::awaitable<void> {
          const auto wakeup = self->lifecycle_wakeup;
          auto observed = wakeup->epoch();
          auto deadline = std::chrono::steady_clock::now() + self->options.limits.pubsub.limits.heartbeat_initial_delay;
          while (true) {
             // Admission also notifies this wakeup; only the deadline schedules work.
             while (!self->lifecycle.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                observed = co_await wakeup->async_wait_until(observed, deadline);
             }
             if (self->lifecycle.stop_requested()) {
                co_return;
             }
             {
                auto lock = std::scoped_lock{self->mutex};
                if (self->stopped) {
                   co_return;
                }
             }
             try {
                co_await self->pubsub_heartbeat_once();
             } catch (const std::bad_alloc&) {
                // A transient local preparation refusal skips one tick, not the sole maintenance owner.
             }
             deadline = std::chrono::steady_clock::now() + self->options.limits.pubsub.limits.heartbeat_interval;
          }
       })) {
      auto lock = std::scoped_lock{mutex};
      pubsub_value.heartbeat_started = false;
   }
}

boost::asio::awaitable<void> node::impl::pubsub_heartbeat_once() {
   sample_pubsub_application_scores();
   auto px = std::vector<pubsub::peer_info>{};
   try { px = prepare_pubsub_prune_peers(); } catch (const std::bad_alloc&) { co_return; }
   auto gossip = std::map<peer_id, std::vector<pubsub::control::ihave>>{};
   auto retries = std::map<peer_id, std::vector<std::vector<std::uint8_t>>>{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (stopped) {
         co_return;
      }
      const auto now = std::chrono::steady_clock::now();
      const auto& limits = options.limits.pubsub.limits;
      if (pubsub_value.scoring) {
         pubsub_value.scoring->tick(now);
      }
      for (const auto& [peer, count] : pubsub_value.router->expire(now)) {
         if (pubsub_value.scoring) {
            static_cast<void>(pubsub_value.scoring->add_behaviour_penalty(peer, static_cast<double>(count), now));
         }
      }
      auto scores = std::map<peer_id, double>{};
      for (auto row = pubsub_value.peers.begin(); row != pubsub_value.peers.end();) {
         if (!row->second.connected && row->second.retain_until <= now) {
            pubsub_value.scores.erase(row->first);
            row = pubsub_value.peers.erase(row);
            continue;
         }
         row->second.have = 0;
         row->second.requested = 0;
         scores.emplace(row->first, pubsub_score_locked(row->first));
         ++row;
      }
      const auto score = [&](const peer_id& peer) {
         const auto it = scores.find(peer);
         return it == scores.end() ? -std::numeric_limits<double>::max() : it->second;
      };
      auto outbound = std::set<peer_id>{};
      for (const auto& [_, session] : sessions) {
         if (!session->closed && session->direction == connection_manager::direction::outbound) {
            outbound.insert(session->info.remote_peer);
         }
      }
      const auto graft_slack = detail::pubsub_backoff::graft_slack(limits.heartbeat_interval);
      pubsub_value.backoffs.expire(now, graft_slack);
      const auto high = std::min(limits.mesh_n_high, limits.max_peers_per_topic);
      const auto target = std::min(limits.mesh_n, high);
      const auto gossip_threshold = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.gossip_threshold : 0.0;
      const auto prune = [&](const peer_id& peer, const std::string& topic) {
         try {
            return admit_pubsub_control_locked(make_pubsub_control_locked(peer, pubsub::topic{topic},
                detail::pubsub_control_queue::kind::prune, limits.prune_backoff, px), now);
         } catch (const std::bad_alloc&) { return false; }
      };
      const auto graft = [&](const peer_id& peer, const std::string& topic) {
         try {
            return admit_pubsub_control_locked(make_pubsub_control_locked(peer, pubsub::topic{topic},
                detail::pubsub_control_queue::kind::graft, std::chrono::seconds{}, {}), now);
         } catch (const std::bad_alloc&) { return false; }
      };
      const auto candidates = [&](const std::string& topic, const std::set<peer_id>& excluded, double threshold,
                                  bool mesh_candidate = true) {
         auto peers = std::vector<peer_id>{};
         for (const auto& [peer, topics] : pubsub_value.peer_topics) {
            if (topics.contains(topic) && pubsub_peer_live_locked(peer) && !excluded.contains(peer) &&
                score(peer) >= threshold &&
                (!mesh_candidate || !pubsub_value.backoffs.graft_blocked(topic, peer, now, graft_slack))) {
               peers.push_back(peer);
            }
         }
         pubsub_value.router->shuffle(peers);
         return peers;
      };
      const auto emit_gossip = [&](const std::string& topic, const std::set<peer_id>& excluded) {
         auto ids = std::vector<std::vector<std::uint8_t>>{};
         for (auto it = pubsub_value.history.rbegin(); it != pubsub_value.history.rend(); ++it) {
            const auto validation = pubsub_value.validations.find(*it);
            const auto message = pubsub_value.cache.find(*it);
            if (validation != pubsub_value.validations.end() && message != pubsub_value.cache.end() &&
                pubsub_value.epoch - validation->second.cache_epoch < limits.history_gossip &&
                message->second.subject.value == topic && can_serve_pubsub_message_locked(*it)) {
               ids.push_back(pubsub::codec::message_id(message->second, options.limits.pubsub));
               if (ids.size() >= limits.max_message_ids) {
                  break;
               }
            }
         }
         if (ids.empty()) {
            return;
         }
         auto peers = candidates(topic, excluded, gossip_threshold, false);
         const auto count = std::min(peers.size(), std::max(limits.gossip_lazy,
            static_cast<std::size_t>(std::ceil(limits.gossip_factor * static_cast<double>(peers.size())))));
         for (auto index = std::size_t{}; index < count; ++index) {
            gossip[peers[index]].push_back(pubsub::control::ihave{.subject = pubsub::topic{topic}, .message_ids = ids});
         }
      };
      for (const auto& [topic, _] : pubsub_value.handlers) {
         auto& mesh = pubsub_value.mesh[topic];
         auto departed = std::vector<peer_id>{};
         for (const auto& peer : mesh) {
            const auto topics = pubsub_value.peer_topics.find(peer);
            if (!pubsub_peer_live_locked(peer) || topics == pubsub_value.peer_topics.end() ||
                !topics->second.contains(topic) || score(peer) < 0) {
               departed.push_back(peer);
            }
         }
         for (const auto& peer : departed) {
            prune(peer, topic);
         }
         // Donor: retain a healthy mesh in [Dlo,Dhi), rather than constantly filling to D.
         if (mesh.size() < limits.mesh_n_low) {
            for (const auto& peer : candidates(topic, mesh, 0)) {
               if (mesh.size() >= target) {
                  break;
               }
               graft(peer, topic);
            }
         }
         if (mesh.size() >= high && mesh.size() > target) {
            auto ranked = std::vector<peer_id>{mesh.begin(), mesh.end()};
            pubsub_value.router->shuffle(ranked);
            std::stable_sort(ranked.begin(), ranked.end(), [&](const peer_id& a, const peer_id& b) { return score(a) > score(b); });
            const auto protected_count = std::min(limits.mesh_score_min, target);
            auto keep = std::set<peer_id>{ranked.begin(), ranked.begin() + protected_count};
            auto rest = std::vector<peer_id>{ranked.begin() + protected_count, ranked.end()};
            pubsub_value.router->shuffle(rest);
            auto outbound_kept = static_cast<std::size_t>(std::ranges::count_if(keep,
               [&](const auto& peer) { return outbound.contains(peer); }));
            // Physical outbound quota takes precedence over the soft Dscore selection.
            for (const auto& peer : rest) {
               if (outbound_kept >= limits.mesh_outbound_min) {
                  break;
               }
               if (outbound.contains(peer)) {
                  const auto replace = std::find_if(ranked.rbegin(), ranked.rend(), [&](const auto& candidate) {
                     return keep.contains(candidate) && !outbound.contains(candidate);
                  });
                  if (keep.size() >= target && replace == ranked.rend()) {
                     break;
                  }
                  // Insert before erasing so allocation failure does not lose a survivor.
                  keep.insert(peer);
                  if (keep.size() > target) {
                     keep.erase(*replace);
                  }
                  ++outbound_kept;
               }
            }
            for (const auto& peer : rest) {
               if (keep.size() >= target) {
                  break;
               }
               keep.insert(peer);
            }
            departed.clear();
            for (const auto& peer : mesh) {
               if (!keep.contains(peer)) {
                  departed.push_back(peer);
               }
            }
            for (const auto& peer : departed) {
               prune(peer, topic);
            }
         }
         auto outbound_count = static_cast<std::size_t>(std::ranges::count_if(mesh,
            [&](const auto& peer) { return outbound.contains(peer); }));
         if (mesh.size() >= limits.mesh_n_low && outbound_count < limits.mesh_outbound_min) {
            for (const auto& peer : candidates(topic, mesh, 0)) {
               if (outbound_count >= limits.mesh_outbound_min || mesh.size() >= limits.max_peers_per_topic) {
                  break;
               }
               if (outbound.contains(peer)) {
                  if (graft(peer, topic)) { ++outbound_count; }
               }
            }
         }
         if (options.limits.pubsub.scoring && mesh.size() > 1 &&
             (pubsub_value.epoch + 1) % limits.opportunistic_graft_ticks == 0) {
            auto ranked = std::vector<peer_id>{mesh.begin(), mesh.end()};
            std::sort(ranked.begin(), ranked.end(), [&](const auto& a, const auto& b) { return score(a) < score(b); });
            const auto median = score(ranked[ranked.size() / 2]);
            if (median < options.limits.pubsub.scoring->thresholds.opportunistic_graft_threshold) {
               auto count = std::size_t{};
               for (const auto& peer : candidates(topic, mesh, median)) {
                  if (count >= limits.opportunistic_graft_peers || mesh.size() >= limits.max_peers_per_topic) {
                     break;
                  }
                  if (score(peer) > median) {
                     if (graft(peer, topic)) { ++count; }
                  }
               }
            }
         }
         emit_gossip(topic, mesh);
      }
      for (auto it = pubsub_value.fanout.begin(); it != pubsub_value.fanout.end();) {
         if (now - it->second.last_publish >= limits.fanout_ttl) {
            it = pubsub_value.fanout.erase(it);
            continue;
         }
         std::erase_if(it->second.peers, [&](const auto& peer) {
            const auto topics = pubsub_value.peer_topics.find(peer);
            const auto threshold = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.publish_threshold : 0.0;
            return !pubsub_peer_live_locked(peer) || topics == pubsub_value.peer_topics.end() ||
                   !topics->second.contains(it->first) || score(peer) < threshold;
         });
         const auto threshold = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.publish_threshold : 0.0;
         for (const auto& peer : candidates(it->first, it->second.peers, threshold, false)) {
            if (it->second.peers.size() >= target) {
               break;
            }
            it->second.peers.insert(peer);
         }
         emit_gossip(it->first, it->second.peers);
         ++it;
      }
      auto retry = pubsub_value.retry_cursor.empty() ? pubsub_value.validations.begin()
         : pubsub_value.validations.upper_bound(pubsub_value.retry_cursor);
      auto count = std::size_t{};
      const auto budget = std::min(limits.gossip_lazy, limits.max_messages);
      for (auto inspected = std::size_t{}; inspected < pubsub_value.validations.size() && count < budget; ++inspected) {
         if (retry == pubsub_value.validations.end()) {
            retry = pubsub_value.validations.begin();
         }
         auto& [key, validation] = *retry++;
         pubsub_value.retry_cursor = key;
         if (validation.state != pubsub_state::validation::status::retryable ||
             !pubsub_peer_live_locked(validation.source) || now < validation.retry_after || now < validation.request_after ||
             !should_request_pubsub_message_locked(key, validation.source, now)) {
            continue;
         }
         const auto cached = pubsub_value.cache.find(key);
         if (cached != pubsub_value.cache.end()) {
            retries[validation.source].push_back(pubsub::codec::message_id(cached->second, options.limits.pubsub));
            ++count;
         }
      }
      // Capture gossip before advancing the insertion bin; new publications enter the next bin.
      // Donor enqueue/shift semantics permit one-shot IHAVE to finish after cache expiry.
      ++pubsub_value.epoch;
   }
   auto controls = std::map<peer_id, pubsub::control>{};
   for (auto& [peer, items] : gossip) { controls[peer].have = std::move(items); }
   for (auto& [peer, ids] : retries) { controls[peer].want.push_back(pubsub::control::iwant{.message_ids = std::move(ids)}); }
   flush_pubsub_controls(std::nullopt, std::move(controls));
   {
      const auto lock = std::scoped_lock{mutex};
      prune_pubsub_cache_locked();
   }
}

} // namespace forge::net::p2p
