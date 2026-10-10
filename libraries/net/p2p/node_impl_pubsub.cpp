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
#include <iterator>
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
import forge.net.p2p.endpoint;
import forge.multiformats.multiaddr;
import forge.net.p2p.exceptions;
import forge.net.p2p.negotiation;
import forge.net.p2p.pubsub;
import forge.net.p2p.resource_manager;
import forge.net.p2p.stream;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/peer_failure.hxx"
#include "details/session_lifecycle.hxx"
#include "details/pubsub_peer_score.hxx"
#include "details/pubsub_router.hxx"

namespace forge::net::p2p {

namespace asio = boost::asio;

[[nodiscard]] exceptions::code p2p_code(const forge::exceptions::base& error);
[[nodiscard]] bool is_orderly_stream_close(const forge::exceptions::base& error) noexcept;

boost::asio::awaitable<std::vector<std::uint8_t>> async_read_length_delimited(forge::net::p2p::stream& stream,
                                                                              std::vector<std::uint8_t>& buffer,
                                                                              std::size_t max_payload_size);

void node::impl::invalidate_pubsub_outbound_locked(const peer_id& peer, std::optional<std::uint64_t> owner_session_id,
                                                   const std::shared_ptr<forge::asio::gate>& owner_write_gate,
                                                   const std::shared_ptr<forge::net::p2p::stream>& owner_stream) noexcept {
   const auto found = pubsub_value.outbound.find(peer);
   if (found == pubsub_value.outbound.end() || (owner_session_id && found->second.session_id != *owner_session_id) ||
       (owner_write_gate && found->second.write_gate != owner_write_gate) ||
       (owner_stream && found->second.stream != owner_stream)) {
      return;
   }
   found->second.write_gate->close();
   pubsub_value.outbound.erase(found);
   for (const auto& [topic, _] : pubsub_value.mesh) {
      prune_pubsub_peer_locked(topic, peer);
   }
}

void node::impl::forget_pubsub_peer_locked(const peer_id& peer) {
   disconnect_pubsub_peer_locked(peer, std::chrono::steady_clock::now());
   pubsub_value.inbound.erase(peer);
   if (const auto topics = pubsub_value.peer_topics.find(peer); topics != pubsub_value.peer_topics.end()) {
      pubsub_value.remote_topic_entries -= topics->second.size();
      pubsub_value.peer_topics.erase(topics);
   }
   for (const auto& [topic, _] : pubsub_value.mesh) {
      prune_pubsub_peer_locked(topic, peer);
   }
   for (auto& [_, fanout] : pubsub_value.fanout) {
      fanout.peers.erase(peer);
   }
   if (pubsub_value.router) {
      pubsub_value.router->forget(peer);
   }
}

void node::impl::disconnect_pubsub_peer_locked(const peer_id& peer, std::chrono::steady_clock::time_point now) {
   pubsub_value.idontwant.forget(peer);
   if (pubsub_value.controls) { pubsub_value.controls->forget(peer); }
   const auto row = pubsub_value.peers.find(peer);
   if (row == pubsub_value.peers.end()) {
      return;
   }
   if (!row->second.connected) {
      if (row->second.retain_until <= now) {
         pubsub_value.peers.erase(row);
         pubsub_value.scores.erase(peer);
      }
      return;
   }
   const auto retention = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->retain_score
                                                      : std::chrono::milliseconds{60'000};
   auto retain = retention.count() != 0;
   if (pubsub_value.scoring) {
      // Decide while P1 and active mesh penalties still describe the connected peer.
      retain = retain && pubsub_value.scoring->score(peer, now) <= 0.0;
      static_cast<void>(pubsub_value.scoring->disconnect(peer, now));
   }
   if (!retain) {
      pubsub_value.peers.erase(row);
      pubsub_value.scores.erase(peer);
   } else {
      row->second.connected = false;
      row->second.retain_until = detail::pubsub_router::deadline(now, retention);
   }
}

void node::impl::disconnect_pubsub_sessions_locked(std::span<const std::uint64_t> selected,
                                                   std::chrono::steady_clock::time_point now) {
   // Exclude the entire committed victim set, not only the sessions already retired.
   // Admission has published its candidate into sessions before this prepass.
   for (const auto id : selected) {
      const auto found = sessions.find(id);
      if (found == sessions.end()) {
         continue;
      }
      const auto& peer = found->second->info.remote_peer;
      const auto survivor = std::ranges::any_of(sessions, [&](const auto& item) {
         return !item.second->closed && item.second->info.remote_peer == peer &&
                std::ranges::find(selected, item.first) == selected.end();
      });
      if (!survivor) {
         disconnect_pubsub_peer_locked(peer, now);
      }
   }
}

void node::impl::finish_pubsub_inbound(const peer_id& peer, std::uint64_t generation) {
   auto lock = std::scoped_lock{mutex};
   const auto found = pubsub_value.inbound.find(peer);
   if (found == pubsub_value.inbound.end() || found->second.erase(generation) == 0) {
      return;
   }
   if (found->second.empty()) {
      pubsub_value.inbound.erase(found);
   }
}

void node::impl::clear_pubsub_outbound_locked() {
   pubsub_value.idontwant.clear();
   if (pubsub_value.router) { pubsub_value.router->clear(); }
   if (pubsub_value.controls) { pubsub_value.controls->clear(); }
   for (const auto& [_, generation] : pubsub_value.outbound) {
      generation.write_gate->close();
   }
   pubsub_value.outbound.clear();
   pubsub_value.connection_gates.close();
   pubsub_value.outbound_budget.clear();
}

void node::impl::reserve_pubsub_outbound_bytes(const peer_id& peer, std::size_t bytes) {
   auto lock = std::scoped_lock{mutex};
   if (stopped) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "cannot publish GossipSub RPC after node shutdown");
   }
   const auto limit = options.limits.pubsub.limits.max_outbound_queue_bytes;
   if (!pubsub_value.outbound_budget.reserve(peer, bytes, limit)) {
      ++metrics_value.backpressure_rejections;
      ++metrics_value.protocol_rejections;
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub outbound queue byte limit reached");
   }
}

void node::impl::release_pubsub_outbound_bytes(const peer_id& peer, std::size_t bytes) noexcept {
   auto lock = std::scoped_lock{mutex};
   pubsub_value.outbound_budget.release(peer, bytes);
}

[[nodiscard]] std::vector<std::uint8_t> uint64_be(std::uint64_t value) {
   auto out = std::vector<std::uint8_t>(8);
   for (auto i = std::size_t{}; i < out.size(); ++i) {
      out[out.size() - 1 - i] = static_cast<std::uint8_t>((value >> (i * 8U)) & 0xffU);
   }
   return out;
}

void node::impl::increment_pubsub_published() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.pubsub_messages_published;
}

void node::impl::increment_pubsub_received() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.pubsub_messages_received;
}

void node::impl::increment_pubsub_delivered() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.pubsub_messages_delivered;
}

void node::impl::increment_pubsub_duplicate() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.pubsub_duplicates;
}

bool node::impl::increment_pubsub_invalid(const std::shared_ptr<session_state>& session,
    const std::optional<pubsub::topic>& subject, bool protocol_rejected) {
   auto close_offender = false;
   auto failure = std::exception_ptr{};
   {
      const auto lock = std::scoped_lock{mutex};
      if (!pubsub_session_live_locked(session)) { return false; }
      const auto& peer = session->info.remote_peer;
      const auto malformed_transition = subject ? resource_manager::transition_result::accepted :
          resources.record_malformed(resource_manager::scope{.peer = peer, .protocol = builtins::meshsub_v11});
      ++metrics_value.pubsub_invalid_messages;
      if (const auto score = pubsub_value.scores.find(peer); score != pubsub_value.scores.end()) {
         ++score->second.invalid_messages;
      }
      if (protocol_rejected) { ++metrics_value.protocol_rejections; }
      if (subject && pubsub_value.scoring) {
         static_cast<void>(pubsub_value.scoring->reject_invalid(peer, *subject, std::chrono::steady_clock::now()));
      }
      if (malformed_transition != resource_manager::transition_result::accepted &&
          malformed_transition != resource_manager::transition_result::policy_rejected) {
         FORGE_THROW_EXCEPTION(exceptions::internal, "P2P malformed-message resource transition failed");
      }
      if (malformed_transition == resource_manager::transition_result::policy_rejected) {
         ++metrics_value.connection_rejections;
         close_offender = true;
         try {
            // Attribute the same concrete-root failure before this owner can be replaced by G2.
            if (session->direct_endpoint) {
               const auto concrete = session->direct_endpoint->to_multiaddr();
               for (const auto& root : session->direct_roots) {
                  if (root.to_string() == concrete.to_string()) {
                     store.mark_address_failure(peer, root, path::kind::direct,
                         endpoint_backoff_until(peer, root, path::kind::direct));
                  }
               }
            }
            ++metrics_value.direct_failures;
         } catch (...) {
            failure = std::current_exception();
         }
      }
   }
   if (close_offender) {
      try {
         forget_session(session); // Exact registry owner; never close a replacement found by peer id.
      } catch (...) {
         if (!failure) { failure = std::current_exception(); }
      }
      request_cancel_session(session);
   }
   if (failure) { std::rethrow_exception(failure); }
   return true;
}

void node::impl::increment_pubsub_control() {
   auto lock = std::scoped_lock{mutex};
   ++metrics_value.pubsub_control_messages;
}

std::vector<std::uint8_t> node::impl::next_pubsub_seqno() {
   auto lock = std::scoped_lock{mutex};
   return uint64_be(pubsub_value.next_seqno++);
}

pubsub::snapshot node::impl::pubsub_snapshot() const {
   auto lock = std::scoped_lock{mutex};
   auto mesh_edges = std::size_t{};
   for (const auto& [_, peers] : pubsub_value.mesh) {
      mesh_edges += peers.size();
   }
   auto out = pubsub::snapshot{
       .topics = pubsub_value.handlers.size(),
       .peers = pubsub_value.peer_topics.size(),
       .mesh_edges = mesh_edges,
       .cached_messages = pubsub_value.cache.size(),
       .messages_published = metrics_value.pubsub_messages_published,
       .messages_received = metrics_value.pubsub_messages_received,
       .messages_delivered = metrics_value.pubsub_messages_delivered,
       .duplicates = metrics_value.pubsub_duplicates,
       .invalid_messages = metrics_value.pubsub_invalid_messages,
       .control_messages = metrics_value.pubsub_control_messages,
       .trace_failures = pubsub_value.trace_failures.load(std::memory_order_relaxed),
       .application_score_failures = pubsub_value.application_score_failures.load(std::memory_order_relaxed),
       .idontwant_entries = pubsub_value.idontwant.size(),
       .idontwant_bytes = pubsub_value.idontwant.bytes(),
       .idontwant_ignored = pubsub_value.idontwant.ignored(),
   };
   pubsub_value.partial.snapshot(out);
   return out;
}

std::vector<pubsub::subscription> node::impl::local_pubsub_subscriptions(std::uint64_t* epoch) const {
   auto lock = std::scoped_lock{mutex};
   if (epoch) { *epoch = pubsub_value.subscription_epoch; }
   auto out = std::vector<pubsub::subscription>{};
   out.reserve(pubsub_value.handlers.size());
   for (const auto& [topic_value, _] : pubsub_value.handlers) {
      out.push_back(pubsub::subscription{.subscribe = true, .subject = pubsub::topic{.value = topic_value}});
   }
   static_cast<void>(refresh_pubsub_subscriptions_locked(out, builtins::meshsub_v13));
   return out;
}

bool node::impl::refresh_pubsub_subscriptions_locked(std::vector<pubsub::subscription>& subscriptions,
                                                     const protocol_id& protocol) const {
   auto changed = false;
   for (auto row = subscriptions.begin(); row != subscriptions.end();) {
      if (row->subscribe != pubsub_value.handlers.contains(row->subject.value)) {
         row = subscriptions.erase(row); // Retire only this topic's stale intent, never unrelated controls/topics.
         changed = true;
         continue;
      }
      auto requests = std::optional<bool>{};
      auto supports = std::optional<bool>{};
      if (row->subscribe && protocol == builtins::meshsub_v13) {
         if (const auto registration = pubsub_value.partial.find(row->subject)) {
            requests = registration->options.requests_partial;
            supports = true;
         }
      }
      changed = row->requests_partial != requests || row->supports_sending_partial != supports || changed;
      row->requests_partial = requests;
      row->supports_sending_partial = supports;
      ++row;
   }
   return changed;
}

std::vector<peer_id> node::impl::pubsub_candidate_peers(const std::string& topic_value,
                                                        std::optional<peer_id> except) const {
   auto out = std::vector<peer_id>{};
   {
      auto lock = std::scoped_lock{mutex};
      if (const auto mesh = pubsub_value.mesh.find(topic_value); mesh != pubsub_value.mesh.end()) {
         for (const auto& peer : mesh->second) {
            if (!except || peer != *except) {
               out.push_back(peer);
            }
         }
      }
      for (const auto& [peer, topics] : pubsub_value.peer_topics) {
         if (topics.contains(topic_value) && (!except || peer != *except) &&
             std::ranges::find(out, peer) == out.end()) {
            out.push_back(peer);
         }
      }
      for (const auto& [_, session] : sessions) {
         const auto& peer = session->info.remote_peer;
         if ((!except || peer != *except) && std::ranges::find(out, peer) == out.end()) {
            out.push_back(peer);
         }
      }
   }
   for (const auto& record : store.candidates(capabilities::pubsub, options.peer_state.max_peers)) {
      const auto supports_pubsub = record.capabilities.has(capabilities::pubsub) ||
                                   std::ranges::any_of(record.protocols, [](const protocol_id& protocol) {
                                      return protocol == builtins::meshsub_v13 || protocol == builtins::meshsub_v12 ||
                                          protocol == builtins::meshsub_v11 || protocol == builtins::meshsub_v10;
                                   });
      if (supports_pubsub && (!except || record.peer != *except) && std::ranges::find(out, record.peer) == out.end()) {
         out.push_back(record.peer);
      }
   }
   return out;
}

} // namespace forge::net::p2p
