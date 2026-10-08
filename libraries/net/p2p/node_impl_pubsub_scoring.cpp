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
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
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
#include <boost/asio/cancellation_signal.hpp>
#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/experimental/concurrent_channel.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/system/system_error.hpp>

module forge.net.p2p.node;
import :lifecycle_stop_listener;
import forge.exceptions;
import forge.asio.gate;
import forge.asio.notification;
import forge.crypto.asymmetric;
import forge.net.p2p.exceptions;
import forge.net.p2p.pubsub;
import forge.net.p2p.stream;
import forge.net.transport.endpoint;
import forge.net.transport.session;
import forge.net.transport.stream;
import forge.net.yamux.session;

#include "details/node_impl.hxx"
#include "details/pubsub_peer_score.hxx"
#include "details/pubsub_router.hxx"

namespace forge::net::p2p {

void node::impl::initialize_pubsub() {
   pubsub::validate(options.limits.pubsub);
   if (!options.capabilities.has(capabilities::pubsub)) {
      return;
   }
   pubsub_value.router = std::make_shared<detail::pubsub_router>(options.limits.pubsub.limits.max_iwant_promises);
   pubsub_value.controls = std::make_shared<detail::pubsub_control_queue>(options.limits.pubsub.limits);
   auto params = options.limits.pubsub.scoring.value_or(pubsub::scoring_params{});
   if (!options.limits.pubsub.scoring) {
      // Neutral accounting still owns bounded seen IDs/validation generations; scoring policy remains opt-in.
      params.limits.max_retained_peers = options.peer_state.max_peers;
      params.limits.max_connected_peers = std::min(options.peer_state.max_peers, options.limits.max_sessions);
      const auto& wire = options.limits.pubsub.limits;
      params.limits.max_topic_size = wire.max_topic_size;
      params.limits.max_topics = wire.max_topics;
      // Author+seqno fits its wire payload; anonymous IDs are fixed 32-byte SHA-256 hashes.
      params.limits.max_message_id_size = std::max(std::size_t{32}, wire.max_message_size);
      // Preserve the accounting budget floor; the larger cache product was overflow-checked above.
      params.limits.max_messages = std::max(params.limits.max_messages, wire.history_length * wire.max_messages);
      params.limits.max_ips_per_peer = options.limits.max_sessions;
      params.retain_score = std::chrono::milliseconds{60'000};
   }
   pubsub_value.scoring = std::make_shared<detail::pubsub_peer_score>(std::move(params), std::chrono::steady_clock::now());
}

bool node::impl::connect_pubsub_peer_locked(const peer_id& peer) {
   if (stopped || !options.capabilities.has(capabilities::pubsub)) {
      return false;
   }
   auto session = std::shared_ptr<session_state>{};
   for (const auto kind : std::array{path::kind::direct, path::kind::hole_punch, path::kind::relay}) {
      const auto candidate = session_for_path_locked(peer, kind, std::nullopt);
      if (candidate && candidate->authentication != peer_authentication::unverified) {
         session = candidate;
         break;
      }
   }
   const auto owner = session ? sessions.find(session->id) : sessions.end();
   // A delayed announcement is not proof that its authenticated session still exists.
   if (!session || session->closed || owner == sessions.end() || owner->second != session) {
      return false;
   }
   const auto bound = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->limits.max_retained_peers
                                                  : options.peer_state.max_peers;
   const auto now = std::chrono::steady_clock::now();
   if (!pubsub_value.peers.contains(peer) && pubsub_value.peers.size() >= bound) {
      return false;
   }
   const auto [row, inserted] = pubsub_value.peers.try_emplace(peer);
   try {
      pubsub_value.scores.try_emplace(peer);
      if (pubsub_value.scoring) {
         auto ips = std::vector<boost::asio::ip::address>{};
         for (const auto& [_, session] : sessions) {
            if (session->info.remote_peer != peer || session->closed ||
                session->authentication == peer_authentication::unverified ||
                session->info.path != path::kind::direct || !session->remote_endpoint ||
                session->remote_endpoint->relayed) {
               continue;
            }
            const auto& remote = session->remote_endpoint->transport;
            if (remote.host_type != endpoint::host_kind::ip4 && remote.host_type != endpoint::host_kind::ip6) {
               continue;
            }
            const auto ip = remote.literal_address();
            if (std::ranges::find(ips, ip) == ips.end()) {
               ips.push_back(ip);
            }
         }
         if (!pubsub_value.scoring->connect(peer, ips, now)) {
            if (inserted) {
               pubsub_value.peers.erase(row);
               pubsub_value.scores.erase(peer);
            }
            return false;
         }
      }
   } catch (...) {
      if (inserted) {
         pubsub_value.peers.erase(row);
         pubsub_value.scores.erase(peer);
      }
      throw;
   }
   if (inserted || !row->second.connected) {
      row->second.generation = pubsub_value.next_peer_generation++;
   }
   row->second.connected = true;
   row->second.retain_until = {};
   return true;
}

void node::impl::forget_pubsub_endpoint_locked(const session_state& session) {
   if (!pubsub_value.scoring || session.info.path != path::kind::direct || !session.remote_endpoint ||
       session.remote_endpoint->relayed) {
      return;
   }
   const auto& endpoint = session.remote_endpoint->transport;
   if (endpoint.host_type != forge::net::p2p::endpoint::host_kind::ip4 &&
       endpoint.host_type != forge::net::p2p::endpoint::host_kind::ip6) {
      return;
   }
   const auto canonical = [](boost::asio::ip::address ip) {
      if (ip.is_v6() && ip.to_v6().is_v4_mapped()) {
         const auto bytes = ip.to_v6().to_bytes();
         return boost::asio::ip::address{boost::asio::ip::address_v4{
            boost::asio::ip::address_v4::bytes_type{bytes[12], bytes[13], bytes[14], bytes[15]}}};
      }
      return ip;
   };
   const auto ip = canonical(endpoint.literal_address());
   for (const auto& [_, current] : sessions) {
      if (current->closed || current->info.remote_peer != session.info.remote_peer ||
          current->info.path != path::kind::direct || !current->remote_endpoint || current->remote_endpoint->relayed ||
          current->authentication == peer_authentication::unverified) {
         continue;
      }
      const auto& remote = current->remote_endpoint->transport;
      if ((remote.host_type == forge::net::p2p::endpoint::host_kind::ip4 ||
           remote.host_type == forge::net::p2p::endpoint::host_kind::ip6) && canonical(remote.literal_address()) == ip) {
         return;
      }
   }
   pubsub_value.scoring->remove_ip(session.info.remote_peer, ip, std::chrono::steady_clock::now());
}

void node::impl::sample_pubsub_application_scores(const std::optional<peer_id>& only) {
   if (!options.limits.pubsub.scoring || !options.limits.pubsub.scoring->app_specific_score) {
      return;
   }
   auto peers = std::vector<std::pair<peer_id, std::uint64_t>>{};
   {
      const auto lock = std::scoped_lock{mutex};
      for (const auto& [peer, state] : pubsub_value.peers) {
         if (state.connected && (!only || *only == peer)) {
            peers.emplace_back(peer, state.generation);
         }
      }
   }
   for (const auto& [peer, generation] : peers) {
      auto sample = double{};
      try {
         sample = options.limits.pubsub.scoring->app_specific_score(peer);
      } catch (...) {
         // User sampling cannot terminate maintenance or replace the last committed score.
         pubsub_value.application_score_failures.fetch_add(1, std::memory_order_relaxed);
         continue;
      }
      if (!std::isfinite(sample)) {
         pubsub_value.application_score_failures.fetch_add(1, std::memory_order_relaxed);
         continue;
      }
      const auto lock = std::scoped_lock{mutex};
      const auto row = pubsub_value.peers.find(peer);
      if (row != pubsub_value.peers.end() && row->second.connected && row->second.generation == generation) {
         static_cast<void>(pubsub_value.scoring->set_application_score(peer, sample, std::chrono::steady_clock::now()));
      }
   }
}

bool node::impl::pubsub_peer_live_locked(const peer_id& peer) const {
   const auto row = pubsub_value.peers.find(peer);
   return row != pubsub_value.peers.end() && row->second.connected;
}

bool node::impl::pubsub_peer_outbound_locked(const peer_id& peer) const {
   return std::ranges::any_of(sessions, [&](const auto& row) {
      return !row.second->closed && row.second->info.remote_peer == peer &&
             row.second->direction == connection_manager::direction::outbound;
   });
}

double node::impl::pubsub_score_locked(const peer_id& peer) {
   return pubsub_value.scoring ? pubsub_value.scoring->score(peer, std::chrono::steady_clock::now()) : 0.0;
}

void node::impl::graft_pubsub_peer_locked(const std::string& topic, const peer_id& peer) {
   if (pubsub_value.mesh[topic].insert(peer).second && pubsub_value.scoring) {
      static_cast<void>(pubsub_value.scoring->graft(peer, std::string_view{topic}, std::chrono::steady_clock::now()));
   }
}

void node::impl::prune_pubsub_peer_locked(const std::string& topic, const peer_id& peer) {
   const auto mesh = pubsub_value.mesh.find(topic);
   if (mesh != pubsub_value.mesh.end() && mesh->second.erase(peer) != 0 && pubsub_value.scoring) {
      static_cast<void>(pubsub_value.scoring->prune(peer, std::string_view{topic}, std::chrono::steady_clock::now()));
   }
}

pubsub::score_snapshot node::impl::pubsub_scores() const {
   const auto lock = std::scoped_lock{mutex};
   auto out = pubsub_value.scoring ? pubsub_value.scoring->snapshot(std::chrono::steady_clock::now())
                                  : pubsub::score_snapshot{};
   if (!pubsub_value.scoring) {
      for (const auto& [peer, state] : pubsub_value.peers) {
         out.peers.push_back(pubsub::peer_score_snapshot{.peer = peer, .connected = state.connected});
         state.connected ? ++out.connected_peers : ++out.retained_peers;
      }
   }
   // Routing membership is an actual node fact, including neutral/unconfigured topics.
   auto indices = std::map<peer_id, std::size_t>{};
   for (auto index = std::size_t{}; index < out.peers.size(); ++index) {
      indices.emplace(out.peers[index].peer, index);
      for (auto& topic : out.peers[index].topics) {
         topic.in_mesh = false;
      }
   }
   for (const auto& [subject, mesh] : pubsub_value.mesh) {
      for (const auto& member : mesh) {
         const auto row = indices.find(member);
         if (row == indices.end()) {
            continue;
         }
         auto& peer = out.peers[row->second];
         const auto found = std::ranges::find_if(peer.topics, [&](const auto& row) { return row.subject.value == subject; });
         if (found != peer.topics.end()) {
            found->in_mesh = true;
         } else {
            peer.topics.push_back(pubsub::topic_score_snapshot{.subject = pubsub::topic{subject}, .in_mesh = true});
         }
      }
   }
   return out;
}

std::vector<peer_id> node::impl::pubsub_publish_peers(const std::string& topic) {
   sample_pubsub_application_scores();
   const auto lock = std::scoped_lock{mutex};
   const auto threshold = options.limits.pubsub.scoring ? options.limits.pubsub.scoring->thresholds.publish_threshold : 0.0;
   auto candidates = std::vector<peer_id>{};
   for (const auto& [peer, topics] : pubsub_value.peer_topics) {
      if (topics.contains(topic) && pubsub_peer_live_locked(peer) && pubsub_score_locked(peer) >= threshold) {
         candidates.push_back(peer);
      }
   }
   if (options.limits.pubsub.flood_publish) {
      return candidates;
   }
   if (const auto mesh = pubsub_value.mesh.find(topic); mesh != pubsub_value.mesh.end() && pubsub_value.handlers.contains(topic)) {
      std::erase_if(candidates, [&](const peer_id& peer) { return !mesh->second.contains(peer); });
      return candidates;
   }
   if (!pubsub_value.fanout.contains(topic) && pubsub_value.fanout.size() >= options.limits.pubsub.limits.max_topics) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "GossipSub fanout topic limit reached");
   }
   auto& fanout = pubsub_value.fanout[topic];
   fanout.last_publish = std::chrono::steady_clock::now();
   std::erase_if(fanout.peers, [&](const peer_id& peer) { return std::ranges::find(candidates, peer) == candidates.end(); });
   pubsub_value.router->shuffle(candidates);
   for (const auto& peer : candidates) {
      if (fanout.peers.size() >= options.limits.pubsub.limits.mesh_n) {
         break;
      }
      fanout.peers.insert(peer);
   }
   return {fanout.peers.begin(), fanout.peers.end()};
}

std::vector<peer_id> node::impl::pubsub_forward_peers(const std::string& topic, const peer_id& sender,
                                                   const std::optional<peer_id>& author) {
   const auto lock = std::scoped_lock{mutex};
   auto out = std::vector<peer_id>{};
   if (const auto mesh = pubsub_value.mesh.find(topic); mesh != pubsub_value.mesh.end()) {
      for (const auto& peer : mesh->second) {
         if (peer != sender && (!author || peer != *author) && pubsub_peer_live_locked(peer) && pubsub_score_locked(peer) >= 0) {
            out.push_back(peer);
         }
      }
   }
   return out;
}

std::optional<node::impl::pubsub_state::request> node::impl::stage_pubsub_request_locked(
    const peer_id& peer, const std::vector<std::vector<std::uint8_t>>& ids) {
   if (ids.empty()) {
      return std::nullopt;
   }
   if (!pubsub_peer_live_locked(peer)) {
      return std::nullopt;
   }
   const auto& limits = options.limits.pubsub.limits;
   if (pubsub_value.cache.size() >= limits.history_length * limits.max_messages) {
      return std::nullopt;
   }
   const auto& id = ids[pubsub_value.router->choose(ids.size())];
   auto request = pubsub_state::request{.peer = peer, .id = bytes_key(id)};
   if (pubsub_value.cache.contains(request.id)) {
      return std::nullopt; // Retry requests for cached validations are not delivery promises.
   }
   if (const auto generation = pubsub_value.router->stage(peer, request.id)) {
      request.generation = *generation;
      return request;
   }
   return std::nullopt;
}

void node::impl::finish_pubsub_request(const pubsub_state::request& request, bool sent,
                                      std::shared_ptr<session_state> origin) noexcept {
   const auto lock = std::scoped_lock{mutex};
   if (sent && !stopped &&
       (!origin || (origin->info.remote_peer == request.peer && pubsub_session_live_locked(origin)))) {
      const auto until = detail::pubsub_router::deadline(std::chrono::steady_clock::now(),
         options.limits.pubsub.limits.iwant_followup_time);
      static_cast<void>(pubsub_value.router->activate(request.peer, request.id, request.generation, until));
   } else {
      pubsub_value.router->abort(request.peer, request.id, request.generation);
   }
}

} // namespace forge::net::p2p
