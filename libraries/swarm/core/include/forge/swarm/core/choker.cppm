module;

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

export module forge.swarm.core.choker;

export import forge.swarm.core.peer_observation;
export import forge.swarm.core.exceptions;

export namespace forge::swarm::core {

// Caller invokes selection rounds and applies the returned complete serving set.
class choker {
 public:
   struct config {
      std::uint32_t peers = 128;
      // Total capacity, INCLUDING the optional single optimistic slot.
      std::uint32_t slots = 4;
      bool optimistic = true;
      std::chrono::milliseconds optimistic_interval{30000};
      std::chrono::milliseconds seed_rotation_interval{60000};
      std::uint64_t seed_quota_bytes = 1024U * 1024U;
   };
   struct candidate {
      peer_id peer = 0;
      bool interested = true;
      peer_observation observation;
      // Cumulative successfully served payload bytes, reported by the caller.
      std::uint64_t served_bytes = 0;
   };
   struct plan {
      std::vector<peer_id> unchoked;
      std::vector<peer_id> choked;
      std::optional<peer_id> optimistic;
      bool operator==(const plan&) const = default;
   };

   choker(config limits, std::uint64_t tie_seed);
   // Full current connection inventory, unique nonzero IDs; absent peers are pruned.
   // Strictly increasing round times after the first, all counters nondecreasing.
   [[nodiscard]] plan select(std::span<const candidate>, transfer_time now, bool seeding);
   [[nodiscard]] std::size_t tracked_peers() const noexcept;

 private:
   struct peer_state {
      candidate previous;
      transfer_time last_unchoked{};
      transfer_time last_optimistic{};
      std::uint64_t served_at_unchoke = 0;
      bool unchoked = false;
      bool served = false;
      bool explored = false;
   };

   config _limits;
   std::uint64_t _seed;
   std::map<peer_id, peer_state> _peers;
   std::optional<peer_id> _optimistic;
   transfer_time _optimistic_since{};
   std::optional<transfer_time> _time;
};

} // namespace forge::swarm::core
