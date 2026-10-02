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
   for (auto& [_, mesh] : pubsub_value.mesh) {
      mesh.erase(peer);
   }
}

void node::impl::forget_pubsub_peer_locked(const peer_id& peer) {
   pubsub_value.inbound.erase(peer);
   pubsub_value.peer_topics.erase(peer);
   for (auto& [_, mesh] : pubsub_value.mesh) {
      mesh.erase(peer);
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

void node::impl::increment_pubsub_invalid(const peer_id& peer) {
   auto offender = std::shared_ptr<session_state>{};
   const auto malformed_transition =
       resources.record_malformed(resource_manager::scope{.peer = peer, .protocol = builtins::meshsub_v11});
   {
      auto lock = std::scoped_lock{mutex};
      ++metrics_value.pubsub_invalid_messages;
      pubsub_value.scores[peer].invalid_messages += 1;
      pubsub_value.scores[peer].value -= 1.0;
      if (malformed_transition == resource_manager::transition_result::policy_rejected) {
         ++metrics_value.connection_rejections;
         for (const auto& [_, session] : sessions) {
            if (session->info.remote_peer == peer && !session->closed) {
               offender = session;
            }
         }
      }
   }
   if (malformed_transition != resource_manager::transition_result::accepted &&
       malformed_transition != resource_manager::transition_result::policy_rejected) {
      FORGE_THROW_EXCEPTION(exceptions::internal, "P2P malformed-message resource transition failed");
   }
   if (offender) {
      record_direct_session_failure(offender);
      forget_session(offender);
      detail::request_session_cancel(offender->connection);
   }
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
   return pubsub::snapshot{
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
   };
}

std::vector<pubsub::subscription> node::impl::local_pubsub_subscriptions() const {
   auto lock = std::scoped_lock{mutex};
   auto out = std::vector<pubsub::subscription>{};
   out.reserve(pubsub_value.handlers.size());
   for (const auto& [topic_value, _] : pubsub_value.handlers) {
      out.push_back(pubsub::subscription{.subscribe = true, .subject = pubsub::topic{.value = topic_value}});
   }
   return out;
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
                                      return protocol == builtins::meshsub_v11 || protocol == builtins::meshsub_v10;
                                   });
      if (supports_pubsub && (!except || record.peer != *except) && std::ranges::find(out, record.peer) == out.end()) {
         out.push_back(record.peer);
      }
   }
   return out;
}

} // namespace forge::net::p2p
