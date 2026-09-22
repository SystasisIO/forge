module;

#include <boost/asio/awaitable.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module forge.net.p2p.peer_store;

import forge.asio.gate;
import forge.net.p2p.exceptions;
import forge.net.p2p.identity;
import forge.net.p2p.scoring;

#include "details/peer_store_impl.hxx"

namespace forge::net::p2p {

namespace {

[[nodiscard]] bool same_address(const forge::multiformats::multiaddr& left,
                                const forge::multiformats::multiaddr& right) {
   return left.to_string() == right.to_string();
}

[[nodiscard]] bool has_endpoint_source(const peer_store::endpoint_sources& sources) noexcept {
   return sources.learned || sources.identify_unsigned || sources.identify_signed;
}

void refresh_endpoint_score(peer_store::endpoint_record& endpoint) {
   endpoint.score = score_path(path::observation{
       .kind = endpoint.kind,
       .latency = endpoint.last_latency,
       .failures = endpoint.failures,
       .successes = endpoint.successes,
       .last_success = endpoint.successes > 0 && endpoint.failures == 0,
   });
}

void refresh_record_score(peer_store::record& record, path::kind kind, bool last_success) {
   record.score = score_path(path::observation{
       .kind = kind,
       .latency = record.last_latency,
       .failures = record.failures,
       .successes = record.successes,
       .last_success = last_success,
   });
}

void normalize_for_storage(peer_store::record& value) {
   if (value.discovered_by == discovery::source::mdns) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "mDNS observations cannot be persisted");
   }
   for (auto& endpoint : value.endpoints) {
      refresh_endpoint_score(endpoint);
   }
   const auto kind = value.endpoints.empty() ? path::kind::direct : value.endpoints.front().kind;
   refresh_record_score(value, kind, value.successes > 0);
}

void add_peer_record_bytes(std::size_t& total, std::size_t size, std::size_t maximum) {
   if (size > maximum - total) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer record exceeds byte limit");
   }
   total += size;
}

void validate_peer_record(const peer_store::record& value, const peer_store::options& options) {
   if (value.endpoints.size() > options.max_endpoints_per_peer ||
       value.protocols.size() > options.max_protocols_per_peer ||
       value.relay_reservations.size() > options.max_relay_reservations_per_peer) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer record exceeds collection limit");
   }

   auto bytes = std::size_t{};
   const auto add = [&](std::size_t size) { add_peer_record_bytes(bytes, size, options.max_peer_record_bytes); };
   add(value.protocol_version.size());
   add(value.agent_version.size());
   add(value.public_key.size());
   add(value.signed_peer_record.size());
   for (const auto& protocol : value.protocols) {
      add(protocol.value.size());
   }
   for (const auto& endpoint : value.endpoints) {
      if (!has_endpoint_source(endpoint.sources)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P endpoint record has no provenance");
      }
      add(endpoint.address.to_string().size());
   }
   if (value.observed_endpoint) {
      add(value.observed_endpoint->to_string().size());
   }
   for (const auto& relay : value.relay_reservations) {
      if (relay.endpoints.size() > options.max_relay_endpoints_per_reservation) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer relay reservation exceeds endpoint limit");
      }
      add(relay.voucher.size());
      for (const auto& endpoint : relay.endpoints) {
         add(endpoint.to_string().size());
      }
   }
   if (!value.public_key.empty()) {
      validate_public_key(decode_public_key(value.public_key), value.peer);
   }
}

void mutate_address(peer_store::record& record, const forge::multiformats::multiaddr& address, path::kind kind,
                     const std::function<void(peer_store::endpoint_record&)>& mutation) {
   auto iterator = std::ranges::find_if(record.endpoints,
                                        [&](const auto& current) { return same_address(current.address, address); });
   if (iterator == record.endpoints.end()) {
      iterator = record.endpoints.insert(record.endpoints.end(),
                                         peer_store::endpoint_record{.address = address, .kind = kind});
   }
   iterator->kind = kind;
   mutation(*iterator);
   refresh_endpoint_score(*iterator);
}

void replace_identify_address_snapshot(peer_store::record& record,
                                       const std::vector<forge::multiformats::multiaddr>& addresses,
                                       bool signed_snapshot) {
   for (auto& current : record.endpoints) {
      if (signed_snapshot) {
         current.sources.identify_signed = false;
      } else {
         current.sources.identify_unsigned = false;
      }
   }
   std::erase_if(record.endpoints, [](const auto& current) { return !has_endpoint_source(current.sources); });

   for (const auto& address : addresses) {
      const auto existing = std::ranges::find_if(
          record.endpoints, [&](const auto& current) { return same_address(current.address, address); });
      if (existing == record.endpoints.end()) {
         auto sources = peer_store::endpoint_sources{.learned = false};
         sources.identify_signed = signed_snapshot;
         sources.identify_unsigned = !signed_snapshot;
         record.endpoints.push_back(peer_store::endpoint_record{
             .address = address,
             .kind = path::kind::direct,
             .sources = sources,
         });
      } else if (signed_snapshot) {
         existing->sources.identify_signed = true;
      } else {
         existing->sources.identify_unsigned = true;
      }
   }
}

} // namespace

void peer_store::impl::add_peer_indexes(const peer_store::record& value) {
   const auto score = score_key{-value.score, value.peer};
   score_index_.insert(score);
   candidates_by_source_[value.discovered_by].insert(score);
   if (value.discovery_expires_at != std::chrono::system_clock::time_point{}) {
      peer_expiry_index_.emplace(value.discovery_expires_at, value.peer);
   }
   for (auto bit = std::uint64_t{1}; bit != 0; bit <<= 1U) {
      if (value.capabilities.has(bit)) {
         candidates_by_capability_[bit].insert(score);
      }
   }
}

void peer_store::impl::remove_peer_indexes(const peer_store::record& value) {
   const auto score = score_key{-value.score, value.peer};
   score_index_.erase(score);
   if (const auto source = candidates_by_source_.find(value.discovered_by); source != candidates_by_source_.end()) {
      source->second.erase(score);
      if (source->second.empty()) {
         candidates_by_source_.erase(source);
      }
   }
   if (value.discovery_expires_at != std::chrono::system_clock::time_point{}) {
      peer_expiry_index_.erase({value.discovery_expires_at, value.peer});
   }
   for (auto bit = std::uint64_t{1}; bit != 0; bit <<= 1U) {
      if (!value.capabilities.has(bit)) {
         continue;
      }
      const auto index = candidates_by_capability_.find(bit);
      if (index == candidates_by_capability_.end()) {
         continue;
      }
      index->second.erase(score);
      if (index->second.empty()) {
         candidates_by_capability_.erase(index);
      }
   }
}

void peer_store::impl::erase_peer_operational(const peer_id& peer) {
   const auto current = records_.find(peer);
   if (current == records_.end()) {
      return;
   }
   remove_peer_indexes(current->second);
   records_.erase(current);
}

std::optional<peer_id> peer_store::impl::store_peer_operational(peer_store::record value) {
   const auto peer = value.peer;
   const auto current = records_.find(value.peer);
   if (current != records_.end()) {
      remove_peer_indexes(current->second);
   }
   records_.insert_or_assign(peer, std::move(value));
   add_peer_indexes(records_.at(peer));

   if (records_.size() <= options_.max_peers) {
      return std::nullopt;
   }
   const auto lowest_score = score_index_.rbegin();
   auto evicted = lowest_score->second;
   erase_peer_operational(evicted);
   return evicted;
}

peer_store::record peer_store::impl::record_for_mutation_locked(const peer_id& peer) const {
   const auto current = records_.find(peer);
   if (current != records_.end()) {
      return current->second;
   }
   return peer_store::record{.peer = peer};
}

void peer_store::impl::commit_peer_mutation(peer_store::record value) {
   auto lock = std::scoped_lock{mutex_};
   ensure_open_locked();
   commit_peer_mutation_locked(std::move(value));
}

void peer_store::impl::commit_peer_mutation_locked(peer_store::record value) {
   validate_peer_record(value, options_);
   auto evicted = std::optional<peer_id>{};
   if (!records_.contains(value.peer) && records_.size() == options_.max_peers) {
      const auto candidate = score_key{-value.score, value.peer};
      const auto current_lowest = *score_index_.rbegin();
      evicted = candidate > current_lowest ? value.peer : current_lowest.second;
   }

   auto affected = std::vector<peer_id>{value.peer};
   if (evicted) {
      affected.push_back(*evicted);
   }
   ensure_peer_mutation_capacity_locked(affected);

   const auto peer = value.peer;
   auto updates = std::map<peer_id, peer_mutation>{};
   if (!evicted || *evicted != peer) {
      updates.emplace(peer, peer_mutation{.value = value});
   }
   if (evicted) {
      updates.insert_or_assign(*evicted, peer_mutation{.value = std::nullopt});
   }
   auto stage = peer_mutation_stage{this, std::move(updates)};
   static_cast<void>(store_peer_operational(std::move(value)));
   stage.commit();
}

peer_store::record peer_store::impl::mutate_peer(const peer_id& peer,
                                                 const std::function<void(peer_store::record&)>& mutation) {
   auto lock = std::scoped_lock{mutex_};
   ensure_open_locked();
   auto value = record_for_mutation_locked(peer);
   mutation(value);
   value.peer = peer;
   normalize_for_storage(value);
   auto result = value;
   commit_peer_mutation_locked(std::move(value));
   return result;
}

void peer_store::impl::upsert(peer_store::record value) {
   normalize_for_storage(value);
   commit_peer_mutation(std::move(value));
}

peer_store::record peer_store::impl::apply_identify(const peer_id& peer, peer_store::identify_update update) {
   if (update.signed_endpoints.has_value() != update.signed_peer_record.has_value() ||
       (update.signed_endpoints && update.unsigned_endpoints)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P Identify update has inconsistent endpoint sources");
   }

   return mutate_peer(peer, [&](peer_store::record& value) {
      if (update.protocol_version) {
         value.protocol_version = std::move(*update.protocol_version);
      }
      if (update.agent_version) {
         value.agent_version = std::move(*update.agent_version);
      }
      if (update.public_key) {
         value.public_key = std::move(*update.public_key);
      }
      if (update.protocols) {
         value.protocols = std::move(*update.protocols);
      }
      if (update.capabilities) {
         value.capabilities = *update.capabilities;
      }
      if (update.replace_observed_endpoint) {
         value.observed_endpoint = std::move(update.observed_endpoint);
      }
      if (update.signed_endpoints) {
         value.signed_peer_record = std::move(*update.signed_peer_record);
         replace_identify_address_snapshot(value, *update.signed_endpoints, true);
      } else if (update.unsigned_endpoints) {
         replace_identify_address_snapshot(value, *update.unsigned_endpoints, false);
      }
   });
}

std::optional<peer_store::record> peer_store::impl::apply_discovery(const peer_id& peer,
                                                                    peer_store::discovery_update update) {
   auto lock = std::scoped_lock{mutex_};
   ensure_open_locked();
   const auto current = records_.find(peer);
   if (current == records_.end()) {
      return std::nullopt;
   }
   auto value = current->second;
   value.discovered_by = update.source;
   value.discovered_at = update.observed_at;
   value.discovery_expires_at = update.expires_at;
   normalize_for_storage(value);
   auto result = value;
   commit_peer_mutation_locked(std::move(value));
   return result;
}

void peer_store::impl::apply_peer_exchange(const peer_id& peer, capability_set capabilities) {
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& value) { value.capabilities.bits |= capabilities.bits; }));
}

void peer_store::impl::upsert_relay_reservation(peer_store::relay_record value) {
   const auto peer = value.relay;
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& record) {
      const auto current = std::ranges::find_if(record.relay_reservations,
                                                [&](const auto& reservation) { return reservation.relay == peer; });
      if (current == record.relay_reservations.end()) {
         record.relay_reservations.push_back(std::move(value));
      } else {
         *current = std::move(value);
      }
      record.capabilities.add(capabilities::relay);
      record.capabilities.add(capabilities::relay_reservation);
   }));
}

bool peer_store::impl::mark_discovery_failure(const peer_id& peer,
                                              std::chrono::system_clock::time_point backoff_until) {
   auto lock = std::scoped_lock{mutex_};
   ensure_open_locked();
   const auto current = records_.find(peer);
   if (current == records_.end()) {
      return false;
   }
   auto value = current->second;
   value.discovery_backoff_until = backoff_until;
   ++value.failures;
   normalize_for_storage(value);
   commit_peer_mutation_locked(std::move(value));
   return true;
}

std::size_t peer_store::impl::prune_expired_relay_reservations(const peer_id& peer,
                                                               std::chrono::system_clock::time_point now) {
   auto lock = std::scoped_lock{mutex_};
   ensure_open_locked();
   const auto current = records_.find(peer);
   if (current == records_.end()) {
      return 0;
   }
   auto value = current->second;
   const auto before = value.relay_reservations.size();
   std::erase_if(value.relay_reservations, [&](const peer_store::relay_record& reservation) {
      return reservation.expires_at != std::chrono::system_clock::time_point{} && reservation.expires_at <= now;
   });
   const auto removed = before - value.relay_reservations.size();
   if (removed == 0) {
      return 0;
   }
   normalize_for_storage(value);
   commit_peer_mutation_locked(std::move(value));
   return removed;
}

void peer_store::impl::learn_address(peer_id peer, forge::multiformats::multiaddr address,
                                     capability_set capabilities) {
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& value) {
      value.peer = peer;
      value.capabilities.bits |= capabilities.bits;
      const auto existing = std::ranges::find_if(
          value.endpoints, [&](const auto& current) { return same_address(current.address, address); });
      if (existing == value.endpoints.end()) {
         value.endpoints.push_back(peer_store::endpoint_record{.address = std::move(address)});
      } else {
         existing->sources.learned = true;
      }
   }));
}

void peer_store::impl::mark_reachability(peer_id peer, reachability::state state,
                                         std::optional<forge::net::p2p::endpoint> observed) {
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& value) {
      value.peer = peer;
      value.reachability = state;
      value.observed_endpoint = std::move(observed);
      value.reachability_expires_at = std::chrono::system_clock::now() + std::chrono::minutes{5};
   }));
}

void peer_store::impl::mark_success(const peer_id& peer, path::kind kind, std::chrono::milliseconds latency) {
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& value) {
      ++value.successes;
      value.last_latency = latency;
      refresh_record_score(value, kind, true);
   }));
}

void peer_store::impl::mark_failure(const peer_id& peer) {
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& value) {
      ++value.failures;
      const auto kind = value.endpoints.empty() ? path::kind::direct : value.endpoints.front().kind;
      refresh_record_score(value, kind, false);
   }));
}

void peer_store::impl::mark_address_success(const peer_id& peer, const forge::multiformats::multiaddr& address,
                                            path::kind kind, std::chrono::milliseconds latency) {
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& value) {
      mutate_address(value, address, kind, [&](peer_store::endpoint_record& current) {
         current.sources.learned = true;
         current.last_latency = latency;
         current.backoff_until = {};
         ++current.successes;
      });
      ++value.successes;
      value.last_latency = latency;
      refresh_record_score(value, kind, true);
   }));
}

void peer_store::impl::mark_address_failure(const peer_id& peer, const forge::multiformats::multiaddr& address,
                                            path::kind kind, std::chrono::system_clock::time_point backoff_until) {
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& value) {
      mutate_address(value, address, kind, [&](peer_store::endpoint_record& current) {
         current.backoff_until = backoff_until;
         ++current.failures;
      });
      ++value.failures;
      refresh_record_score(value, kind, false);
   }));
}

void peer_store::impl::upsert_routing_peer(protocol_id protocol, dht::peer value, discovery::source source,
                                           std::chrono::system_clock::time_point expires_at) {
   const auto peer = value.id;
   static_cast<void>(mutate_peer(peer, [&](peer_store::record& record) {
      record.peer = peer;
      if (std::ranges::find(record.protocols, protocol) == record.protocols.end()) {
         record.protocols.push_back(std::move(protocol));
      }
      record.discovered_by = source;
      record.discovered_at = std::chrono::system_clock::now();
      record.discovery_expires_at = expires_at;
      for (auto address : value.endpoints) {
         const auto existing = std::ranges::find_if(
             record.endpoints, [&](const auto& current) { return same_address(current.address, address); });
         if (existing == record.endpoints.end()) {
            record.endpoints.push_back(peer_store::endpoint_record{.address = std::move(address)});
         } else {
            existing->sources.learned = true;
         }
      }
   }));
}

} // namespace forge::net::p2p
