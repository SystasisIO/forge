#pragma once

#include <cstddef>
#include <map>

namespace forge::net::p2p::detail {

class pubsub_outbound_budget {
 public:
   [[nodiscard]] bool reserve(const peer_id& peer, std::size_t bytes, std::size_t limit);
   void release(const peer_id& peer, std::size_t bytes) noexcept;
   void clear() noexcept;

   [[nodiscard]] std::size_t peers() const noexcept;
   [[nodiscard]] std::size_t total() const noexcept;

 private:
   std::map<peer_id, std::size_t> reserved_;
   std::size_t total_ = 0;
};

} // namespace forge::net::p2p::detail
