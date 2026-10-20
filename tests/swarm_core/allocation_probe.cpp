#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <new>
#include <stdexcept>
#include <vector>

import forge.swarm.core.transfer;
import forge.swarm.core.choker;

namespace {
thread_local std::ptrdiff_t allocations_before_failure = -1;
}

void* operator new(std::size_t size) {
   if (allocations_before_failure == 0)
      throw std::bad_alloc{};
   if (allocations_before_failure > 0)
      --allocations_before_failure;
   if (auto* memory = std::malloc(size == 0 ? 1 : size))
      return memory;
   throw std::bad_alloc{};
}
void* operator new[](std::size_t size) {
   return ::operator new(size);
}
void operator delete(void* pointer) noexcept {
   std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
   std::free(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
   std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
   std::free(pointer);
}

namespace core = forge::swarm::core;

namespace {

void require(bool condition, const char* message) {
   if (!condition)
      throw std::runtime_error{message};
}

core::descriptor fixture() {
   const auto bytes = std::vector<std::uint8_t>(8U * core::block_size, 7U);
   const auto hashes = core::hash_file(bytes, 2U * core::block_size);
   return {core::descriptor_version::v1, 2U * core::block_size, {{{"data"}, bytes.size(), hashes.root, hashes.pieces}}};
}

unsigned pick_failures(const core::descriptor& descriptor, unsigned scenario) {
   const auto now = core::transfer_time{std::chrono::milliseconds{1}};
   unsigned failures = 0;
   bool reached_success = false;
   for (std::ptrdiff_t point = 0; point < 128; ++point) {
      auto limits = core::transfer::config{};
      limits.requests_per_peer = scenario == 2U ? 7U : scenario == 3U ? 8U : 6U;
      limits.request_bytes_per_peer = std::uint64_t{limits.requests_per_peer} * core::block_size;
      if (scenario == 3U) {
         limits.requests = 9U;
         limits.request_bytes = 9U * core::block_size;
      }
      auto state = core::transfer{descriptor, limits, 10};
      state.set_selection(core::transfer::selection::sequential);
      const auto original_peer = state.add_peer(std::array<std::uint8_t, 1>{15});
      auto peer = original_peer;
      static_cast<void>(state.set_choked(peer, false, now));
      auto existing = std::vector<core::transfer::request>{};
      if (scenario == 1U) {
         const auto first = state.pick(peer, now, 1).front();
         require(state.receive(peer, first.id, first.length, now).accepted, "partial setup failed");
      }
      if (scenario == 2U)
         existing = state.pick(peer, now, 1U);
      if (scenario == 3U) {
         existing = state.pick(peer, now, 8U);
         require(existing.size() == 8U, "endgame setup must own every block");
         peer = state.add_peer(std::array<std::uint8_t, 1>{15});
         static_cast<void>(state.set_choked(peer, false, now));
      }
      const auto before = state.inspect();
      const auto checkpoint = state.progress();
      bool failed = false;
      allocations_before_failure = point;
      try {
         static_cast<void>(state.pick(peer, now, 6));
      } catch (const std::bad_alloc&) {
         failed = true;
      }
      allocations_before_failure = -1;
      if (!failed) {
         reached_success = true;
         break;
      }
      ++failures;
      const auto after = state.inspect();
      require(after.requests == before.requests && after.request_bytes == before.request_bytes &&
                  after.unfinished_pieces == before.unfinished_pieces &&
                  after.unfinished_bytes == before.unfinished_bytes &&
                  after.duplicate_requests == before.duplicate_requests && state.progress() == checkpoint,
              "allocation failure left hidden requests or changed checkpoint state");
      const auto expected = scenario == 3U ? 1U : 6U;
      const auto retry = state.pick(peer, now, 6);
      require(retry.size() == expected, "allocation failure consumed exact transfer capacity");
      require(state.pick(peer, now, 1).empty(), "exact request/byte limit was not enforced");
      for (const auto& original : existing) {
         require(state.cancel(original_peer, original.id, now), "failed batch lost existing request ownership");
      }
      for (const auto& request : retry) {
         require(state.cancel(peer, request.id, now), "retry lost request ownership");
      }
      require(state.inspect().requests == 0U && state.inspect().request_bytes == 0U &&
                  state.inspect().duplicate_requests == 0U,
              "canceled retry leaked capacity");
   }
   require(reached_success && failures >= 2U, "allocation probe did not cover failure and success");
   return failures;
}

unsigned choke_failures() {
   const auto now = core::transfer_time{std::chrono::milliseconds{1}};
   const auto later = now + std::chrono::seconds{31};
   auto candidates = std::array<core::choker::candidate, 8>{};
   for (unsigned i = 0; i < candidates.size(); ++i) {
      candidates[i].peer = i + 1;
      candidates[i].observation.received_bytes = (i + 1U) * 100U;
      candidates[i].observation.useful_bytes = (i + 1U) * 100U;
   }
   auto advanced = candidates;
   for (unsigned i = 0; i < advanced.size(); ++i) {
      const auto increment = i < 3U ? 10000U - i * 1000U : 1U;
      advanced[i].observation.received_bytes += increment;
      advanced[i].observation.useful_bytes += increment;
      advanced[i].served_bytes += increment;
   }
   auto reference = core::choker{{}, 19};
   static_cast<void>(reference.select(candidates, now, false));
   const auto expected = reference.select(advanced, later, false);
   for (core::peer_id id = 1U; id <= 3U; ++id) {
      require(std::find(expected.unchoked.begin(), expected.unchoked.end(), id) != expected.unchoked.end(),
              "nonzero samples must determine the regular serving slots");
   }
   unsigned failures = 0;
   bool reached_success = false;
   for (std::ptrdiff_t point = 0; point < 128; ++point) {
      auto state = core::choker{{}, 19};
      static_cast<void>(state.select(candidates, now, false));
      bool failed = false;
      allocations_before_failure = point;
      try {
         static_cast<void>(state.select(advanced, later, false));
      } catch (const std::bad_alloc&) {
         failed = true;
      }
      allocations_before_failure = -1;
      if (!failed) {
         reached_success = true;
         break;
      }
      ++failures;
      require(state.select(advanced, later, false) == expected,
              "allocation failure consumed choke samples, round time or optimistic rotation");
   }
   require(reached_success && failures > 4U, "choke allocation probe did not cover failure and success");
   return failures;
}

unsigned index_failures(const core::descriptor& descriptor, unsigned scenario) {
   const auto now = core::transfer_time{std::chrono::milliseconds{1}};
   unsigned failures = 0;
   bool reached_success = false;
   for (std::ptrdiff_t point = 0; point < 128; ++point) {
      auto state = core::transfer{descriptor, {}, 10};
      state.set_selection(core::transfer::selection::sequential);
      const auto peer = state.add_peer(std::array<std::uint8_t, 1>{scenario < 2U ? std::uint8_t{1} : std::uint8_t{15}});
      static_cast<void>(state.set_choked(peer, false, now));
      if (scenario == 2U) {
         const auto requests = state.pick(peer, now, 2);
         for (const auto& request : requests) {
            const auto received = state.receive(peer, request.id, request.length, now);
            if (received.ready)
               static_cast<void>(state.finish(*received.ready, true));
         }
         require(state.verified({0, 0}), "discard setup requires verified piece");
      }
      if (scenario == 3U)
         state.set_priority({0, 0}, 0);
      const auto checkpoint = state.progress();
      const auto before = state.inspect();
      auto availability = std::array<std::uint32_t, 4>{};
      for (unsigned i = 0; i < 4; ++i)
         availability[i] = state.availability({0, i});
      const auto change = [&] {
         if (scenario == 0U)
            static_cast<void>(state.add_peer(std::array<std::uint8_t, 1>{15}));
         if (scenario == 1U)
            state.set_availability(peer, std::array<std::uint8_t, 1>{15});
         if (scenario == 2U)
            static_cast<void>(state.discard({0, 0}, now));
         if (scenario == 3U)
            state.set_priority({0, 0}, 255);
      };
      bool failed = false;
      allocations_before_failure = point;
      try {
         change();
      } catch (const std::bad_alloc&) {
         failed = true;
      }
      allocations_before_failure = -1;
      if (!failed) {
         reached_success = true;
         break;
      }
      ++failures;
      require(state.inspect().peers == before.peers && state.progress() == checkpoint,
              "index allocation failure changed peer/checkpoint state");
      for (unsigned i = 0; i < 4; ++i) {
         require(state.availability({0, i}) == availability[i], "index allocation failure changed rarity counts");
      }
      const auto retained = state.pick(peer, now, 1);
      require(retained.size() == 1U && retained.front().piece.index == (scenario < 2U ? 0U : 1U),
              "index allocation failure lost the original ordering");
      require(state.cancel(peer, retained.front().id, now), "index probe cancellation failed");
      change();
      const auto retry = state.pick(peer, now, 1);
      require(retry.size() == 1U && retry.front().piece.index == 0U,
              "index transition could not recover after allocation failure");
   }
   require(reached_success && failures != 0U, "index probe did not cover failure and success");
   return failures;
}

} // namespace

int main() {
   try {
      const auto descriptor = fixture();
      auto failures = choke_failures();
      for (unsigned scenario = 0; scenario < 4; ++scenario)
         failures += pick_failures(descriptor, scenario);
      for (unsigned scenario = 0; scenario < 4; ++scenario)
         failures += index_failures(descriptor, scenario);
      std::cout << "PASS: " << failures << " allocation failure points preserve observable transfer/choke state\n";
      return 0;
   } catch (const std::exception& error) {
      allocations_before_failure = -1;
      std::cerr << error.what() << '\n';
      return 1;
   }
}
