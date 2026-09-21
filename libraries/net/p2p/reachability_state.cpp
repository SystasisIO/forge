module;

#include <algorithm>
#include <chrono>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.host_event;
import forge.net.p2p.identity;
import forge.net.p2p.reachability;
import forge.net.p2p.reachability_policy;

#include "details/host_addresses.hxx"
#include "details/reachability_state.hxx"

namespace forge::net::p2p::detail {
namespace {

bool same_address(const endpoint& left, const endpoint& right) {
   return left.transport.host_type == right.transport.host_type && left.transport.host == right.transport.host &&
          left.transport.zone == right.transport.zone && left.transport.port == right.transport.port &&
          left.transport.protocol == right.transport.protocol &&
          left.encapsulation == right.encapsulation && left.relayed.has_value() == right.relayed.has_value() &&
          (!left.relayed || left.relayed->target == right.relayed->target);
}

bool same_address(const std::optional<endpoint>& left, const std::optional<endpoint>& right) {
   return left.has_value() == right.has_value() && (!left || same_address(*left, *right));
}

} // namespace

reachability_state::reachability_state(reachability_policy policy) : policy_(policy) { validate(policy_); }

bool reachability_state::record_v1(const peer_id& observer, const endpoint& remote,
                                    std::uint64_t generation, reachability::state value, bool internet_scope,
                                    std::chrono::steady_clock::time_point now) {
   return internet_scope && record(observer, remote, std::nullopt, generation, value, now);
}

bool reachability_state::record_v2(const peer_id& observer, const endpoint& remote, std::uint64_t generation, endpoint address,
                                    reachability::state value, bool verified_dialback,
                                    std::chrono::steady_clock::time_point now) {
   if (value == reachability::state::publicly_reachable && !verified_dialback) {
      return false;
   }
   address.peer.reset();
   return record(observer, remote, std::move(address), generation, value, now);
}

bool reachability_state::record(const peer_id& observer, const endpoint& remote, std::optional<endpoint> address,
                                 std::uint64_t generation, reachability::state value, std::chrono::steady_clock::time_point now) {
   if (!valid_peer_id(observer) || (value != reachability::state::publicly_reachable &&
       value != reachability::state::private_network && value != reachability::state::unknown)) {
      return false;
   }
   const auto group = host_addresses::observer_group(remote);
   if (!group) {
      return false;
   }
   const auto lock = std::scoped_lock{mutex_};
   if (closed_ || generation != address_generation_ || addresses_.empty() ||
       (address && std::ranges::none_of(addresses_, [&](const auto& value) { return same_address(value, *address); }))) {
      return false;
   }
   std::erase_if(observations_, [&](const auto& entry) { return entry.expires_at <= now; });
   const auto previous = std::ranges::find_if(observations_, [&](const auto& entry) {
      return entry.peer == observer && same_address(entry.address, address);
   });
   if (value == reachability::state::unknown) {
      if (previous != observations_.end()) {
         observations_.erase(previous);
      }
      return true;
   }
   const auto ttl = std::chrono::duration_cast<std::chrono::steady_clock::duration>(policy_.observation_ttl);
   const auto expires = now > std::chrono::steady_clock::time_point::max() - ttl
       ? std::chrono::steady_clock::time_point::max() : now + ttl;
   auto next = observation{observer, *group, std::move(address), value, expires};
   if (previous != observations_.end()) {
      *previous = std::move(next);
      return true;
   }
   if (observations_.size() >= policy_.max_observations) {
      return false;
   }
   auto peers = std::set<peer_id>{};
   auto addresses = std::set<std::string>{};
   for (const auto& entry : observations_) {
      peers.insert(entry.peer);
      if (entry.address) {
         addresses.insert(entry.address->to_string());
      }
   }
   if ((!peers.contains(observer) && peers.size() >= policy_.max_observers) ||
       (next.address && !addresses.contains(next.address->to_string()) && addresses.size() >= policy_.max_candidates)) {
      return false;
   }
   observations_.push_back(std::move(next));
   return true;
}

std::uint64_t reachability_state::set_addresses(std::span<const endpoint> current) {
   if (current.size() > policy_.max_candidates) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P reachability address limit reached");
   }
   auto addresses = std::vector<endpoint>{current.begin(), current.end()};
   for (auto& value : addresses) {
      value.peer.reset();
   }
   std::ranges::sort(addresses, {}, [](const endpoint& value) { return value.to_string(); });
   const auto duplicate = std::ranges::unique(addresses, [](const auto& left, const auto& right) {
      return same_address(left, right);
   });
   addresses.erase(duplicate.begin(), duplicate.end());
   const auto lock = std::scoped_lock{mutex_};
   if (closed_) {
      FORGE_THROW_EXCEPTION(exceptions::closed, "P2P reachability result admission is closed");
   }
   if (std::ranges::equal(addresses, addresses_, [](const auto& left, const auto& right) { return same_address(left, right); })) {
      return address_generation_;
   }
   if (address_generation_ == std::numeric_limits<std::uint64_t>::max()) {
      closed_ = true;
      observations_.clear();
      addresses_.clear();
      FORGE_THROW_EXCEPTION(exceptions::sequence_exhausted, "P2P reachability address generation exhausted");
   }
   addresses_.swap(addresses);
   observations_.clear();
   return ++address_generation_;
}

reachability_state::candidate_snapshot reachability_state::candidates() const {
   const auto lock = std::scoped_lock{mutex_};
   return {address_generation_, addresses_};
}

void reachability_state::invalidate() noexcept {
   const auto lock = std::scoped_lock{mutex_};
   observations_.clear();
   addresses_.clear();
   if (address_generation_ == std::numeric_limits<std::uint64_t>::max()) {
      closed_ = true;
   } else {
      ++address_generation_;
   }
}

void reachability_state::close() noexcept {
   const auto lock = std::scoped_lock{mutex_};
   closed_ = true;
   observations_.clear();
   addresses_.clear();
}

void reachability_state::expire(std::chrono::steady_clock::time_point now) {
   const auto lock = std::scoped_lock{mutex_};
   std::erase_if(observations_, [&](const auto& entry) { return entry.expires_at <= now; });
}

reachability::state reachability_state::aggregate(const std::optional<endpoint>& address,
                                                  std::chrono::steady_clock::time_point now) const {
   auto votes = std::map<std::string, reachability::state>{};
   for (const auto& entry : observations_) {
      if (entry.expires_at <= now || !same_address(entry.address, address)) {
         continue;
      }
      // A verified v2 dialback proves this one address, not the node's v1 state.
      if (address && entry.value == reachability::state::publicly_reachable) {
         return reachability::state::publicly_reachable;
      }
      const auto [position, added] = votes.try_emplace(entry.group, entry.value);
      if (!added && position->second != entry.value) {
         position->second = reachability::state::unknown;
      }
   }
   std::size_t positive = 0;
   std::size_t negative = 0;
   for (const auto& [_, vote] : votes) {
      positive += vote == reachability::state::publicly_reachable;
      negative += vote == reachability::state::private_network;
   }
   if (positive >= policy_.min_reachability_observers && positive > negative) {
      return reachability::state::publicly_reachable;
   }
   if (negative >= policy_.min_reachability_observers && negative > positive) {
      return reachability::state::private_network;
   }
   return reachability::state::unknown;
}

host_event reachability_state::snapshot(std::chrono::steady_clock::time_point now) const {
   const auto lock = std::scoped_lock{mutex_};
   auto result = host_event{};
   result.autonat_v1 = aggregate(std::nullopt, now);
   result.effective = result.autonat_v1;
   auto addresses = std::map<std::string, endpoint>{};
   for (const auto& entry : observations_) {
      if (entry.address && entry.expires_at > now) {
         addresses.try_emplace(entry.address->to_string(), *entry.address);
      }
   }
   for (const auto& [_, address] : addresses) {
      const auto value = aggregate(address, now);
      result.autonat_v2.push_back({address, value});
      if (value == reachability::state::publicly_reachable &&
          host_addresses::classify_endpoint_scope(address) == host_addresses::endpoint_scope::public_address) {
         result.effective = reachability::state::publicly_reachable;
      }
   }
   return result;
}

} // namespace forge::net::p2p::detail
