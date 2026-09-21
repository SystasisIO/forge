module;

#include <chrono>
#include <cstddef>

export module forge.net.p2p.reachability_policy;

export namespace forge::net::p2p {

struct reachability_policy {
   bool client_v1_enabled = true;
   bool client_v2_enabled = true;
   bool service_v1_enabled = false;
   bool service_v2_enabled = false;
   std::chrono::milliseconds timeout{20'000};
   std::chrono::milliseconds refresh_interval{60'000};
   std::chrono::milliseconds observation_ttl{600'000};
   std::size_t min_observers = 4;
   std::size_t min_reachability_observers = 3;
   std::size_t max_observers = 64;
   std::size_t max_observations = 1024;
   std::size_t max_candidates = 256;
   std::size_t max_confirmed_per_local = 3;
   std::size_t max_pending_probes = 4;
   std::size_t service_per_peer_limit = 3;
   std::size_t service_global_limit = 30;
   std::chrono::milliseconds service_rate_interval{60'000};
   bool ping_enabled = true;
   std::chrono::milliseconds ping_interval{30'000};
   std::chrono::milliseconds ping_timeout{10'000};
   std::size_t max_parallel_pings = 4;
   std::size_t max_event_subscribers = 64;
};

void validate(const reachability_policy& value);

} // namespace forge::net::p2p
