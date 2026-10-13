#pragma once

#include <atomic>
#include <cstddef>

namespace forge::tests::p2p {

// Replaceable allocation symbols are linked only into the isolated control target.
class pubsub_control_allocation {
 public:
   explicit pubsub_control_allocation(std::size_t ordinal = 0) noexcept;
   ~pubsub_control_allocation();
   pubsub_control_allocation(const pubsub_control_allocation&) = delete;
   pubsub_control_allocation& operator=(const pubsub_control_allocation&) = delete;
   [[nodiscard]] std::size_t calls() const noexcept;
   static void fail_next(std::atomic_size_t& observed) noexcept;
   static void record();
};

} // namespace forge::tests::p2p
