module;

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <vector>
#include <forge/exceptions/macros.hpp>

module forge.swarm.core.choker;

namespace forge::swarm::core {
namespace {

std::uint64_t rank(std::uint64_t seed, peer_id id) noexcept {
   auto value = seed + id + 0x9e3779b97f4a7c15ULL;
   value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
   value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
   return value ^ (value >> 31U);
}

bool monotonic(const peer_observation& before, const peer_observation& after) noexcept {
   return after.received_bytes >= before.received_bytes && after.useful_bytes >= before.useful_bytes &&
          after.responses >= before.responses && after.latency_milliseconds >= before.latency_milliseconds &&
          after.timeouts >= before.timeouts && after.integrity_failures >= before.integrity_failures;
}

} // namespace

choker::choker(config limits, std::uint64_t tie_seed) : _limits(limits), _seed(tie_seed) {
   if (limits.peers == 0U || limits.peers > 4096U || limits.slots > limits.peers ||
       limits.optimistic_interval.count() <= 0 || limits.seed_rotation_interval.count() <= 0 ||
       limits.seed_quota_bytes == 0U) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "invalid choke selection bounds");
   }
}

choker::plan choker::select(std::span<const candidate> candidates, transfer_time now, bool seeding) {
   if (now.time_since_epoch().count() < 0 || (_time && now <= *_time)) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "choke rounds require increasing nonnegative time");
   }
   if (candidates.size() > _limits.peers) {
      FORGE_THROW_EXCEPTION(exceptions::limit_exceeded, "choke candidate bound exceeded");
   }
   auto current = std::map<peer_id, const candidate*>{};
   for (const auto& value : candidates) {
      if (value.peer == 0U || !current.emplace(value.peer, &value).second ||
          value.observation.useful_bytes > value.observation.received_bytes ||
          (value.observation.responses == 0U && value.observation.latency_milliseconds != 0U)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "malformed choke candidate");
      }
      const auto before = _peers.find(value.peer);
      if (before != _peers.end() && (!monotonic(before->second.previous.observation, value.observation) ||
                                     value.served_bytes < before->second.previous.served_bytes)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_transfer, "choke observation counters regressed");
      }
   }
   // Prepare the entire next round off to the side. Allocation failure cannot
   // consume samples or an optimistic rotation without returning the plan.
   auto states = std::map<peer_id, peer_state>{};
   auto eligible = std::vector<peer_id>{};
   eligible.reserve(candidates.size());
   for (const auto& [id, value] : current) {
      const auto before = _peers.find(id);
      states.emplace(id, before == _peers.end() ? peer_state{} : before->second);
      if (value->interested)
         eligible.push_back(id);
   }
   const auto delta = [&](peer_id id, auto member) {
      return current.at(id)->observation.*member - states.at(id).previous.observation.*member;
   };
   const auto latency = [&](peer_id id) {
      const auto responses = delta(id, &peer_observation::responses);
      return responses == 0U ? std::numeric_limits<std::uint64_t>::max()
                             : delta(id, &peer_observation::latency_milliseconds) / responses;
   };
   const auto quota_complete = [&](peer_id id) {
      const auto& state = states.at(id);
      return state.unchoked && current.at(id)->served_bytes - state.served_at_unchoke >= _limits.seed_quota_bytes &&
             now - state.last_unchoked >= _limits.seed_rotation_interval;
   };
   std::sort(eligible.begin(), eligible.end(), [&](peer_id a, peer_id b) {
      const auto useful_a = delta(a, &peer_observation::useful_bytes);
      const auto useful_b = delta(b, &peer_observation::useful_bytes);
      if (!seeding && useful_a != useful_b)
         return useful_a > useful_b;
      const auto& left = states.at(a);
      const auto& right = states.at(b);
      if (seeding) {
         const auto quota_a = quota_complete(a);
         const auto quota_b = quota_complete(b);
         if (quota_a != quota_b)
            return !quota_a;
         // Ignore residual upload for a choked peer, as in the donor choker.
         const auto rate_a = left.unchoked ? current.at(a)->served_bytes - left.previous.served_bytes : 0U;
         const auto rate_b = right.unchoked ? current.at(b)->served_bytes - right.previous.served_bytes : 0U;
         if (rate_a != rate_b)
            return rate_a > rate_b;
      }
      const auto integrity_a = delta(a, &peer_observation::integrity_failures);
      const auto integrity_b = delta(b, &peer_observation::integrity_failures);
      if (integrity_a != integrity_b)
         return integrity_a < integrity_b;
      const auto timeout_a = delta(a, &peer_observation::timeouts);
      const auto timeout_b = delta(b, &peer_observation::timeouts);
      if (timeout_a != timeout_b)
         return timeout_a < timeout_b;
      if (latency(a) != latency(b))
         return latency(a) < latency(b);
      if (left.served != right.served)
         return !left.served;
      if (left.last_unchoked != right.last_unchoked)
         return left.last_unchoked < right.last_unchoked;
      const auto rank_a = rank(_seed, a);
      const auto rank_b = rank(_seed, b);
      return rank_a != rank_b ? rank_a < rank_b : a < b;
   });

   auto result = plan{};
   result.unchoked.reserve(std::min<std::size_t>(_limits.slots, eligible.size()));
   result.choked.reserve(candidates.size());
   const auto regular_slots = _limits.slots - (_limits.optimistic && _limits.slots != 0U ? 1U : 0U);
   const auto regular = std::min<std::size_t>(regular_slots, eligible.size());
   result.unchoked.insert(result.unchoked.end(), eligible.begin(), eligible.begin() + regular);
   const auto selected = [&](peer_id id) {
      return std::find(result.unchoked.begin(), result.unchoked.end(), id) != result.unchoked.end();
   };
   auto optimistic_since = _optimistic_since;
   if (_limits.optimistic && _limits.slots != 0U && eligible.size() > regular) {
      const auto keep = _optimistic && current.contains(*_optimistic) && current.at(*_optimistic)->interested &&
                        !selected(*_optimistic) && now - _optimistic_since < _limits.optimistic_interval;
      if (keep)
         result.optimistic = _optimistic;
      else {
         for (const auto id : eligible) {
            if (selected(id))
               continue;
            if (!result.optimistic) {
               result.optimistic = id;
               continue;
            }
            const auto& a = states.at(id);
            const auto& b = states.at(*result.optimistic);
            if ((a.explored != b.explored && !a.explored) ||
                (a.explored == b.explored &&
                 (a.last_optimistic < b.last_optimistic ||
                  (a.last_optimistic == b.last_optimistic &&
                   (rank(_seed, id) < rank(_seed, *result.optimistic) ||
                    (rank(_seed, id) == rank(_seed, *result.optimistic) && id < *result.optimistic)))))) {
               result.optimistic = id;
            }
         }
         optimistic_since = now;
         auto& state = states.at(*result.optimistic);
         state.last_optimistic = now;
         state.explored = true;
      }
      result.unchoked.push_back(*result.optimistic);
   }
   std::sort(result.unchoked.begin(), result.unchoked.end());
   for (const auto& [id, value] : current) {
      auto& state = states.at(id);
      const auto unchoked = selected(id);
      if (unchoked && !state.unchoked) {
         state.last_unchoked = now;
         state.served_at_unchoke = value->served_bytes;
         state.served = true;
      }
      if (!unchoked)
         result.choked.push_back(id);
      state.unchoked = unchoked;
      state.previous = *value;
   }
   _peers.swap(states);
   _optimistic = result.optimistic;
   _optimistic_since = optimistic_since;
   _time = now;
   return result;
}

std::size_t choker::tracked_peers() const noexcept {
   return _peers.size();
}

} // namespace forge::swarm::core
