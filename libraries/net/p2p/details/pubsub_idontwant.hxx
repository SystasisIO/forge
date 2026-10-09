#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>

namespace forge::net::p2p::detail {

// Serialized by the node mutex. No tasks, transport, scores or probabilistic ID hashing.
class pubsub_idontwant {
 public:
   void receive(const peer_id& peer, std::span<const pubsub::control::idontwant> values,
                const pubsub::limits& limits, std::size_t max_peers) noexcept;
   [[nodiscard]] bool contains(const peer_id& peer, std::string_view id) const noexcept;
   void heartbeat() noexcept;
   void forget(const peer_id& peer) noexcept;
   void clear() noexcept;
   [[nodiscard]] std::size_t size() const noexcept;
   [[nodiscard]] std::size_t bytes() const noexcept;
   [[nodiscard]] std::uint64_t ignored() const noexcept;

 private:
   struct state {
      std::map<std::string, std::size_t, std::less<>> ids;
      std::size_t bytes = 0;
      std::size_t rpcs = 0;
   };
   std::map<peer_id, state> _peers;
   std::size_t _size = 0;
   std::size_t _bytes = 0;
   std::uint64_t _ignored = 0;
};

} // namespace forge::net::p2p::detail
