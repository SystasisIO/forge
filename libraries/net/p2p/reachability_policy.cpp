module;

#include <chrono>
#include <forge/exceptions/macros.hpp>

module forge.net.p2p.reachability_policy;

import forge.net.p2p.exceptions;

namespace forge::net::p2p {

void validate(const reachability_policy& value) {
   constexpr auto largest_interval = std::chrono::hours{24};
   const auto valid_interval = [&](std::chrono::milliseconds interval) {
      return interval > std::chrono::milliseconds::zero() && interval <= largest_interval;
   };
   if (!valid_interval(value.timeout) || !valid_interval(value.refresh_interval) ||
       !valid_interval(value.observation_ttl) || !valid_interval(value.service_rate_interval) ||
       !valid_interval(value.ping_interval) || !valid_interval(value.ping_timeout) ||
       value.observation_ttl < value.timeout) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options,
                            "P2P reachability intervals must be positive, bounded by one day, and TTL cover a probe");
   }
   if (value.min_observers == 0 || value.min_reachability_observers == 0 ||
       value.max_observers < value.min_observers || value.max_observers < value.min_reachability_observers ||
       value.max_observations < value.max_observers || value.max_candidates == 0 ||
       value.max_confirmed_per_local == 0 || value.max_confirmed_per_local > value.max_candidates ||
       value.max_pending_probes == 0 || value.max_pending_probes > value.max_observers ||
       value.max_parallel_pings == 0 || value.max_event_subscribers == 0 ||
       value.service_per_peer_limit == 0 || value.service_global_limit < value.service_per_peer_limit) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_options, "P2P reachability resource bounds are inconsistent");
   }
}

} // namespace forge::net::p2p
