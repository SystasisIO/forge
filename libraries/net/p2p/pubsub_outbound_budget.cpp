module;

#include <algorithm>
#include <cstddef>
#include <map>

module forge.net.p2p.node;

import forge.net.p2p.identity;

#include "details/pubsub_outbound_budget.hxx"

namespace forge::net::p2p::detail {

bool pubsub_outbound_budget::reserve(const peer_id& peer, std::size_t bytes, std::size_t limit) {
   const auto found = reserved_.find(peer);
   const auto peer_reserved = found == reserved_.end() ? 0 : found->second;
   if (bytes > limit || peer_reserved > limit - bytes || total_ > limit - bytes) {
      return false;
   }
   if (found == reserved_.end()) {
      reserved_.emplace(peer, bytes);
   } else {
      found->second += bytes;
   }
   total_ += bytes;
   return true;
}

void pubsub_outbound_budget::release(const peer_id& peer, std::size_t bytes) noexcept {
   const auto found = reserved_.find(peer);
   if (found == reserved_.end()) {
      return;
   }
   const auto released = std::min(found->second, bytes);
   total_ -= std::min(total_, released);
   if (found->second <= bytes) {
      reserved_.erase(found);
   } else {
      found->second -= bytes;
   }
}

void pubsub_outbound_budget::clear() noexcept {
   reserved_.clear();
   total_ = 0;
}

std::size_t pubsub_outbound_budget::peers() const noexcept {
   return reserved_.size();
}

std::size_t pubsub_outbound_budget::total() const noexcept {
   return total_;
}

} // namespace forge::net::p2p::detail
