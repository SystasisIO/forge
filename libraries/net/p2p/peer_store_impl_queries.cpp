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
#include <span>
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

void expire_reachability(peer_store::record& record, std::chrono::system_clock::time_point now) {
   if (record.reachability_expires_at == std::chrono::system_clock::time_point{} ||
       record.reachability_expires_at > now) {
      return;
   }
   record.reachability = reachability::state::unknown;
   record.observed_endpoint.reset();
   record.reachability_expires_at = {};
}

void add_peer_record_bytes(std::size_t& total, std::size_t size, std::size_t maximum) {
   if (size > maximum - total) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P peer record exceeds byte limit");
   }
   total += size;
}

void validate_rendezvous_record(const rendezvous::registration& value, const peer_store::options& options) {
   if (value.endpoints.size() > options.max_endpoints_per_peer) {
      FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "P2P Rendezvous record exceeds endpoint limit");
   }

   auto bytes = std::size_t{};
   const auto add = [&](std::size_t size) { add_peer_record_bytes(bytes, size, options.max_peer_record_bytes); };
   add(value.namespace_name.size());
   add(value.peer.value.size());
   add(value.signed_peer_record.size());
   for (const auto& endpoint : value.endpoints) {
      add(endpoint.to_string().size());
   }
}

[[nodiscard]] std::string current_failure_message() {
   try {
      throw;
   } catch (const std::exception& error) {
      return error.what();
   } catch (...) {
      return "unknown persistence failure";
   }
}

[[nodiscard]] std::string durability_failure_message(const peer_store::apply_result& result) {
   return result.durability_failure.empty() ? "persistence commit completed without durable acknowledgement"
                                            : result.durability_failure;
}

[[noreturn]] void throw_durability_uncertain(const peer_store::apply_result& result) {
   FORGE_THROW_EXCEPTION(exceptions::durability_uncertain, "peer state durability could not be confirmed",
                         forge::exceptions::ctx("reason", durability_failure_message(result)));
}

} // namespace

void peer_store::impl::store_rendezvous_operational(rendezvous::registration value) {
   const auto key = rendezvous_map_key{value.namespace_name, value.peer};
   erase_rendezvous_operational(key);
   rendezvous_by_sequence_.emplace(rendezvous_sequence_key{value.namespace_name, value.sequence, value.peer}, key);
   rendezvous_by_global_sequence_.emplace(rendezvous_global_sequence_key{value.sequence, key}, key);
   rendezvous_expiry_index_.emplace(value.expires_at, key);
   ++rendezvous_per_peer_[value.peer];
   rendezvous_.emplace(key, std::move(value));
}

void peer_store::impl::erase_rendezvous_operational(const rendezvous_map_key& key) {
   const auto current = rendezvous_.find(key);
   if (current == rendezvous_.end()) {
      return;
   }
   rendezvous_by_sequence_.erase(
       rendezvous_sequence_key{current->second.namespace_name, current->second.sequence, current->second.peer});
   rendezvous_by_global_sequence_.erase(rendezvous_global_sequence_key{current->second.sequence, key});
   rendezvous_expiry_index_.erase({current->second.expires_at, key});
   const auto count = rendezvous_per_peer_.find(current->second.peer);
   if (count != rendezvous_per_peer_.end() && --count->second == 0) {
      rendezvous_per_peer_.erase(count);
   }
   rendezvous_.erase(current);
}

boost::asio::awaitable<void> peer_store::impl::async_upsert_rendezvous(rendezvous::registration value) {
   co_await async_store_rendezvous(std::move(value), std::nullopt);
}

boost::asio::awaitable<void> peer_store::impl::async_register_rendezvous(rendezvous::registration value,
                                                                         std::size_t max_registrations_per_peer) {
   if (max_registrations_per_peer == 0) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "peer store Rendezvous per-peer capacity must be positive");
   }
   co_await async_store_rendezvous(std::move(value), max_registrations_per_peer);
}

boost::asio::awaitable<void>
peer_store::impl::async_store_rendezvous(rendezvous::registration value,
                                         std::optional<std::size_t> max_registrations_per_peer) {
   auto admission = admit_persistence_operation();
   auto ticket = co_await persistence_gate_.acquire();
   validate_rendezvous_record(value, options_);
   const auto key = rendezvous_map_key{value.namespace_name, value.peer};
   {
      auto lock = std::scoped_lock{mutex_};
      if (!rendezvous_.contains(key) && rendezvous_.size() >= options_.max_rendezvous) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "peer store Rendezvous capacity reached");
      }
      const auto existing = rendezvous_.contains(key);
      const auto count = rendezvous_per_peer_.find(value.peer);
      const auto registrations = count == rendezvous_per_peer_.end() ? 0U : count->second;
      if (!existing && max_registrations_per_peer && registrations >= *max_registrations_per_peer) {
         FORGE_THROW_EXCEPTION(exceptions::backpressure_rejected, "peer store Rendezvous per-peer capacity reached");
      }
      if (rendezvous_sequence_ == std::numeric_limits<std::uint64_t>::max()) {
         FORGE_THROW_EXCEPTION(exceptions::sequence_exhausted, "peer store Rendezvous sequence is exhausted");
      }
      value.sequence = ++rendezvous_sequence_;
   }

   auto batch = peer_store::mutation_batch{};
   batch.rendezvous_upserts.push_back(value);
   batch.rendezvous_sequence_high_watermark = value.sequence;
   batch.durable = true;
   auto result = peer_store::apply_result{};
   try {
      result = co_await persistence_->async_apply(std::move(batch));
   } catch (...) {
      auto lock = std::scoped_lock{mutex_};
      mark_persistence_failure_locked(current_failure_message());
      throw;
   }

   {
      auto lock = std::scoped_lock{mutex_};
      store_rendezvous_operational(std::move(value));
      if (result.durability_confirmed) {
         mark_persistence_healthy_locked(true);
      } else {
         mark_durability_uncertain_locked(durability_failure_message(result));
      }
   }
   if (!result.durability_confirmed) {
      throw_durability_uncertain(result);
   }
}

boost::asio::awaitable<void> peer_store::impl::async_remove_rendezvous(peer_id peer, std::string namespace_name) {
   auto admission = admit_persistence_operation();
   auto ticket = co_await persistence_gate_.acquire();
   validate_rendezvous_record(rendezvous::registration{.namespace_name = namespace_name, .peer = peer}, options_);

   auto batch = peer_store::mutation_batch{};
   batch.rendezvous_removals.push_back(peer_store::rendezvous_key{.namespace_name = namespace_name, .peer = peer});
   batch.durable = true;
   auto result = peer_store::apply_result{};
   try {
      result = co_await persistence_->async_apply(std::move(batch));
   } catch (...) {
      auto lock = std::scoped_lock{mutex_};
      mark_persistence_failure_locked(current_failure_message());
      throw;
   }

   {
      auto lock = std::scoped_lock{mutex_};
      erase_rendezvous_operational({std::move(namespace_name), std::move(peer)});
      if (result.durability_confirmed) {
         mark_persistence_healthy_locked(true);
      } else {
         mark_durability_uncertain_locked(durability_failure_message(result));
      }
   }
   if (!result.durability_confirmed) {
      throw_durability_uncertain(result);
   }
}

std::optional<peer_store::record> peer_store::impl::find(const peer_id& peer) const {
   auto lock = std::scoped_lock{mutex_};
   const auto iterator = records_.find(peer);
   if (iterator == records_.end()) {
      return std::nullopt;
   }
   auto value = iterator->second;
   expire_reachability(value, std::chrono::system_clock::now());
   return value;
}

std::optional<public_key> peer_store::impl::find_public_key(const peer_id& peer) const {
   auto lock = std::scoped_lock{mutex_};
   const auto current = records_.find(peer);
   if (current == records_.end() || current->second.public_key.empty()) {
      return std::nullopt;
   }
   return decode_public_key(current->second.public_key);
}

std::vector<peer_store::record> peer_store::impl::snapshot(std::size_t limit) const {
   auto lock = std::scoped_lock{mutex_};
   auto result = std::vector<peer_store::record>{};
   result.reserve(std::min(limit, records_.size()));
   const auto now = std::chrono::system_clock::now();
   for (const auto& [_, value] : records_) {
      if (result.size() == limit) {
         break;
      }
      auto copy = value;
      expire_reachability(copy, now);
      result.push_back(std::move(copy));
   }
   return result;
}

std::vector<peer_store::record> peer_store::impl::candidates(std::uint64_t capability, std::size_t limit,
                                                          std::span<const peer_id> excluded_peers) const {
   auto lock = std::scoped_lock{mutex_};
   auto result = std::vector<peer_store::record>{};
   if (capability == 0 || limit == 0) {
      return result;
   }

   const auto index_capability = capability & (~capability + 1U);
   const auto index = candidates_by_capability_.find(index_capability);
   if (index == candidates_by_capability_.end()) {
      return result;
   }

   result.reserve(std::min(limit, index->second.size()));
   const auto excluded = std::set<peer_id>{excluded_peers.begin(), excluded_peers.end()};
   const auto now = std::chrono::system_clock::now();
   for (const auto& [_, peer] : index->second) {
      if (result.size() == limit) {
         break;
      }
      if (excluded.contains(peer)) { continue; }
      const auto record = records_.find(peer);
      if (record == records_.end() || !record->second.capabilities.has(capability)) {
         continue;
      }
      if (record->second.discovery_expires_at != std::chrono::system_clock::time_point{} &&
          record->second.discovery_expires_at <= now) {
         continue;
      }
      auto copy = record->second;
      expire_reachability(copy, now);
      result.push_back(std::move(copy));
   }
   return result;
}

std::vector<peer_store::record> peer_store::impl::scored_candidates(std::size_t limit) const {
   auto lock = std::scoped_lock{mutex_};
   auto result = std::vector<peer_store::record>{};
   if (limit == 0) {
      return result;
   }

   result.reserve(std::min(limit, score_index_.size()));
   const auto now = std::chrono::system_clock::now();
   for (auto current = score_index_.begin(); current != score_index_.end() && result.size() < limit; ++current) {
      const auto record = records_.find(current->second);
      if (record == records_.end() ||
          (record->second.discovery_expires_at != std::chrono::system_clock::time_point{} &&
           record->second.discovery_expires_at <= now)) {
         continue;
      }
      auto copy = record->second;
      expire_reachability(copy, now);
      result.push_back(std::move(copy));
   }
   return result;
}

std::vector<peer_store::record> peer_store::impl::scored_candidates(discovery::source source, std::size_t limit) const {
   auto lock = std::scoped_lock{mutex_};
   auto result = std::vector<peer_store::record>{};
   if (limit == 0) {
      return result;
   }

   const auto index = candidates_by_source_.find(source);
   if (index == candidates_by_source_.end()) {
      return result;
   }

   result.reserve(std::min(limit, index->second.size()));
   const auto now = std::chrono::system_clock::now();
   for (auto current = index->second.begin(); current != index->second.end() && result.size() < limit; ++current) {
      const auto record = records_.find(current->second);
      if (record == records_.end() || record->second.discovered_by != source ||
          (record->second.discovery_expires_at != std::chrono::system_clock::time_point{} &&
           record->second.discovery_expires_at <= now)) {
         continue;
      }
      auto copy = record->second;
      expire_reachability(copy, now);
      result.push_back(std::move(copy));
   }
   return result;
}

std::vector<rendezvous::registration> peer_store::impl::discover_rendezvous(std::string_view namespace_name,
                                                                            std::uint64_t after_sequence,
                                                                            std::size_t limit) const {
   auto lock = std::scoped_lock{mutex_};
   auto result = std::vector<rendezvous::registration>{};
   const auto now = std::chrono::system_clock::now();
   if (namespace_name.empty()) {
      for (auto iterator = rendezvous_by_global_sequence_.lower_bound({after_sequence, {}});
           iterator != rendezvous_by_global_sequence_.end() && result.size() < limit; ++iterator) {
         if (iterator->first.first <= after_sequence) {
            continue;
         }
         const auto value = rendezvous_.find(iterator->second);
         if (value != rendezvous_.end() && value->second.expires_at > now) {
            result.push_back(value->second);
         }
      }
      return result;
   }

   for (auto iterator = rendezvous_by_sequence_.lower_bound(
            rendezvous_sequence_key{std::string{namespace_name}, after_sequence, peer_id{}});
        iterator != rendezvous_by_sequence_.end() && result.size() < limit; ++iterator) {
      const auto& [current_namespace, sequence, _] = iterator->first;
      if (current_namespace != namespace_name) {
         break;
      }
      if (sequence <= after_sequence) {
         continue;
      }
      const auto value = rendezvous_.find(iterator->second);
      if (value != rendezvous_.end() && value->second.expires_at > now) {
         result.push_back(value->second);
      }
   }
   return result;
}

} // namespace forge::net::p2p
