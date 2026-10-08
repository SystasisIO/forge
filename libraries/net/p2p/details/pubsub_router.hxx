#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace forge::net::p2p::detail {

// Serialized by the node owner. No callback, clock source, task or session ownership.
class pubsub_router {
 public:
   using clock = std::chrono::steady_clock;
   explicit pubsub_router(std::size_t capacity);
   pubsub_router(std::size_t capacity, std::uint64_t seed);
   void shuffle(std::vector<peer_id>& peers);
   [[nodiscard]] std::size_t choose(std::size_t size);
   [[nodiscard]] static clock::time_point deadline(clock::time_point now, std::chrono::milliseconds duration) noexcept;
   [[nodiscard]] std::optional<std::uint64_t> stage(const peer_id& peer, const std::string& id);
   [[nodiscard]] bool activate(const peer_id& peer, const std::string& id, std::uint64_t generation,
                               clock::time_point until) noexcept;
   void abort(const peer_id& peer, const std::string& id, std::uint64_t generation) noexcept;
   [[nodiscard]] bool promise(const peer_id& peer, const std::string& id, clock::time_point until);
   void fulfill(const std::string& id);
   void forget(const peer_id& peer);
   void clear() noexcept;
   [[nodiscard]] std::map<peer_id, std::size_t> expire(clock::time_point now);
   [[nodiscard]] std::size_t pending() const noexcept;

 private:
   using deadline_index = std::multimap<clock::time_point, std::pair<std::string, peer_id>>;
   struct request {
      std::uint64_t generation;
      bool active = false;
      deadline_index::iterator deadline;
   };
   const std::size_t _capacity;
   std::mt19937_64 _random;
   std::uint64_t _generation = 0;
   std::map<std::string, std::map<peer_id, request>> _promises;
   deadline_index _deadlines;
};

} // namespace forge::net::p2p::detail
