module;

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <vector>

module forge.net.p2p.node;
import forge.net.p2p.identity;

#include "details/pubsub_router.hxx"

namespace forge::net::p2p::detail {

pubsub_router::pubsub_router(std::size_t capacity) : pubsub_router(capacity, std::random_device{}()) {}
pubsub_router::pubsub_router(std::size_t capacity, std::uint64_t seed) : _capacity(capacity), _random(seed) {}

void pubsub_router::shuffle(std::vector<peer_id>& peers) {
   std::shuffle(peers.begin(), peers.end(), _random);
}

std::size_t pubsub_router::choose(std::size_t size) {
   return size == 0 ? 0 : std::uniform_int_distribution<std::size_t>{0, size - 1}(_random);
}

pubsub_router::clock::time_point pubsub_router::deadline(clock::time_point now,
                                                       std::chrono::milliseconds duration) noexcept {
   if (duration.count() <= 0) {
      return now;
   }
   if (duration > std::chrono::duration_cast<std::chrono::milliseconds>(clock::duration::max())) {
      return clock::time_point::max();
   }
   const auto delay = std::chrono::duration_cast<clock::duration>(duration);
   return now > clock::time_point::max() - delay ? clock::time_point::max() : now + delay;
}

std::optional<std::uint64_t> pubsub_router::stage(const peer_id& peer, const std::string& id) {
   const auto existing = _promises.find(id);
   if (existing != _promises.end() && existing->second.contains(peer)) {
      return std::nullopt; // A second dispatch cannot acquire or abort the first dispatch's token.
   }
   if (_deadlines.size() >= _capacity || _generation == std::numeric_limits<std::uint64_t>::max()) {
      return std::nullopt;
   }
   const auto generation = _generation + 1;
   const auto [message, inserted] = _promises.try_emplace(id);
   try {
      // Preallocate the deadline node. Activation only rekeys it after successful native I/O.
      const auto deadline = _deadlines.emplace(clock::time_point::max(), std::pair{id, peer});
      try {
         message->second.emplace(peer, request{.generation = generation, .deadline = deadline});
      } catch (...) {
         _deadlines.erase(deadline);
         throw;
      }
   } catch (...) {
      if (inserted) {
         _promises.erase(message);
      }
      throw;
   }
   _generation = generation;
   return generation;
}

bool pubsub_router::activate(const peer_id& peer, const std::string& id, std::uint64_t generation,
                             clock::time_point until) noexcept {
   const auto message = _promises.find(id);
   if (message == _promises.end()) {
      return false;
   }
   const auto source = message->second.find(peer);
   if (source == message->second.end() || source->second.generation != generation || source->second.active) {
      return false;
   }
   auto node = _deadlines.extract(source->second.deadline);
   node.key() = until;
   source->second.deadline = _deadlines.insert(std::move(node));
   source->second.active = true;
   return true;
}

void pubsub_router::abort(const peer_id& peer, const std::string& id, std::uint64_t generation) noexcept {
   const auto message = _promises.find(id);
   if (message == _promises.end()) {
      return;
   }
   const auto source = message->second.find(peer);
   if (source == message->second.end() || source->second.generation != generation) {
      return;
   }
   _deadlines.erase(source->second.deadline);
   message->second.erase(source);
   if (message->second.empty()) {
      _promises.erase(message);
   }
}

bool pubsub_router::promise(const peer_id& peer, const std::string& id, clock::time_point until) {
   if (const auto existing = _promises.find(id); existing != _promises.end() && existing->second.contains(peer)) {
      return true;
   }
   const auto generation = stage(peer, id);
   return generation && activate(peer, id, *generation, until);
}

void pubsub_router::fulfill(const std::string& id) {
   if (const auto message = _promises.find(id); message != _promises.end()) {
      for (const auto& [_, request] : message->second) {
         _deadlines.erase(request.deadline);
      }
      _promises.erase(message);
   }
}

void pubsub_router::forget(const peer_id& peer) {
   for (auto message = _promises.begin(); message != _promises.end();) {
      if (const auto source = message->second.find(peer); source != message->second.end()) {
         _deadlines.erase(source->second.deadline);
         message->second.erase(source);
      }
      message = message->second.empty() ? _promises.erase(message) : std::next(message);
   }
}

void pubsub_router::clear() noexcept {
   _promises.clear();
   _deadlines.clear();
}

std::map<peer_id, std::size_t> pubsub_router::expire(clock::time_point now) {
   auto broken = std::map<peer_id, std::size_t>{};
   // Prepare attribution before retiring promises; allocation failure preserves every deadline.
   for (auto it = _deadlines.begin(); it != _deadlines.end() && it->first <= now; ++it) {
      if (_promises.at(it->second.first).at(it->second.second).active) {
         ++broken[it->second.second];
      }
   }
   for (auto it = _deadlines.begin(); it != _deadlines.end() && it->first <= now;) {
      const auto message = _promises.find(it->second.first);
      if (!message->second.at(it->second.second).active) {
         ++it;
         continue;
      }
      message->second.erase(it->second.second);
      if (message->second.empty()) {
         _promises.erase(message);
      }
      it = _deadlines.erase(it);
   }
   return broken;
}

std::size_t pubsub_router::pending() const noexcept {
   return _deadlines.size();
}

} // namespace forge::net::p2p::detail
