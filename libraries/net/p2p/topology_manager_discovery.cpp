module;

#include "details/rendezvous_time.hxx"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/strand.hpp>

module forge.net.p2p.node;

import forge.exceptions;
import forge.asio.notification;
import forge.net.p2p.discovery;
import forge.net.p2p.exceptions;

#include "details/cancellation_latch.hxx"
#include "details/lifecycle_wakeup.hxx"
#include "details/topology_manager.hxx"

namespace forge::net::p2p::detail {
namespace {

[[nodiscard]] std::uint64_t observation_hash(const peer_id& peer, discovery::source source, std::size_t failures) {
   auto value = std::uint64_t{1469598103934665603ULL};
   for (const auto byte : peer.to_string()) {
      value ^= static_cast<std::uint8_t>(byte);
      value *= 1099511628211ULL;
   }
   value ^= static_cast<std::uint16_t>(source);
   value *= 1099511628211ULL;
   value ^= failures;
   return value;
}

[[nodiscard]] std::uint64_t rendezvous_hash(const peer_id& peer, std::string_view namespace_name,
                                            std::size_t failures) {
   auto value = observation_hash(peer, discovery::source::rendezvous, failures);
   for (const auto byte : namespace_name) {
      value ^= static_cast<std::uint8_t>(byte);
      value *= 1099511628211ULL;
   }
   return value;
}

[[nodiscard]] std::chrono::system_clock::time_point rendezvous_renewal_time(
    std::chrono::system_clock::time_point now, std::chrono::system_clock::time_point expires_at) {
   const auto lifetime = expires_at - now;
   if (lifetime <= std::chrono::system_clock::duration::zero()) {
      return now;
   }
   const auto maximum_margin = std::chrono::duration_cast<std::chrono::system_clock::duration>(std::chrono::minutes{1});
   auto margin = std::min(lifetime / 10, maximum_margin);
   if (margin <= std::chrono::system_clock::duration::zero()) {
      margin = lifetime / 2;
   }
   if (margin <= std::chrono::system_clock::duration::zero()) {
      return now;
   }
   return expires_at - margin;
}

[[nodiscard]] std::chrono::steady_clock::time_point
bounded_retry_deadline(std::chrono::steady_clock::time_point now, std::chrono::milliseconds delay) noexcept {
   if (delay <= std::chrono::milliseconds::zero() || now == std::chrono::steady_clock::time_point::max()) {
      return now;
   }
   const auto available = std::chrono::steady_clock::time_point::max() - now;
   const auto requested = std::chrono::duration_cast<std::chrono::steady_clock::duration>(delay);
   return requested >= available ? std::chrono::steady_clock::time_point::max() : now + requested;
}

void bound_source_results(std::vector<discovery::result>& results, std::size_t limit) {
   if (results.size() > limit) {
      results.resize(limit);
   }
   std::stable_sort(results.begin(), results.end(), [](const discovery::result& left, const discovery::result& right) {
      if (left.peer != right.peer) {
         return left.peer.to_string() < right.peer.to_string();
      }
      if (left.score != right.score) {
         return left.score > right.score;
      }
      if (left.expires_at != right.expires_at) {
         return left.expires_at < right.expires_at;
      }
      return static_cast<std::uint16_t>(left.discovered_by) < static_cast<std::uint16_t>(right.discovered_by);
   });
}

} // namespace

boost::asio::awaitable<std::vector<discovery::result>> topology_manager::async_collect_discovery() {
   auto cancellation = std::make_shared<cancellation_latch>();
   add_cancellation(cancellation);
   try {
      const auto dht_enabled = policy_.dht_enabled && static_cast<bool>(callbacks_.discover);
      const auto rendezvous_enabled = policy_.rendezvous_enabled && !rendezvous_clients_.empty() &&
                                      static_cast<bool>(callbacks_.local_rendezvous_record) &&
                                      static_cast<bool>(callbacks_.rendezvous_register) &&
                                      static_cast<bool>(callbacks_.rendezvous_discover);
      const auto peer_exchange_enabled = policy_.peer_exchange_enabled && static_cast<bool>(callbacks_.peer_exchange);
      auto dht = std::vector<discovery::result>{};
      auto rendezvous = std::vector<discovery::result>{};
      auto peer_exchange = std::vector<discovery::result>{};
      auto enabled_sources = std::size_t{};
      auto successful_sources = std::size_t{};
      auto first_failure = std::exception_ptr{};
      const auto remember_failure = [&first_failure](std::string_view message) {
         if (!first_failure) {
            first_failure = std::current_exception();
         }
         forge::exceptions::capture_and_log(message);
      };

      if (dht_enabled) {
         ++enabled_sources;
         try {
            // Each source receives the same sticky latch. Direct awaiting keeps
            // its caller cancellation intact without relaying a raw signal from
            // the topology stop thread into another executor.
            dht = co_await callbacks_.discover(cancellation);
            bound_source_results(dht, policy_.max_candidates);
            ++successful_sources;
         } catch (...) {
            if (!stopping()) {
               remember_failure("P2P topology DHT discovery failed");
            }
         }
      }

      if (!stopping() && rendezvous_enabled) {
         ++enabled_sources;
         try {
            rendezvous = co_await async_collect_rendezvous(cancellation, policy_.max_candidates);
            bound_source_results(rendezvous, policy_.max_candidates);
            ++successful_sources;
         } catch (...) {
            if (!stopping()) {
               remember_failure("P2P topology Rendezvous discovery failed");
            }
         }
      }

      if (!stopping() && peer_exchange_enabled) {
         ++enabled_sources;
         try {
            peer_exchange = co_await callbacks_.peer_exchange(
                cancellation, std::min(policy_.max_peer_exchange_peers, policy_.max_parallel_queries));
            bound_source_results(peer_exchange, policy_.max_candidates);
            ++successful_sources;
         } catch (...) {
            if (!stopping()) {
               remember_failure("P2P topology peer exchange discovery failed");
            }
         }
      }

      if (!stopping() && enabled_sources != 0 && successful_sources == 0 && first_failure) {
         std::rethrow_exception(first_failure);
      }

      auto results = std::vector<discovery::result>{};
      results.reserve(policy_.max_candidates);
      auto dht_index = std::size_t{};
      auto rendezvous_index = std::size_t{};
      auto peer_exchange_index = std::size_t{};
      auto source_offset = std::size_t{};
      {
         const auto lock = std::scoped_lock{mutex_};
         source_offset = next_source_index_;
         next_source_index_ = (next_source_index_ + 1) % 3;
      }
      while (results.size() < policy_.max_candidates) {
         auto advanced = false;
         const auto append_next = [&results, &advanced](std::vector<discovery::result>& source, std::size_t& index) {
            if (index < source.size()) {
               results.push_back(std::move(source[index++]));
               advanced = true;
            }
         };
         for (auto source = std::size_t{}; source < 3 && results.size() < policy_.max_candidates; ++source) {
            switch ((source_offset + source) % 3) {
            case 0:
               append_next(dht, dht_index);
               break;
            case 1:
               append_next(rendezvous, rendezvous_index);
               break;
            default:
               append_next(peer_exchange, peer_exchange_index);
               break;
            }
         }
         if (!advanced) {
            break;
         }
      }

      static_cast<void>(cancellation->finish());
      remove_cancellation(cancellation);
      co_return results;
   } catch (...) {
      static_cast<void>(cancellation->finish());
      remove_cancellation(cancellation);
      throw;
   }
}

boost::asio::awaitable<std::vector<discovery::result>>
topology_manager::async_collect_rendezvous(const std::shared_ptr<cancellation_latch>& cancellation, std::size_t limit) {
   if (policy_.operating_mode == topology::mode::static_only || !policy_.rendezvous_enabled || rendezvous_clients_.empty() ||
       !callbacks_.local_rendezvous_record || !callbacks_.rendezvous_register || !callbacks_.rendezvous_discover) {
      co_return std::vector<discovery::result>{};
   }

   const auto local_record = callbacks_.local_rendezvous_record();
   if (local_record.signed_peer_record.empty()) {
      co_return std::vector<discovery::result>{};
   }

   auto results = std::vector<discovery::result>{};
   results.reserve(limit);
   const auto point_limit = std::min(policy_.rendezvous_points.size(), policy_.max_rendezvous_points);
   auto attempted = false;
   auto succeeded = false;
   auto first_failure = std::exception_ptr{};
   const auto remember_failure = [&first_failure](std::exception_ptr failure) {
      if (!first_failure) {
         first_failure = std::move(failure);
      }
   };
   for (auto client = rendezvous_clients_.begin(); client != rendezvous_clients_.end(); ++client) {
      rendezvous_work work;
      {
         const auto lock = std::scoped_lock{mutex_};
         if (phase_ != phase::running || client->second.point_index >= point_limit ||
             client->second.retry_after > clocks_.steady_now()) {
            continue;
         }
         work = rendezvous_work{
             .key = client->first,
             .point_index = client->second.point_index,
             .registration_due = !client->second.confirmed_registration ||
                                 client->second.registered_generation != local_record.generation ||
                                 client->second.renew_after <= clocks_.system_now(),
             .cookie = client->second.cookie,
         };
      }

      attempted = true;
      try {
         if (work.registration_due) {
            auto registered = co_await callbacks_.rendezvous_register(
                work.point_index, work.key.namespace_name, local_record.signed_peer_record, cancellation);
            if (!registered.accepted || registered.ttl.count() <= 0) {
               note_rendezvous_failure(work.key);
               remember_failure(std::make_exception_ptr(
                   exceptions::internal{"P2P topology Rendezvous registration was rejected"}));
               continue;
            }
            const auto now = clocks_.system_now();
            const auto expires_at = rendezvous_expiry_after(now, registered.ttl);
            const auto lock = std::scoped_lock{mutex_};
            if (const auto state = rendezvous_clients_.find(work.key); state != rendezvous_clients_.end()) {
               state->second.confirmed_registration = true;
               state->second.registered_generation = local_record.generation;
               state->second.expires_at = expires_at;
               state->second.renew_after = rendezvous_renewal_time(now, expires_at);
               // A successful registration clears transport backoff for this point.
               state->second.failures = 0;
               state->second.retry_after = {};
            }
         }

         auto discovered = co_await callbacks_.rendezvous_discover(work.point_index, work.key.namespace_name,
                                                                     policy_.max_candidates, std::move(work.cookie),
                                                                     cancellation);
         if (discovered.response_status == callbacks::rendezvous_discover_result::status::invalid_cookie) {
            {
               const auto lock = std::scoped_lock{mutex_};
               if (const auto state = rendezvous_clients_.find(work.key); state != rendezvous_clients_.end()) {
                  state->second.cookie.clear();
               }
            }
            discovered = co_await callbacks_.rendezvous_discover(work.point_index, work.key.namespace_name,
                                                                   policy_.max_candidates, {}, cancellation);
         }
         if (discovered.response_status != callbacks::rendezvous_discover_result::status::ok) {
            note_rendezvous_failure(work.key);
            remember_failure(
                std::make_exception_ptr(exceptions::internal{"P2P topology Rendezvous discovery was rejected"}));
            continue;
         }
         note_rendezvous_success(work.key, std::move(discovered.cookie));
         succeeded = true;
         for (auto& result : discovered.results) {
            if (results.size() >= limit) {
               break;
            }
            result.discovered_by = discovery::source::rendezvous;
            results.push_back(std::move(result));
         }
      } catch (...) {
         note_rendezvous_failure(work.key);
         remember_failure(std::current_exception());
         if (!stopping()) {
            forge::exceptions::capture_and_log("P2P topology rendezvous query failed");
         }
      }
   }
   if (!stopping() && attempted && !succeeded) {
      if (first_failure) {
         std::rethrow_exception(first_failure);
      }
      throw exceptions::internal{"P2P topology Rendezvous discovery failed without a result"};
   }
   co_return results;
}

boost::asio::awaitable<void> topology_manager::async_unregister_rendezvous() {
   if (!callbacks_.rendezvous_unregister) {
      co_return;
   }
   auto confirmed = std::vector<std::pair<rendezvous_key, rendezvous_state>>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      confirmed.reserve(rendezvous_clients_.size());
      for (const auto& [key, state] : rendezvous_clients_) {
         if (state.confirmed_registration) {
            confirmed.emplace_back(key, state);
         }
      }
   }
   for (const auto& [key, state] : confirmed) {
      try {
         co_await callbacks_.rendezvous_unregister(state.point_index, key.namespace_name);
      } catch (...) {
         forge::exceptions::capture_and_log("P2P topology rendezvous unregister failed");
      }
   }
}

void topology_manager::merge_observations(const std::vector<discovery::result>& results) {
   const auto now = clocks_.system_now();
   const auto fallback_expiry = saturating_topology_expiry(now, policy_.refresh_interval);
   const auto lock = std::scoped_lock{mutex_};
   for (auto it = observations_.begin(); it != observations_.end();) {
      if (it->second.expires_at <= now) {
         it = observations_.erase(it);
      } else {
         ++it;
      }
   }

   const auto lower_value = [](const auto& left_key, const auto& left_result,
                               std::chrono::system_clock::time_point left_expiry, const auto& right_key,
                               const auto& right_result, std::chrono::system_clock::time_point right_expiry) {
      if (left_result.score != right_result.score) {
         return left_result.score < right_result.score;
      }
      if (left_expiry != right_expiry) {
         return left_expiry < right_expiry;
      }
      if (left_key.source != right_key.source) {
         return static_cast<std::uint16_t>(left_key.source) > static_cast<std::uint16_t>(right_key.source);
      }
      return left_key.peer.to_string() > right_key.peer.to_string();
   };

   const auto input_count = std::min(results.size(), policy_.max_candidates);
   for (auto index = std::size_t{}; index < input_count; ++index) {
      const auto& result = results[index];
      if (result.discovered_by == discovery::source::mdns || result.peer.value.empty() || result.endpoints.empty()) {
         continue;
      }
      const auto key = observation_key{.peer = result.peer, .source = result.discovered_by};
      const auto expiry = result.expires_at == std::chrono::system_clock::time_point{} ? fallback_expiry : result.expires_at;
      auto value = result;
      value.expires_at = expiry;
      if (const auto existing = observations_.find(key); existing != observations_.end()) {
         existing->second.result = std::move(value);
         existing->second.expires_at = expiry;
         continue;
      }

      if (observations_.size() >= policy_.max_candidates) {
         auto victim = observations_.begin();
         for (auto current = std::next(victim); current != observations_.end(); ++current) {
            if (lower_value(current->first, current->second.result, current->second.expires_at, victim->first,
                            victim->second.result, victim->second.expires_at)) {
               victim = current;
            }
         }
         if (!lower_value(victim->first, victim->second.result, victim->second.expires_at, key, value, expiry)) {
            continue;
         }
         observations_.erase(victim);
      }
      observations_.emplace(key, observation{.result = std::move(value), .expires_at = expiry});
   }
}

std::vector<topology_manager::dial_candidate>
topology_manager::candidates_for_dial(const connection_manager::snapshot& sessions) {
   const auto system_now = clocks_.system_now();
   const auto steady_now = clocks_.steady_now();
   auto connected = std::set<peer_id>{};
   for (const auto& session : sessions.sessions) {
      connected.insert(session.peer);
   }

   auto by_peer = std::map<peer_id, discovery::result>{};
   auto revisions = std::map<peer_id, std::uint64_t>{};
   {
      const auto lock = std::scoped_lock{mutex_};
      for (auto it = observations_.begin(); it != observations_.end();) {
         if (it->second.expires_at <= system_now) {
            it = observations_.erase(it);
            continue;
         }
         if (connected.contains(it->first.peer) || it->second.retry_after > steady_now) {
            ++it;
            continue;
         }
         const auto found = by_peer.find(it->first.peer);
         if (found == by_peer.end() || it->second.result.score > found->second.score ||
             (it->second.result.score == found->second.score &&
              static_cast<std::uint16_t>(it->second.result.discovered_by) <
                  static_cast<std::uint16_t>(found->second.discovered_by))) {
            by_peer.insert_or_assign(it->first.peer, it->second.result);
         }
         ++it;
      }
      for (const auto& [peer, state] : mdns_) {
         if (connected.contains(peer) || by_peer.contains(peer) || state.retry_after > steady_now) { continue; }
         auto candidate = discovery::result{.peer = peer, .discovered_by = discovery::source::mdns};
         const std::string* previous_address = nullptr;
         for (const auto& lease : state.leases) {
            if (lease.expires_at <= steady_now) { continue; }
            if (!previous_address || *previous_address != lease.address_key) {
               candidate.endpoints.push_back(lease.address);
               previous_address = &lease.address_key;
            }
         }
         if (!candidate.endpoints.empty()) {
            by_peer.emplace(peer, std::move(candidate));
            revisions.emplace(peer, state.revision);
         }
      }
   }

   auto candidates = std::vector<discovery::result>{};
   candidates.reserve(by_peer.size());
   for (auto& [_, candidate] : by_peer) {
      candidates.push_back(std::move(candidate));
   }
   std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
      if (left.score != right.score) {
         return left.score > right.score;
      }
      if (left.peer != right.peer) {
         return left.peer.to_string() < right.peer.to_string();
      }
      return static_cast<std::uint16_t>(left.discovered_by) < static_cast<std::uint16_t>(right.discovered_by);
   });
   if (candidates.size() > policy_.max_candidates) {
      candidates.resize(policy_.max_candidates);
   }
   auto result = std::vector<dial_candidate>{};
   for (auto& candidate : candidates) {
      const auto found = revisions.find(candidate.peer);
      const auto revision = found == revisions.end() ? 0 : found->second;
      result.push_back({std::move(candidate), revision});
   }
   return result;
}

void topology_manager::replace_mdns_snapshot(std::vector<mdns_lease> leases) {
   const auto now = clocks_.steady_now();
   auto next = std::map<peer_id, mdns_observation>{};
   auto count = std::size_t{};
   auto bytes = std::size_t{};
   for (auto& lease : leases) {
      if (lease.expires_at <= now || !lease.interface_index || !lease.generation || lease.peer.value.empty()) { continue; }
      auto key = lease.address.to_string();
      lease.address_key.swap(key);
      const auto size = 2 * lease.address_key.size() + lease.peer.value.size() + sizeof(mdns_lease);
      if (count == 4096 || size > 1024 * 1024 - bytes) { break; }
      if (!next.contains(lease.peer) && next.size() == policy_.max_candidates) { continue; }
      next[lease.peer].leases.push_back(std::move(lease));
      ++count;
      bytes += size;
   }
   // Canonical identity excludes expiry. All formatting, sorting and duplicate
   // storage work happens before taking the topology mutex.
   for (auto& [peer, value] : next) {
      std::sort(value.leases.begin(), value.leases.end(), [](const auto& a, const auto& b) {
         if (a.address_key != b.address_key) { return a.address_key < b.address_key; }
         if (a.interface_index != b.interface_index) { return a.interface_index < b.interface_index; }
         return a.generation < b.generation;
      });
      auto unique = std::vector<mdns_lease>{};
      unique.reserve(value.leases.size());
      for (auto& lease : value.leases) {
         if (!unique.empty() && unique.back().address_key == lease.address_key &&
             unique.back().interface_index == lease.interface_index && unique.back().generation == lease.generation) {
            unique.back().expires_at = std::max(unique.back().expires_at, lease.expires_at);
         } else { unique.push_back(std::move(lease)); }
      }
      value.leases = std::move(unique);
   }
   auto canceled = std::vector<std::shared_ptr<cancellation_latch>>{};
   canceled.reserve(std::min(policy_.max_parallel_dials, std::size_t{4096}));
   const auto commit_now = clocks_.steady_now();
   {
      const auto lock = std::scoped_lock{mutex_};
      if (phase_ == phase::stopping || phase_ == phase::stopped) { return; }
      for (auto& [peer, value] : next) {
         const auto previous = mdns_.find(peer);
         const auto unchanged = previous != mdns_.end() && previous->second.leases.size() == value.leases.size() &&
             std::ranges::equal(previous->second.leases, value.leases, [&](const auto& old, const auto& current) {
                return old.interface_index == current.interface_index && old.generation == current.generation &&
                    old.address_key == current.address_key && current.expires_at > commit_now &&
                    old.expires_at > commit_now;
             });
         if (unchanged) {
            value.retry_after = previous->second.retry_after;
            value.failures = previous->second.failures;
            value.revision = previous->second.revision;
         } else {
            value.revision = ++mdns_revision_;
         }
      }
      for (auto& [cancel, attempt] : mdns_attempts_) {
         const auto found = next.find(attempt.peer);
         if (attempt.finished || attempt.expired) { continue; }
         if (found == next.end() || found->second.revision != attempt.revision) {
            attempt.expired = true;
            canceled.push_back(cancel);
         } else {
            attempt.deadline = std::chrono::steady_clock::time_point::max();
            for (const auto& lease : found->second.leases) {
               attempt.deadline = std::min(attempt.deadline, lease.expires_at);
            }
         }
      }
      mdns_.swap(next); // Destroy the previous snapshot after unlocking.
      reconcile_pending_ = true;
   }
   for (const auto& cancel : canceled) { cancel->request_stop(); }
   changed_->notify();
}

std::optional<std::chrono::steady_clock::time_point>
topology_manager::admit_mdns_dial(dial_candidate& candidate, const std::shared_ptr<cancellation_latch>& cancellation) {
   const auto now = clocks_.steady_now();
   const auto lock = std::scoped_lock{mutex_};
   const auto found = mdns_.find(candidate.result.peer);
   if (phase_ != phase::running || found == mdns_.end() || found->second.revision != candidate.mdns_revision ||
       mdns_attempts_.size() >= std::min(policy_.max_parallel_dials, std::size_t{4096})) {
      return std::nullopt;
   }
   candidate.result.endpoints.clear();
   auto deadline = std::chrono::steady_clock::time_point::max();
   const std::string* previous_address = nullptr;
   for (const auto& lease : found->second.leases) {
      if (lease.expires_at <= now) { continue; }
      deadline = std::min(deadline, lease.expires_at);
      if (!previous_address || *previous_address != lease.address_key) {
         candidate.result.endpoints.push_back(lease.address);
         previous_address = &lease.address_key;
      }
   }
   if (candidate.result.endpoints.empty()) { return std::nullopt; }
   mdns_attempts_.emplace(cancellation, mdns_attempt{candidate.result.peer, candidate.mdns_revision, deadline});
   return deadline;
}

void topology_manager::finish_mdns_dial(const dial_candidate& candidate,
                                       const std::shared_ptr<cancellation_latch>& cancellation, bool succeeded) {
   const auto now = clocks_.steady_now();
   const auto lock = std::scoped_lock{mutex_};
   mdns_attempts_.erase(cancellation);
   const auto found = mdns_.find(candidate.result.peer);
   if (found == mdns_.end() || found->second.revision != candidate.mdns_revision) { return; }
   if (cancellation->stop_requested()) { return; }
   if (succeeded) {
      found->second.failures = 0;
      found->second.retry_after = {};
   } else {
      found->second.failures = std::min(found->second.failures + 1, std::size_t{4});
      found->second.retry_after = bounded_retry_deadline(now,
          retry_delay(observation_key{candidate.result.peer, discovery::source::mdns}, found->second.failures));
   }
}

std::chrono::milliseconds topology_manager::retry_delay(const observation_key& key, std::size_t failures) const {
   const auto capped_failures = std::min(failures, std::size_t{4});
   const auto multiplier = std::uint64_t{1} << capped_failures;
   const auto base = static_cast<std::uint64_t>(policy_.refresh_interval.count());
   const auto maximum =
       static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{1}).count());
   const auto bounded = base > maximum / multiplier ? maximum : std::min(maximum, base * multiplier);
   const auto fraction = static_cast<double>(observation_hash(key.peer, key.source, failures) % 10'001U) / 10'000.0;
   const auto factor = 1.0 - policy_.retry_jitter + (2.0 * policy_.retry_jitter * fraction);
   const auto adjusted = static_cast<std::uint64_t>(std::llround(static_cast<double>(bounded) * factor));
   return std::chrono::milliseconds{std::max<std::uint64_t>(1, std::min(maximum, adjusted))};
}

std::chrono::milliseconds topology_manager::retry_delay(const rendezvous_key& key, std::size_t failures) const {
   const auto capped_failures = std::min(failures, std::size_t{4});
   const auto multiplier = std::uint64_t{1} << capped_failures;
   const auto base = static_cast<std::uint64_t>(policy_.refresh_interval.count());
   const auto maximum =
       static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::hours{1}).count());
   const auto bounded = base > maximum / multiplier ? maximum : std::min(maximum, base * multiplier);
   const auto fraction = static_cast<double>(rendezvous_hash(key.peer, key.namespace_name, failures) % 10'001U) / 10'000.0;
   const auto factor = 1.0 - policy_.retry_jitter + (2.0 * policy_.retry_jitter * fraction);
   const auto adjusted = static_cast<std::uint64_t>(std::llround(static_cast<double>(bounded) * factor));
   return std::chrono::milliseconds{std::max<std::uint64_t>(1, std::min(maximum, adjusted))};
}

void topology_manager::note_rendezvous_failure(const rendezvous_key& key) noexcept {
   try {
      const auto now = clocks_.steady_now();
      const auto lock = std::scoped_lock{mutex_};
      const auto state = rendezvous_clients_.find(key);
      if (state == rendezvous_clients_.end()) {
         return;
      }
      if (state->second.failures < (std::numeric_limits<std::size_t>::max)()) {
         ++state->second.failures;
      }
      state->second.retry_after = bounded_retry_deadline(now, retry_delay(key, state->second.failures));
   } catch (...) {
   }
}

void topology_manager::note_rendezvous_success(const rendezvous_key& key, std::vector<std::uint8_t> cookie) noexcept {
   try {
      const auto lock = std::scoped_lock{mutex_};
      const auto state = rendezvous_clients_.find(key);
      if (state == rendezvous_clients_.end()) {
         return;
      }
      state->second.cookie = std::move(cookie);
      state->second.failures = 0;
      state->second.retry_after = {};
   } catch (...) {
   }
}

void topology_manager::note_dial_result(const discovery::result& result, bool succeeded) noexcept {
   try {
      const auto key = observation_key{.peer = result.peer, .source = result.discovered_by};
      const auto now = clocks_.steady_now();
      const auto lock = std::scoped_lock{mutex_};
      const auto found = observations_.find(key);
      if (found == observations_.end()) {
         return;
      }
      if (succeeded) {
         found->second.failures = 0;
         found->second.retry_after = {};
         return;
      }
      if (found->second.failures < (std::numeric_limits<std::size_t>::max)()) {
         ++found->second.failures;
      }
      found->second.retry_after = bounded_retry_deadline(now, retry_delay(key, found->second.failures));
   } catch (...) {
   }
}

} // namespace forge::net::p2p::detail
