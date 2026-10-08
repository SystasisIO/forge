#pragma once

#include <cstddef>

namespace forge::tests::p2p {

// Thread-local injection is armed only around synchronous native claim/score transitions.
class pubsub_claim_allocation {
 public:
   explicit pubsub_claim_allocation(std::size_t fail_at = 0) noexcept;
   ~pubsub_claim_allocation();
   pubsub_claim_allocation(const pubsub_claim_allocation&) = delete;
   pubsub_claim_allocation& operator=(const pubsub_claim_allocation&) = delete;
   [[nodiscard]] std::size_t calls() const noexcept;
   static void record();
};

} // namespace forge::tests::p2p
