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
             co_await self->pubsub_heartbeat_once();
             deadline = std::chrono::steady_clock::now() + self->options.limits.pubsub.limits.heartbeat_interval;
          }
       })) {
      auto lock = std::scoped_lock{mutex};
      pubsub_value.heartbeat_started = false;
   }
}

boost::asio::awaitable<void> node::impl::pubsub_heartbeat_once() {
   auto grafts = std::map<peer_id, std::vector<pubsub::control::graft>>{};
   auto prunes = std::map<peer_id, std::vector<pubsub::control::prune>>{};
   auto gossip = std::map<peer_id, std::vector<pubsub::control::ihave>>{};
   auto retries = std::map<peer_id, std::vector<std::vector<std::uint8_t>>>{};
   {
      auto lock = std::scoped_lock{mutex};
      if (stopped) {
         co_return;
      }
      const auto now = std::chrono::steady_clock::now();
      const auto backoff_limit = detail::pubsub_backoff::limit_for(options.limits.pubsub.limits.max_topics,
                                                                   options.limits.max_sessions);
      pubsub_value.backoffs.expire(now);
      const auto add_prune = [&](const peer_id& peer, const std::string& topic) {
         pubsub_value.backoffs.record_local(topic, peer, options.limits.pubsub.limits.prune_backoff, now,
                                             backoff_limit);
         prunes[peer].push_back(pubsub::control::prune{
             .subject = pubsub::topic{.value = topic},
             .backoff = options.limits.pubsub.limits.prune_backoff,
         });
      };
      const auto mesh_high =
          std::min(options.limits.pubsub.limits.mesh_n_high, options.limits.pubsub.limits.max_peers_per_topic);
      const auto mesh_target = std::min(options.limits.pubsub.limits.mesh_n, mesh_high);
      for (const auto& [topic_value, _] : pubsub_value.handlers) {
         auto& mesh = pubsub_value.mesh[topic_value];
         for (auto it = mesh.begin(); it != mesh.end();) {
            const auto has_session = std::ranges::any_of(sessions, [&](const auto& item) {
               return item.second->info.remote_peer == *it && !item.second->closed;
            });
            const auto topics = pubsub_value.peer_topics.find(*it);
            const auto subscribed = topics != pubsub_value.peer_topics.end() && topics->second.contains(topic_value);
            if (!has_session || !subscribed) {
               if (has_session && !subscribed) {
                  add_prune(*it, topic_value);
               }
               it = mesh.erase(it);
            } else {
               ++it;
            }
         }
         for (const auto& [peer, topics] : pubsub_value.peer_topics) {
            if (mesh.size() >= mesh_target) {
               break;
            }
            const auto has_session = std::ranges::any_of(sessions, [&](const auto& item) {
               return item.second->info.remote_peer == peer && !item.second->closed;
            });
            if (has_session && topics.contains(topic_value) && !mesh.contains(peer) &&
                !pubsub_value.backoffs.blocked(topic_value, peer, now)) {
               mesh.insert(peer);
               grafts[peer].push_back(pubsub::control::graft{.subject = pubsub::topic{.value = topic_value}});
            }
         }
         while (mesh.size() > mesh_high) {
            auto it = std::prev(mesh.end());
            const auto peer = *it;
            mesh.erase(it);
            add_prune(peer, topic_value);
         }
         auto ids = std::vector<std::vector<std::uint8_t>>{};
         auto seen = std::size_t{};
         for (auto it = pubsub_value.history.rbegin();
              it != pubsub_value.history.rend() && seen < options.limits.pubsub.limits.history_gossip; ++it, ++seen) {
            if (const auto found = pubsub_value.cache.find(*it); found != pubsub_value.cache.end() &&
                                                                 found->second.subject.value == topic_value &&
                                                                 can_serve_pubsub_message_locked(*it)) {
               ids.push_back(pubsub::codec::message_id(found->second));
            }
         }
         if (!ids.empty()) {
            if (ids.size() > options.limits.pubsub.limits.gossip_lazy) {
               ids.resize(options.limits.pubsub.limits.gossip_lazy);
            }
            for (const auto& peer : mesh) {
               gossip[peer].push_back(pubsub::control::ihave{
                   .subject = pubsub::topic{.value = topic_value},
                   .message_ids = ids,
               });
            }
         }
      }
      auto retry_count = std::size_t{};
      auto retry = pubsub_value.retry_cursor.empty() ? pubsub_value.validations.begin()
                                                     : pubsub_value.validations.upper_bound(pubsub_value.retry_cursor);
      const auto retry_budget =
          std::min(options.limits.pubsub.limits.gossip_lazy, options.limits.pubsub.limits.max_messages);
      for (auto inspected = std::size_t{}; inspected < pubsub_value.validations.size() && retry_count < retry_budget;
           ++inspected) {
         if (retry == pubsub_value.validations.end()) {
            retry = pubsub_value.validations.begin();
         }
         auto& [key, validation] = *retry;
         pubsub_value.retry_cursor = key;
         ++retry;
         if (validation.state != pubsub_state::validation::status::retryable || validation.source.value.empty() ||
             now < validation.retry_after || now < validation.request_after) {
            continue;
         }
         const auto cached = pubsub_value.cache.find(key);
         if (cached == pubsub_value.cache.end()) {
            continue;
         }
         const auto source = validation.source;
         const auto existing = retries.find(source);
         if (existing != retries.end() && existing->second.size() >= options.limits.pubsub.limits.max_message_ids) {
            continue;
         }
         if (!should_request_pubsub_message_locked(key, source, now)) {
            continue;
         }
         retries[source].push_back(pubsub::codec::message_id(cached->second));
         ++retry_count;
      }
   }
   for (const auto& [peer, items] : grafts) {
      try {
         co_await send_pubsub_rpc(peer, pubsub::rpc{.control_value = pubsub::control{.grafts = items}});
      } catch (const forge::exceptions::base& error) {
         record_pubsub_send_failure(peer, error);
      }
   }
   for (const auto& [peer, items] : prunes) {
      try {
         co_await send_pubsub_rpc(peer, pubsub::rpc{.control_value = pubsub::control{.prunes = items}});
      } catch (const forge::exceptions::base& error) {
         record_pubsub_send_failure(peer, error);
      }
   }
   for (const auto& [peer, items] : gossip) {
      try {
         co_await send_pubsub_rpc(peer, pubsub::rpc{.control_value = pubsub::control{.have = items}});
      } catch (const forge::exceptions::base& error) {
         record_pubsub_send_failure(peer, error);
      }
   }
   for (const auto& [peer, message_ids] : retries) {
      try {
         co_await send_pubsub_rpc(peer, pubsub::rpc{.control_value = pubsub::control{
                                                        .want = std::vector<pubsub::control::iwant>{
                                                            pubsub::control::iwant{.message_ids = message_ids}}}});
      } catch (const forge::exceptions::base& error) {
         record_pubsub_send_failure(peer, error);
      }
   }
}

} // namespace forge::net::p2p
