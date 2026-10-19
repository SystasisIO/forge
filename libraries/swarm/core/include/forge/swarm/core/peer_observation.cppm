module;

#include <chrono>
#include <cstdint>

export module forge.swarm.core.peer_observation;

export namespace forge::swarm::core {

// Connection-local opaque identity. Transfer never reuses an issued peer ID.
using peer_id = std::uint64_t;
using transfer_time = std::chrono::time_point<std::chrono::steady_clock, std::chrono::milliseconds>;

// Cumulative, saturating counters, not admission, reputation or payment policy.
struct peer_observation {
   std::uint64_t received_bytes = 0;
   std::uint64_t useful_bytes = 0;
   std::uint64_t responses = 0;
   std::uint64_t latency_milliseconds = 0;
   std::uint64_t timeouts = 0;
   // Only a failed piece whose complete provenance is this one peer.
   std::uint64_t integrity_failures = 0;
};

} // namespace forge::swarm::core
