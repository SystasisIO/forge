module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.node;

import forge.net.p2p.exceptions;
import forge.net.p2p.identity;

#include "details/connection_manager.hxx"

namespace forge::net::p2p {
namespace {

std::atomic_bool fail_next_peer_session_prepare_for_test = false;

[[nodiscard]] std::int64_t checked_tag_sum(std::int64_t left, std::int64_t right) {
   if ((right > 0 && left > (std::numeric_limits<std::int64_t>::max)() - right) ||
       (right < 0 && left < (std::numeric_limits<std::int64_t>::min)() - right)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P peer tag value overflow");
   }
   return left + right;
}

[[nodiscard]] double normalized_network_score(double value) noexcept {
   return std::isfinite(value) ? value : 0.0;
}

} // namespace

connection_manager::connection_manager(policy value) : policy_(value) {}

const connection_manager::policy& connection_manager::configured_policy() const noexcept {
   return policy_;
}

void connection_manager::protect(const peer_id& peer, std::string tag) {
   if (peer.value.empty() || tag.empty()) {
      return;
   }
   protected_[peer].insert(std::move(tag));
}

bool connection_manager::unprotect(const peer_id& peer, std::string_view tag) {
   const auto found = protected_.find(peer);
   if (found == protected_.end()) {
      return false;
   }
   if (!tag.empty()) {
      found->second.erase(std::string{tag});
   }
   if (tag.empty() || found->second.empty()) {
      protected_.erase(found);
      return false;
   }
   return true;
}

bool connection_manager::is_protected(const peer_id& peer) const {
   const auto found = protected_.find(peer);
   return found != protected_.end() && !found->second.empty();
}

void connection_manager::tag(const peer_id& peer, std::string tag, std::int64_t value) {
   if (peer.value.empty() || tag.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P peer tag and peer id must be non-empty");
   }
   if (tag.size() > policy_.max_tag_size) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P peer tag exceeds the configured size limit");
   }

   const auto found = tags_.find(peer);
   const auto existing_tag = found != tags_.end() && found->second.contains(tag);
   if (!existing_tag && found == tags_.end() && tags_.size() >= policy_.max_tagged_peers) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P tagged peer limit reached");
   }
   if (!existing_tag && found != tags_.end() && found->second.size() >= policy_.max_tags_per_peer) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P tags per peer limit reached");
   }

   auto aggregate = std::int64_t{0};
   if (found != tags_.end()) {
      for (const auto& [name, current] : found->second) {
         if (name != tag) {
            aggregate = checked_tag_sum(aggregate, current);
         }
      }
   }
   static_cast<void>(checked_tag_sum(aggregate, value));
   if (found == tags_.end()) {
      tags_.emplace(peer, std::map<std::string, std::int64_t>{{std::move(tag), value}});
   } else {
      found->second.insert_or_assign(std::move(tag), value);
   }
}

bool connection_manager::untag(const peer_id& peer, std::string_view tag) {
   if (tag.empty()) {
      return false;
   }
   const auto found = tags_.find(peer);
   if (found == tags_.end()) {
      return false;
   }
   const auto erased = found->second.erase(std::string{tag}) != 0;
   if (found->second.empty()) {
      tags_.erase(found);
   }
   return erased;
}

std::int64_t connection_manager::aggregate_tag_value(const peer_id& peer) const {
   const auto found = tags_.find(peer);
   if (found == tags_.end()) {
      return 0;
   }
   auto aggregate = std::int64_t{0};
   for (const auto& [_, value] : found->second) {
      aggregate = checked_tag_sum(aggregate, value);
   }
   return aggregate;
}

void connection_manager::update_network_score(const peer_id& peer, double score) {
   const auto sessions = sessions_by_peer_.find(peer);
   if (sessions == sessions_by_peer_.end()) {
      return;
   }
   const auto normalized = normalized_network_score(score);
   network_scores_[peer] = normalized;
   for (const auto id : sessions->second) {
      if (const auto session = sessions_.find(id); session != sessions_.end()) {
         session->second.network_score = normalized;
      }
   }
}

connection_manager::peer_prune_plan
connection_manager::plan_peer_prune(std::size_t target_peers, std::size_t max_victims,
                                    std::chrono::steady_clock::time_point now) const {
   auto result = peer_prune_plan{.connected_peers = sessions_by_peer_.size()};
   if (target_peers >= result.connected_peers || max_victims == 0) {
      return result;
   }

   auto candidates = std::vector<peer_prune_candidate>{};
   candidates.reserve(sessions_by_peer_.size());
   for (const auto& [peer, ids] : sessions_by_peer_) {
      if (ids.empty() || is_protected(peer)) {
         continue;
      }

      auto latest_opened_at = std::chrono::steady_clock::time_point{};
      auto latest_used_at = std::chrono::steady_clock::time_point{};
      for (const auto id : ids) {
         const auto session = sessions_.find(id);
         if (session == sessions_.end()) {
            continue;
         }
         latest_opened_at = std::max(latest_opened_at, session->second.opened_at);
         latest_used_at = std::max(latest_used_at, session->second.last_used_at);
      }
      if (now - latest_opened_at < policy_.grace_period) {
         continue;
      }

      const auto score = network_scores_.find(peer);
      candidates.push_back(peer_prune_candidate{
          .peer = peer,
          .tag_value = aggregate_tag_value(peer),
          .network_score = score == network_scores_.end() ? 0.0 : score->second,
          .opened_at = latest_opened_at,
          .last_used_at = latest_used_at,
      });
   }

   std::sort(candidates.begin(), candidates.end(),
             [](const peer_prune_candidate& left, const peer_prune_candidate& right) {
                if (left.tag_value != right.tag_value) {
                   return left.tag_value < right.tag_value;
                }
                if (left.network_score != right.network_score) {
                   return left.network_score < right.network_score;
                }
                if (left.last_used_at != right.last_used_at) {
                   return left.last_used_at < right.last_used_at;
                }
                if (left.opened_at != right.opened_at) {
                   return left.opened_at < right.opened_at;
                }
                return left.peer.to_string() < right.peer.to_string();
             });

   const auto required_victims = result.connected_peers - target_peers;
   const auto selected = std::min(required_victims, max_victims);
   result.victim_peers.reserve(selected);
   for (auto index = std::size_t{}; index < selected && index < candidates.size(); ++index) {
      const auto& victim = candidates[index];
      result.victim_peers.push_back(victim.peer);
      const auto sessions = sessions_by_peer_.find(victim.peer);
      if (sessions != sessions_by_peer_.end()) {
         result.session_ids.insert(result.session_ids.end(), sessions->second.begin(), sessions->second.end());
      }
   }
   return result;
}

void connection_manager::erase_record(std::uint64_t id) {
   const auto found = sessions_.find(id);
   if (found == sessions_.end()) {
      return;
   }
   if (auto peer = sessions_by_peer_.find(found->second.peer); peer != sessions_by_peer_.end()) {
      peer->second.erase(id);
      if (peer->second.empty()) {
         sessions_by_peer_.erase(peer);
         network_scores_.erase(found->second.peer);
      }
   }
   sessions_.erase(found);
}

bool connection_manager::should_prune_before(const session_record& left, const session_record& right) const {
   const auto left_tag = aggregate_tag_value(left.peer);
   const auto right_tag = aggregate_tag_value(right.peer);
   if (left_tag != right_tag) {
      return left_tag < right_tag;
   }
   const auto left_score = network_scores_.contains(left.peer) ? network_scores_.at(left.peer) : 0.0;
   const auto right_score = network_scores_.contains(right.peer) ? network_scores_.at(right.peer) : 0.0;
   if (left_score != right_score) {
      return left_score < right_score;
   }
   if (left.last_used_at != right.last_used_at) {
      return left.last_used_at < right.last_used_at;
   }
   if (left.opened_at != right.opened_at) {
      return left.opened_at < right.opened_at;
   }
   if (left.peer != right.peer) {
      return left.peer.to_string() < right.peer.to_string();
   }
   return left.id < right.id;
}

std::optional<std::uint64_t> connection_manager::select_prune_one(const std::vector<std::uint64_t>& selected,
                                                                  std::chrono::steady_clock::time_point now,
                                                                  std::optional<direction> required_direction) const {
   auto victim = sessions_.end();
   for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
      if (std::ranges::find(selected, it->first) != selected.end()) {
         continue;
      }
      if (required_direction && it->second.direction != *required_direction) {
         continue;
      }
      if (is_protected(it->second.peer) || now - it->second.opened_at < policy_.grace_period) {
         continue;
      }
      if (victim == sessions_.end() || should_prune_before(it->second, victim->second)) {
         victim = it;
      }
   }
   if (victim == sessions_.end()) {
      return std::nullopt;
   }
   return victim->first;
}

connection_manager::admission connection_manager::remember(session_record record,
                                                           std::chrono::steady_clock::time_point now) {
   if (record.peer.value.empty()) {
      return admission{.accepted = false, .reason = "P2P session peer id is empty"};
   }
   if (record.id == 0) {
      return admission{.accepted = false, .reason = "P2P session id is empty"};
   }
   if (sessions_.contains(record.id)) {
      return admission{.accepted = false, .reason = "P2P duplicate session id"};
   }
   if (record.opened_at == std::chrono::steady_clock::time_point{}) {
      record.opened_at = now;
   }
   if (record.last_used_at == std::chrono::steady_clock::time_point{}) {
      record.last_used_at = record.opened_at;
   }

   auto result = admission{};
   const auto may_prune = !last_prune_ || now - *last_prune_ >= policy_.prune_silence;
   const auto global_saturated = sessions_.size() >= policy_.max_sessions;

   if (global_saturated) {
      // The candidate itself needs a slot. Keep the public low watermark valid
      // at one, while still allowing a max_sessions == 1 replacement.
      const auto target =
          policy_.max_sessions == 0 ? std::size_t{0} : std::min(policy_.low_watermark, policy_.max_sessions - 1);
      result.pruned.reserve(sessions_.size() - target);
      while (may_prune && sessions_.size() - result.pruned.size() > target) {
         const auto victim = select_prune_one(result.pruned, now);
         if (!victim) {
            break;
         }
         result.pruned.push_back(*victim);
      }
      if (sessions_.size() - result.pruned.size() >= policy_.max_sessions) {
         return admission{.accepted = false, .reason = "P2P max sessions reached"};
      }
   }

   const auto id = record.id;
   const auto peer = record.peer;
   const auto score = normalized_network_score(record.network_score);
   auto session_inserted = false;
   auto peer_inserted = false;
   auto peer_session_inserted = false;
   auto score_inserted = false;
   try {
      if (std::exchange(fail_next_prepare_for_test_, false)) {
         throw std::bad_alloc{};
      }
      const auto [session, inserted] = sessions_.emplace(id, std::move(record));
      static_cast<void>(session);
      if (!inserted) {
         return admission{.accepted = false, .reason = "P2P duplicate session id"};
      }
      session_inserted = inserted;
      auto [peer_sessions, inserted_peer] = sessions_by_peer_.try_emplace(peer);
      peer_inserted = inserted_peer;
      if (fail_next_peer_session_prepare_for_test.exchange(false, std::memory_order_relaxed)) {
         throw std::bad_alloc{};
      }
      peer_session_inserted = peer_sessions->second.insert(id).second;
      auto [network_score, inserted_score] = network_scores_.try_emplace(peer, score);
      score_inserted = inserted_score;
      if (!inserted_score) {
         network_score->second = score;
      }
   } catch (...) {
      if (score_inserted) {
         network_scores_.erase(peer);
      }
      if (auto found = sessions_by_peer_.find(peer); found != sessions_by_peer_.end()) {
         if (peer_session_inserted) {
            found->second.erase(id);
         }
         if (peer_inserted && found->second.empty()) {
            sessions_by_peer_.erase(found);
         }
      }
      if (session_inserted) {
         sessions_.erase(id);
      }
      throw;
   }
   for (const auto victim : result.pruned) {
      erase_record(victim);
   }
   if (!result.pruned.empty()) {
      last_prune_ = now;
   }
   result.accepted = true;
   return result;
}

void connection_manager::forget(std::uint64_t id) {
   erase_record(id);
}

void connection_manager::forget_peer(const peer_id& peer) {
   auto found = sessions_by_peer_.find(peer);
   if (found == sessions_by_peer_.end()) {
      return;
   }
   auto ids = std::vector<std::uint64_t>{found->second.begin(), found->second.end()};
   for (const auto id : ids) {
      forget(id);
   }
}

void connection_manager::touch(std::uint64_t id, std::chrono::steady_clock::time_point now) {
   if (auto found = sessions_.find(id); found != sessions_.end()) {
      found->second.last_used_at = now;
   }
}

void connection_manager::clear() {
   sessions_.clear();
   sessions_by_peer_.clear();
   network_scores_.clear();
}

connection_manager::snapshot connection_manager::current(std::size_t max_sessions) const {
   auto out = snapshot{
       .active_sessions = sessions_.size(),
       .active_peers = sessions_by_peer_.size(),
   };
   out.protected_peers.reserve(protected_.size());
   for (const auto& [peer, tags] : protected_) {
      if (!tags.empty()) {
         out.protected_peers.push_back(peer);
      }
   }
   out.sessions.reserve(std::min(max_sessions, sessions_.size()));
   for (const auto& [_, record] : sessions_) {
      if (out.sessions.size() >= max_sessions) {
         break;
      }
      out.sessions.push_back(record);
   }
   return out;
}

std::size_t connection_manager::size() const noexcept {
   return sessions_.size();
}

connection_manager::policy connection_policy_for(const node::limits& limits) {
   return connection_manager::policy{
       .max_sessions = limits.max_sessions,
       .low_watermark = limits.session_low_watermark,
       .max_tagged_peers = limits.topology.max_tagged_peers,
       .max_tags_per_peer = limits.topology.max_tags_per_peer,
       .max_tag_size = limits.topology.max_tag_size,
       .grace_period = limits.session_grace_period,
       .prune_silence = limits.session_prune_silence,
   };
}

void connection_manager::fail_next_prepare_for_test() noexcept {
   fail_next_prepare_for_test_ = true;
}

namespace detail {

void fail_next_connection_manager_peer_session_prepare_for_test() noexcept {
   fail_next_peer_session_prepare_for_test.store(true, std::memory_order_relaxed);
}

} // namespace detail

} // namespace forge::net::p2p
