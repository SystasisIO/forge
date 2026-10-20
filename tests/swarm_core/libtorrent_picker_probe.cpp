#include <libtorrent/bitfield.hpp>
#include <libtorrent/download_priority.hpp>
#include <libtorrent/performance_counters.hpp>
#include <libtorrent/piece_picker.hpp>
#include <libtorrent/random.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

import forge.swarm.core.transfer;

namespace core = forge::swarm::core;
namespace donor = libtorrent;

namespace {

core::descriptor fixture(std::uint32_t pieces) {
   const auto bytes = std::vector<std::uint8_t>(pieces * 4U * core::block_size, 7U);
   const auto hashes = core::hash_file(bytes, 4U * core::block_size);
   return {core::descriptor_version::v1, 4U * core::block_size, {{{"data"}, bytes.size(), hashes.root, hashes.pieces}}};
}

donor::piece_block pick(donor::piece_picker& picker, donor::picker_options_t options) {
   auto available = donor::typed_bitfield<donor::piece_index_t>{5, true};
   auto result = std::vector<donor::piece_block>{};
   auto counters = donor::counters{};
   picker.pick_pieces(available, result, 1, 0, nullptr, options, {}, 5, counters);
   if (result.size() != 1U)
      throw std::runtime_error{"donor did not produce exactly one candidate"};
   return result.front();
}

void compare(const core::transfer::request& actual, const donor::piece_block& expected) {
   if (actual.piece.file != 0U ||
       actual.piece.index != static_cast<std::uint32_t>(static_cast<int>(expected.piece_index)) ||
       actual.offset != static_cast<std::uint32_t>(expected.block_index) * core::block_size ||
       actual.length != core::block_size) {
      throw std::runtime_error{"picker differs from unchanged libtorrent on unambiguous candidate"};
   }
}

void measure_maximum_descriptor() {
   const auto count = core::max_piece_count;
   const auto size = std::uint64_t{count} * core::block_size;
   const auto layer = std::vector<core::sha256>(count, core::sha256{});
   const auto root = core::root_from_piece_layer(layer, size, core::block_size);
   const auto descriptor =
       core::descriptor{core::descriptor_version::v1, core::block_size, {{{"max"}, size, root, layer}}};
   auto actual = core::transfer{descriptor, {}, 41};
   const auto available = std::vector<std::uint8_t>(count / 8U, 255U);
   const auto peer = actual.add_peer(available);
   auto other_bitmap = std::vector<std::uint8_t>(count / 8U, 0U);
   const auto other = actual.add_peer(other_bitmap);
   const auto now = core::transfer_time{};
   static_cast<void>(actual.set_choked(peer, false, now));
   auto expected = donor::piece_picker{static_cast<std::int64_t>(size), static_cast<int>(core::block_size)};
   auto donor_available = donor::typed_bitfield<donor::piece_index_t>{static_cast<int>(count), true};
   for (unsigned i = 0; i < count; ++i)
      expected.inc_refcount(donor::piece_index_t{static_cast<int>(i)}, nullptr);
   // Both selectors enter the timing loop with their initial index prepared.
   // Libtorrent builds its lazy index on first pick, unlike Forge's add_peer.
   const auto warm_requests = actual.pick(peer, now, 16);
   for (const auto& request : warm_requests)
      static_cast<void>(actual.cancel(peer, request.id, now));
   auto warm_selected = std::vector<donor::piece_block>{};
   auto warm_counters = donor::counters{};
   expected.pick_pieces(donor_available, warm_selected, 16, 0, nullptr, donor::piece_picker::rarest_first, {}, 1,
                        warm_counters);
   if (warm_requests.size() != 16U || warm_selected.size() != 16U) {
      throw std::runtime_error{"maximum descriptor warmup failed"};
   }
   constexpr auto rounds = 32U;
   for (unsigned mode = 0; mode < 3; ++mode) {
      auto forge_elapsed = std::chrono::nanoseconds{};
      auto donor_elapsed = std::chrono::nanoseconds{};
      for (unsigned i = 0; i < rounds; ++i) {
         if (mode == 2U)
            other_bitmap[i / 8U] |= static_cast<std::uint8_t>(1U << (i % 8U));
         auto start = std::chrono::steady_clock::now();
         if (mode != 0U)
            actual.set_availability(other, other_bitmap);
         const auto requests = actual.pick(peer, now, 16);
         forge_elapsed += std::chrono::steady_clock::now() - start;
         if (requests.size() != 16U)
            throw std::runtime_error{"maximum descriptor pipeline stalled"};
         for (const auto& request : requests) {
            if (!actual.cancel(peer, request.id, now))
               throw std::runtime_error{"maximum descriptor cancellation failed"};
         }
         auto selected = std::vector<donor::piece_block>{};
         auto counters = donor::counters{};
         start = std::chrono::steady_clock::now();
         if (mode == 2U)
            expected.inc_refcount(donor::piece_index_t{static_cast<int>(i)}, nullptr);
         expected.pick_pieces(donor_available, selected, 16, 0, nullptr, donor::piece_picker::rarest_first, {}, 1,
                              counters);
         donor_elapsed += std::chrono::steady_clock::now() - start;
         if (selected.size() != 16U)
            throw std::runtime_error{"maximum descriptor donor pipeline stalled"};
         for (const auto& block : selected) {
            if (!expected.mark_as_downloading(block, nullptr))
               throw std::runtime_error{"maximum descriptor donor admission failed"};
            expected.abort_download(block, nullptr);
         }
      }
      // Measurements are evidence, not a machine-dependent timing assertion.
      const auto label = mode == 0U ? "stable inventory" : mode == 1U ? "unchanged bitmap" : "single-bit update";
      std::cout << "PROFILE: " << count << " pieces, " << rounds << " batches of 16, " << label
                << "; Forge update/pick "
                << std::chrono::duration_cast<std::chrono::microseconds>(forge_elapsed).count() << " us, donor pick "
                << std::chrono::duration_cast<std::chrono::microseconds>(donor_elapsed).count() << " us\n";
   }
}

void measure_exhausted_budget() {
   // Four one-TiB files reach both 262144 pieces and 1024 blocks per piece
   // without allocating payload. Valid piece layers supply metadata geometry.
   const auto layer = std::vector<core::sha256>(core::max_file_size / core::max_piece_size, core::sha256{});
   const auto root = core::root_from_piece_layer(layer, core::max_file_size, core::max_piece_size);
   auto descriptor = core::descriptor{core::descriptor_version::v1, core::max_piece_size, {}};
   for (unsigned i = 0; i < 4; ++i) {
      descriptor.files.push_back({{"file" + std::to_string(i)}, core::max_file_size, root, layer});
   }
   for (unsigned residual = 0; residual < 2; ++residual) {
      auto limits = core::transfer::config{};
      limits.requests_per_peer = 32;
      limits.request_bytes = limits.request_bytes_per_peer = core::block_size + residual;
      auto state = core::transfer{descriptor, limits, 41};
      state.set_selection(core::transfer::selection::sequential);
      const auto bitmap = std::vector<std::uint8_t>(core::max_piece_count / 8U, 255U);
      const auto peer = state.add_peer(bitmap);
      const auto now = core::transfer_time{};
      static_cast<void>(state.set_choked(peer, false, now));
      const auto start = std::chrono::steady_clock::now();
      const auto first = state.pick(peer, now, 32);
      if (first.size() != 1U)
         throw std::runtime_error{"byte-limited batch must admit exactly one full block"};
      for (unsigned i = 0; i < 4; ++i) {
         if (!state.pick(peer, now, 32).empty())
            throw std::runtime_error{"byte-limited batch over-admitted"};
      }
      const auto elapsed = std::chrono::steady_clock::now() - start;
      if (state.inspect().requests != 1U || state.inspect().request_bytes != core::block_size ||
          state.inspect().unfinished_pieces != 1U || !state.cancel(peer, first.front().id, now)) {
         throw std::runtime_error{"byte-limited batch damaged ownership or resource accounting"};
      }
      std::cout << "PROFILE: 262144 pieces x 1024 blocks, remaining byte budget " << residual
                << ", admission plus four blocked picks "
                << std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count() << " us\n";
   }
}

} // namespace

unsigned compare_libtorrent_picker() {
   const auto descriptor = fixture(5U);
   const auto now = core::transfer_time{std::chrono::milliseconds{1}};
   unsigned cases = 0;
   for (unsigned scenario = 0; scenario < 4; ++scenario) {
      auto limits = core::transfer::config{};
      if (scenario == 3U)
         limits.unfinished_pieces = 1;
      auto actual = core::transfer{descriptor, limits, 41};
      auto expected = donor::piece_picker{5LL * descriptor.piece_size, static_cast<int>(descriptor.piece_size)};
      donor::aux::random_engine().seed(41);
      const auto bitmaps = scenario == 0U ? std::vector<std::uint8_t>{31, 15, 7, 3, 1} : std::vector<std::uint8_t>{31};
      core::peer_id target = 0;
      for (const auto bitmap : bitmaps) {
         const auto id = actual.add_peer(std::array{bitmap});
         if (target == 0)
            target = id;
         for (unsigned piece = 0; piece < 5; ++piece) {
            if ((bitmap & (1U << piece)) != 0U)
               expected.inc_refcount(donor::piece_index_t{static_cast<int>(piece)}, nullptr);
         }
      }
      static_cast<void>(actual.set_choked(target, false, now));
      auto mode = donor::piece_picker::rarest_first;
      if (scenario == 1U || scenario == 2U) {
         actual.set_selection(core::transfer::selection::sequential);
         mode = donor::piece_picker::sequential;
      }
      if (scenario == 1U) {
         actual.set_priority({0, 0}, 0);
         expected.set_piece_priority(donor::piece_index_t{0}, donor::dont_download);
      }
      if (scenario == 2U || scenario == 3U) {
         actual.set_priority({0, 3}, 7);
         expected.set_piece_priority(donor::piece_index_t{3}, donor::download_priority_t{7});
      }
      const auto requests = actual.pick(target, now, 1);
      if (requests.size() != 1U)
         throw std::runtime_error{"Forge did not produce exactly one candidate"};
      const auto block = pick(expected, mode);
      compare(requests.front(), block);
      ++cases;
      if (scenario == 3U) {
         if (!expected.mark_as_downloading(block, nullptr))
            throw std::runtime_error{"donor request admission failed"};
         actual.set_priority({0, 3}, 1);
         expected.set_piece_priority(donor::piece_index_t{3}, donor::default_priority);
         const auto next = actual.pick(target, now, 1);
         if (next.size() != 1U)
            throw std::runtime_error{"Forge partial-piece candidate missing"};
         compare(next.front(), pick(expected, mode | donor::piece_picker::prioritize_partials));
         ++cases;
      }
   }
   // The donor picker proposes work; mark_as_downloading is its admission step.
   // Forge's pick performs both, so compare after matching ownership transitions.
   auto actual = core::transfer{descriptor, {}, 83};
   auto expected = donor::piece_picker{5LL * descriptor.piece_size, static_cast<int>(descriptor.piece_size)};
   const auto peer = actual.add_peer(std::array<std::uint8_t, 1>{31});
   static_cast<void>(actual.set_choked(peer, false, now));
   actual.set_selection(core::transfer::selection::sequential);
   for (int piece = 0; piece < 5; ++piece)
      expected.inc_refcount(donor::piece_index_t{piece}, nullptr);
   auto admitted = std::vector<core::transfer::request>{};
   auto donor_admitted = std::vector<donor::piece_block>{};
   for (unsigned i = 0; i < 12; ++i) {
      const auto selected = actual.pick(peer, now, 1);
      if (selected.size() != 1U)
         throw std::runtime_error{"sequential Forge pipeline stalled"};
      const auto block = pick(expected, donor::piece_picker::sequential);
      compare(selected.front(), block);
      if (!expected.mark_as_downloading(block, nullptr))
         throw std::runtime_error{"donor pipeline admission failed"};
      admitted.push_back(selected.front());
      donor_admitted.push_back(block);
      ++cases;
   }
   if (!actual.cancel(peer, admitted[5].id, now))
      throw std::runtime_error{"Forge cancel lost request ownership"};
   expected.abort_download(donor_admitted[5], nullptr);
   const auto replacement = actual.pick(peer, now, 1);
   if (replacement.size() != 1U || replacement.front().id == admitted[5].id) {
      throw std::runtime_error{"canceled block must receive a new Forge request identity"};
   }
   compare(replacement.front(), pick(expected, donor::piece_picker::sequential));
   ++cases;
   if (actual.receive(peer, admitted[5].id, admitted[5].length, now).accepted) {
      throw std::runtime_error{"late canceled response revived a retired request"};
   }
   measure_maximum_descriptor();
   measure_exhausted_budget();
   return cases;
}
