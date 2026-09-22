module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio/ip/address.hpp>

module forge.net.p2p.node;

import forge.net.p2p.endpoint;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.multiformats.multiaddr;

#include "details/host_addresses.hxx"
#include "details/observed_address_manager.hxx"

namespace forge::net::p2p::detail {
namespace {

[[nodiscard]] bool is_nat64(const boost::asio::ip::address& address) {
   if (!address.is_v6()) {
      return false;
   }
   const auto bytes = address.to_v6().to_bytes();
   const auto prefix = bytes[0] == 0 && bytes[1] == 0x64 && bytes[2] == 0xff && bytes[3] == 0x9b;
   const auto well_known = prefix && std::all_of(bytes.begin() + 4, bytes.begin() + 12,
                                               [](auto value) { return value == 0; });
   return well_known || (prefix && bytes[4] == 0 && bytes[5] == 1);
}

[[nodiscard]] std::optional<endpoint> normalized(const endpoint& value, bool listener = false) {
   if ((!value.is_direct_tcp() && !value.is_direct_quic()) || value.transport.port == 0 ||
       (value.transport.host_type != endpoint::host_kind::ip4 && value.transport.host_type != endpoint::host_kind::ip6)) {
      return std::nullopt;
   }
   auto error = boost::system::error_code{};
   const auto address = boost::asio::ip::make_address(value.transport.host, error);
   if (error || address.is_v4() != (value.transport.host_type == endpoint::host_kind::ip4) || is_nat64(address) ||
       (address.is_v6() && address.to_v6().scope_id() != 0)) {
      return std::nullopt;
   }
   const auto scope = host_addresses::classify_endpoint_scope(value);
   if (scope != host_addresses::endpoint_scope::public_address &&
       scope != host_addresses::endpoint_scope::private_address && !(listener && address.is_unspecified())) {
      return std::nullopt;
   }
   return endpoint{.transport = {.host_type = value.transport.host_type, .protocol = value.transport.protocol,
                                 .host = address.to_string(), .port = value.transport.port,
                                 .zone = value.transport.zone}};
}

[[nodiscard]] bool same_transport(const endpoint& left, const endpoint& right) {
   return left.transport.host_type == right.transport.host_type && left.transport.protocol == right.transport.protocol;
}

} // namespace

observed_address_manager::observed_address_manager() : observed_address_manager(options{}) {}

observed_address_manager::observed_address_manager(options value) : options_(value) {
   if (value.max_observations == 0 || value.max_candidates == 0 || value.min_observers == 0 ||
       value.min_observers > value.max_observations || value.max_confirmed_per_local == 0 ||
       value.ttl <= std::chrono::steady_clock::duration::zero()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "invalid P2P observed-address bounds");
   }
   observations_.reserve(value.max_observations);
}

bool observed_address_manager::observe(std::uint64_t session_id, const peer_id& authenticated_peer,
                                       const endpoint& local, const endpoint& remote, const endpoint& reported,
                                       std::span<const endpoint> listened, time_point now) {
   if (!valid_peer_id(authenticated_peer) || (remote.peer && *remote.peer != authenticated_peer) ||
       (local.peer && reported.peer && *local.peer != *reported.peer)) {
      return false;
   }
   // QUIC sessions can retain the shared socket's wildcard bind. Keep that
   // actual bind as the key; the listener check below must still establish ownership.
   auto local_address = normalized(local, local.is_direct_quic());
   auto remote_address = normalized(remote);
   auto reported_address = normalized(reported);
   if (!local_address || !remote_address || !reported_address || !same_transport(*local_address, *reported_address) ||
       !same_transport(*local_address, *remote_address)) {
      return false;
   }
   auto observer = host_addresses::observer_group(*remote_address);
   if (!observer) {
      return false;
   }
   const auto is_listened = std::ranges::any_of(listened, [&](const auto& value) {
      const auto listener = normalized(value, true);
      return listener && same_transport(*local_address, *listener) &&
             local_address->transport.zone == listener->transport.zone &&
             local_address->transport.port == listener->transport.port &&
             (local_address->transport.host == listener->transport.host ||
              boost::asio::ip::make_address(listener->transport.host).is_unspecified());
   });
   if (!is_listened) {
      return false;
   }
   const auto expires_at = now > time_point::max() - options_.ttl ? time_point::max() : now + options_.ttl;
   auto incoming = observation{
       .session_id = session_id,
       .peer = peer_id::from_string(authenticated_peer.value).to_string(),
       .observer = std::move(*observer),
       .key = {local_address->to_multiaddr().to_bytes(), reported_address->to_multiaddr().to_bytes()},
       .reported = std::move(*reported_address), .expires_at = expires_at};

   const auto lock = std::scoped_lock{mutex_};
   expire_locked(now);
   const auto previous = std::ranges::find(observations_, session_id, &observation::session_id);
   if (previous == observations_.end() && observations_.size() == options_.max_observations) {
      return false;
   }
   auto candidates = std::set<candidate_key>{};
   for (const auto& item : observations_) {
      if (item.session_id != session_id) {
         candidates.insert(item.key);
      }
   }
   candidates.insert(incoming.key);
   if (candidates.size() > options_.max_candidates) {
      return false;
   }
   // All allocating preparation precedes replacement of the old vote.
   static_assert(std::is_nothrow_move_assignable_v<observation>);
   static_assert(std::is_nothrow_move_constructible_v<observation>);
   if (previous == observations_.end()) {
      observations_.push_back(std::move(incoming));
   } else {
      *previous = std::move(incoming);
   }
   return true;
}

void observed_address_manager::remove(std::uint64_t session_id) {
   const auto lock = std::scoped_lock{mutex_};
   std::erase_if(observations_, [&](const auto& value) { return value.session_id == session_id; });
}

void observed_address_manager::expire_locked(time_point now) {
   std::erase_if(observations_, [&](const auto& value) { return value.expires_at <= now; });
}

void observed_address_manager::expire(time_point now) {
   const auto lock = std::scoped_lock{mutex_};
   expire_locked(now);
}

std::size_t observed_address_manager::count_observers(const std::vector<const observation*>& values) {
   auto edges = std::map<std::string, std::set<std::string>>{};
   for (const auto* value : values) {
      edges[value->peer].insert(value->observer);
   }
   // Maximum matching enforces both peer and IP/prefix independence. Taking
   // min(unique peers, unique prefixes) can manufacture a quorum from aliases.
   auto owners = std::map<std::string, const std::string*>{};
   const auto assign = [&](auto&& self, const std::string& peer, std::set<std::string>& visited) -> bool {
      for (const auto& observer : edges.at(peer)) {
         if (!visited.insert(observer).second) {
            continue;
         }
         const auto previous = owners.find(observer);
         if (previous == owners.end() || self(self, *previous->second, visited)) {
            owners.insert_or_assign(observer, &peer);
            return true;
         }
      }
      return false;
   };
   auto count = std::size_t{};
   for (const auto& [peer, observers] : edges) {
      auto visited = std::set<std::string>{};
      if (assign(assign, peer, visited)) {
         ++count;
      }
   }
   return count;
}

std::vector<endpoint> observed_address_manager::confirmed(time_point now) const {
   const auto lock = std::scoped_lock{mutex_};
   auto groups = std::map<candidate_key, std::vector<const observation*>>{};
   for (const auto& value : observations_) {
      if (value.expires_at > now) {
         groups[value.key].push_back(&value);
      }
   }
   auto ranked = std::vector<ranked_candidate>{};
   for (const auto& [key, values] : groups) {
      const auto observers = count_observers(values);
      if (observers >= options_.min_observers) {
         ranked.push_back({.key = &key, .address = &values.front()->reported, .observers = observers});
      }
   }
   std::ranges::sort(ranked, [](const auto& left, const auto& right) {
      if (left.key->first != right.key->first) {
         return left.key->first < right.key->first;
      }
      if (left.observers != right.observers) {
         return left.observers > right.observers;
      }
      return left.key->second < right.key->second;
   });
   auto result = std::vector<endpoint>{};
   auto emitted = std::set<address_key>{};
   const address_key* local = nullptr;
   auto selected = std::size_t{};
   for (const auto& item : ranked) {
      if (!local || *local != item.key->first) {
         local = &item.key->first;
         selected = 0;
      }
      if (selected == options_.max_confirmed_per_local) {
         continue;
      }
      ++selected;
      if (emitted.insert(item.key->second).second) {
         result.push_back(*item.address);
      }
   }
   return result;
}

} // namespace forge::net::p2p::detail
