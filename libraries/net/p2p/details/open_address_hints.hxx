#pragma once

namespace forge::net::p2p {
class cancellation_latch;
}

namespace forge::net::p2p::detail {

// Immutable operation-owned acquisition context, never an address-book update.
struct open_address_hints {
   struct relay {
      peer_id peer;
      std::vector<forge::multiformats::multiaddr> carriers;
   };
   open_address_hints(const peer_id&, std::vector<forge::multiformats::multiaddr>,
      address_resolution::limits, std::size_t max_direct_endpoints,
      std::chrono::milliseconds direct_attempt_timeout,
      std::shared_ptr<cancellation_latch> cancellation = {});
   [[nodiscard]] std::vector<forge::multiformats::multiaddr> carriers(const peer_id&) const;
   std::vector<forge::multiformats::multiaddr> direct;
   std::vector<relay> relays;
   const std::size_t root_limit;
   const std::size_t max_direct_endpoints;
   const std::chrono::milliseconds direct_attempt_timeout;
   const std::shared_ptr<cancellation_latch> cancellation;
};

} // namespace forge::net::p2p::detail
