#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <vector>

import forge.swarm.core.transfer;
import forge.swarm.core.choker;

namespace transfer_test {
namespace core = forge::swarm::core;
using namespace std::chrono_literals;

core::transfer_time at(std::int64_t milliseconds) {
   return core::transfer_time{std::chrono::milliseconds{milliseconds}};
}

core::descriptor manifest(std::uint32_t pieces = 4, std::uint32_t blocks = 1, std::uint32_t trim = 0) {
   const auto size = pieces * blocks * core::block_size - trim;
   const auto bytes = std::vector<std::uint8_t>(size, 42U);
   const auto hashes = core::hash_file(bytes, blocks * core::block_size);
   return {core::descriptor_version::v1, blocks * core::block_size, {{{"payload"}, size, hashes.root, hashes.pieces}}};
}

core::peer_id connect(core::transfer& state, std::initializer_list<std::uint8_t> bitmap,
                      core::transfer_time now = at(0)) {
   const auto bytes = std::vector<std::uint8_t>(bitmap);
   const auto id = state.add_peer(bytes);
   BOOST_CHECK(state.set_choked(id, false, now).empty());
   return id;
}

void empty(const core::transfer& state) {
   const auto status = state.inspect();
   BOOST_TEST(status.requests == 0U);
   BOOST_TEST(status.request_bytes == 0U);
   BOOST_TEST(status.duplicate_requests == 0U);
   BOOST_TEST(status.unfinished_pieces == 0U);
   BOOST_TEST(status.unfinished_bytes == 0U);
}

core::choker::candidate candidate(core::peer_id id, std::uint64_t useful = 0) {
   return {id, true, {useful, useful, useful == 0U ? 0U : 1U, useful == 0U ? 0U : 5U}, 0U};
}

bool includes(const std::vector<core::peer_id>& peers, core::peer_id id) {
   return std::find(peers.begin(), peers.end(), id) != peers.end();
}

} // namespace transfer_test

BOOST_AUTO_TEST_SUITE(swarm_transfer)

BOOST_AUTO_TEST_CASE(availability_counts_rarest_sequential_and_explicit_priorities) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(), {}, 41U};
   const auto a = connect(state, {15U});
   const auto b = connect(state, {5U});
   const auto c = connect(state, {1U});
   BOOST_TEST(state.availability({0, 0}) == 3U);
   BOOST_TEST(state.availability({0, 1}) == 1U);
   BOOST_TEST(state.availability({0, 2}) == 2U);
   state.set_priority({0, 3}, 0U);
   auto picks = state.pick(a, at(0), 1U);
   BOOST_REQUIRE_EQUAL(picks.size(), 1U);
   BOOST_CHECK(picks[0].piece == (core::piece_id{0, 1}));
   BOOST_CHECK(state.cancel(a, picks[0].id, at(1)));
   state.set_selection(core::transfer::selection::sequential);
   picks = state.pick(a, at(1), 1U);
   BOOST_CHECK(picks[0].piece == (core::piece_id{0, 0}));
   BOOST_CHECK(state.cancel(a, picks[0].id, at(1)));
   state.set_priority({0, 2}, 200U);
   picks = state.pick(a, at(1), 1U);
   BOOST_CHECK(picks[0].piece == (core::piece_id{0, 2}));
   BOOST_CHECK(state.cancel(a, picks[0].id, at(1)));
   state.set_availability(b, std::array<std::uint8_t, 1>{8U});
   BOOST_TEST(state.availability({0, 0}) == 2U);
   BOOST_TEST(state.availability({0, 2}) == 1U);
   BOOST_TEST(state.availability({0, 3}) == 2U);
   BOOST_CHECK(state.remove_peer(c, at(2)).empty());
   BOOST_TEST(state.availability({0, 0}) == 1U);
   for (auto i = 0U; i < 4U; ++i)
      state.set_priority({0, i}, 0U);
   BOOST_CHECK(state.pick(a, at(2), 20U).empty());
   BOOST_CHECK(!state.inspect().complete);
}

BOOST_AUTO_TEST_CASE(equal_rarity_has_replayable_seeded_diversity) {
   using namespace transfer_test;
   auto first_pieces = std::set<std::uint32_t>{};
   const auto description = manifest(8U);
   for (auto seed = 0U; seed < 64U; ++seed) {
      auto left = core::transfer{description, {}, seed};
      auto right = core::transfer{description, {}, seed};
      const auto a = connect(left, {255U});
      const auto b = connect(right, {255U});
      const auto x = left.pick(a, at(0), 8U);
      const auto y = right.pick(b, at(0), 8U);
      BOOST_CHECK(x == y);
      BOOST_REQUIRE_EQUAL(x.size(), 8U);
      first_pieces.insert(x[0].piece.index);
      auto unique = std::set<core::piece_id>{};
      for (const auto& request : x)
         unique.insert(request.piece);
      BOOST_TEST(unique.size() == 8U);
   }
   BOOST_TEST(first_pieces.size() >= 6U);
}

BOOST_AUTO_TEST_CASE(partial_pressure_overrides_rarity_but_not_explicit_priority) {
   using namespace transfer_test;
   const auto description = manifest(4U, 2U);
   auto resume = core::resume_state{1U,   core::calculate_swarm_id(description),           0U, false,
                                    {0U}, {{{0, 0}, {1U}}, {{0, 1}, {1U}}, {{0, 2}, {1U}}}};
   auto limits = core::transfer::config{};
   limits.unfinished_pieces = 4U;
   auto state = core::transfer{description, limits, 9U, resume};
   const auto a = connect(state, {15U});
   static_cast<void>(connect(state, {7U}));
   BOOST_TEST(state.availability({0, 3}) == 1U);
   const auto picks = state.pick(a, at(0), 1U);
   BOOST_REQUIRE_EQUAL(picks.size(), 1U);
   BOOST_TEST(picks[0].piece.index < 3U);
   BOOST_TEST(picks[0].offset == core::block_size);
   BOOST_CHECK(state.cancel(a, picks[0].id, at(1)));
   state.set_priority({0, 3}, 255U);
   const auto high = state.pick(a, at(1), 1U);
   BOOST_REQUIRE_EQUAL(high.size(), 1U);
   BOOST_TEST(high[0].piece.index == 3U);
}

BOOST_AUTO_TEST_CASE(maintained_order_tracks_single_bit_identical_bitmap_priority_and_policy_changes) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(8U), {}, 77U};
   const auto a = connect(state, {255U});
   const auto b = connect(state, {127U});
   const auto first = state.pick(a, at(0), 1U).at(0);
   BOOST_TEST(first.piece.index == 7U);
   BOOST_CHECK(state.cancel(a, first.id, at(0)));
   const auto expect = [&](std::uint32_t index) {
      const auto request = state.pick(a, at(1), 1U).at(0);
      BOOST_TEST(request.piece.index == index);
      BOOST_CHECK(state.cancel(a, request.id, at(1)));
   };
   state.set_availability(b, std::array<std::uint8_t, 1>{247U});
   expect(3U);
   state.set_availability(b, std::array<std::uint8_t, 1>{247U});
   expect(3U);
   state.set_availability(b, std::array<std::uint8_t, 1>{251U});
   expect(2U);
   state.set_priority({0, 0}, 255U);
   expect(0U);
   state.set_priority({0, 0}, 0U);
   expect(2U);
   state.set_selection(core::transfer::selection::sequential);
   expect(1U);
   state.set_availability(b, std::array<std::uint8_t, 1>{249U});
   expect(1U);
   state.set_priority({0, 0}, 1U);
   expect(0U);
   BOOST_CHECK(state.remove_peer(b, at(1)).empty());
   state.set_selection(core::transfer::selection::rarest_first);
   auto reference = core::transfer{manifest(8U), {}, 77U};
   const auto other = connect(reference, {255U});
   const auto actual = state.pick(a, at(1), 8U);
   const auto expected = reference.pick(other, at(1), 8U);
   BOOST_REQUIRE_EQUAL(actual.size(), expected.size());
   for (auto i = std::size_t{}; i < actual.size(); ++i)
      BOOST_CHECK(actual[i].piece == expected[i].piece);
}

BOOST_AUTO_TEST_CASE(receive_and_cancel_update_partial_preference_without_rebuilding_global_order) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(4U, 2U), {}, 77U};
   const auto a = connect(state, {15U});
   const auto b = connect(state, {1U});
   state.set_selection(core::transfer::selection::sequential);
   const auto first = state.pick(a, at(0), 1U).at(0);
   BOOST_TEST(first.piece.index == 0U);
   BOOST_CHECK(state.receive(a, first.id, first.length, at(1)).accepted);
   state.set_selection(core::transfer::selection::rarest_first);
   const auto rare = state.pick(a, at(1), 1U).at(0);
   BOOST_TEST(rare.piece.index != 0U);
   BOOST_CHECK(state.cancel(a, rare.id, at(1)));
   const auto repeated = state.pick(a, at(1), 1U).at(0);
   BOOST_CHECK(repeated.piece == rare.piece);
   BOOST_CHECK(state.cancel(a, repeated.id, at(1)));
   // Equal rarity now promotes the received partial ahead of every fresh piece.
   state.set_availability(b, std::array<std::uint8_t, 1>{0U});
   const auto promoted = state.pick(a, at(1), 1U).at(0);
   BOOST_TEST(promoted.piece.index == 0U);
   BOOST_TEST(promoted.offset == core::block_size);
   const auto ready = state.receive(a, promoted.id, promoted.length, at(2));
   BOOST_REQUIRE(ready.ready);
   BOOST_CHECK(state.finish(*ready.ready, false) == core::transfer::verification_result::retry);
   empty(state);
   // Discard of an open request retains its piece in the maintained order.
   const auto retry = state.pick(a, at(2), 1U).at(0);
   BOOST_REQUIRE_EQUAL(state.discard(retry.piece, at(2)).size(), 1U);
   const auto replacement = state.pick(a, at(2), 1U).at(0);
   BOOST_CHECK(replacement.piece == retry.piece);
   BOOST_TEST(replacement.id > retry.id);
   BOOST_CHECK(!state.receive(a, retry.id, retry.length, at(2)).accepted);
}

BOOST_AUTO_TEST_CASE(verified_index_removal_and_discard_readmission_respect_filter_changes) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(12U), {}, 77U};
   state.set_selection(core::transfer::selection::sequential);
   const auto a = connect(state, {255U, 15U});
   for (auto i = 0U; i < 6U; ++i) {
      const auto request = state.pick(a, at(0), 1U).at(0);
      BOOST_TEST(request.piece.index == i);
      const auto received = state.receive(a, request.id, request.length, at(0));
      BOOST_REQUIRE(received.ready);
      BOOST_CHECK(state.finish(*received.ready, true) == core::transfer::verification_result::verified);
   }
   const auto b = connect(state, {63U, 0U});
   state.set_availability(b, std::array<std::uint8_t, 2>{63U, 0U});
   state.set_priority({0, 2}, 0U);
   BOOST_CHECK(state.discard({0, 2}, at(0)).empty());
   auto request = state.pick(a, at(0), 1U).at(0);
   BOOST_TEST(request.piece.index == 6U);
   BOOST_CHECK(state.cancel(a, request.id, at(0)));
   state.set_priority({0, 2}, 200U);
   request = state.pick(a, at(0), 1U).at(0);
   BOOST_TEST(request.piece.index == 2U);
   const auto received = state.receive(a, request.id, request.length, at(0));
   BOOST_REQUIRE(received.ready);
   BOOST_CHECK(state.finish(*received.ready, true) == core::transfer::verification_result::verified);
   state.set_priority({0, 2}, 1U);
   BOOST_CHECK(state.discard({0, 2}, at(0)).empty());
   request = state.pick(a, at(0), 1U).at(0);
   BOOST_TEST(request.piece.index == 2U);
   BOOST_CHECK(state.cancel(a, request.id, at(0)));
}

BOOST_AUTO_TEST_CASE(global_endgame_coverage_tracks_verification_discard_and_priority_filtering) {
   using namespace transfer_test;
   auto limits = core::transfer::config{};
   limits.endgame_replicas = 3U;
   auto state = core::transfer{manifest(2U), limits, 77U};
   state.set_selection(core::transfer::selection::sequential);
   const auto a = connect(state, {3U});
   const auto b = connect(state, {3U});
   const auto c = connect(state, {2U});
   const auto requests = state.pick(a, at(0), 2U);
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   const auto received = state.receive(a, requests[0].id, requests[0].length, at(0));
   BOOST_REQUIRE(received.ready);
   BOOST_CHECK(state.finish(*received.ready, true) == core::transfer::verification_result::verified);
   const auto duplicate = state.pick(b, at(0), 1U);
   BOOST_REQUIRE_EQUAL(duplicate.size(), 1U);
   BOOST_TEST(duplicate[0].piece.index == 1U);
   BOOST_CHECK(state.discard({0, 0}, at(0)).empty());
   BOOST_CHECK(state.pick(c, at(0), 1U).empty());
   state.set_priority({0, 0}, 0U);
   const auto next = state.pick(c, at(0), 1U);
   BOOST_REQUIRE_EQUAL(next.size(), 1U);
   BOOST_TEST(next[0].piece.index == 1U);
}

BOOST_AUTO_TEST_CASE(ownership_exact_geometry_and_explicit_piece_verification) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(1U, 2U, 17U), {}, 1U};
   const auto a = connect(state, {1U});
   const auto b = connect(state, {1U});
   const auto picks = state.pick(a, at(1), 2U);
   BOOST_REQUIRE_EQUAL(picks.size(), 2U);
   BOOST_TEST(picks[0].offset == 0U);
   BOOST_TEST(picks[0].length == core::block_size);
   BOOST_TEST(picks[1].offset == core::block_size);
   BOOST_TEST(picks[1].length == core::block_size - 17U);
   const auto before = state.progress();
   BOOST_CHECK_THROW((void)state.receive(b, picks[0].id, picks[0].length, at(100)), core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.cancel(b, picks[0].id, at(100)), core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.receive(a, picks[0].id, picks[0].length - 1U, at(100)),
                     core::exceptions::invalid_transfer);
   BOOST_CHECK(state.progress() == before);
   BOOST_TEST(state.inspect().requests == 2U);
   const auto first = state.receive(a, picks[0].id, picks[0].length, at(2));
   BOOST_CHECK(first.accepted);
   BOOST_CHECK(!first.ready);
   BOOST_TEST(state.observations(a).useful_bytes == 0U);
   const auto second = state.receive(a, picks[1].id, picks[1].length, at(3));
   BOOST_REQUIRE(second.ready);
   BOOST_CHECK(!state.verified({0, 0}));
   BOOST_CHECK(!state.inspect().complete);
   BOOST_TEST(state.inspect().request_bytes == 0U);
   BOOST_TEST(state.inspect().unfinished_bytes == 2U * core::block_size - 17U);
   BOOST_CHECK(state.finish(*second.ready, true) == core::transfer::verification_result::verified);
   BOOST_CHECK(state.finish(*second.ready, true) == core::transfer::verification_result::ignored);
   BOOST_CHECK(state.inspect().complete);
   BOOST_TEST(state.observations(a).useful_bytes == 2U * core::block_size - 17U);
   BOOST_TEST(state.observations(a).latency_milliseconds == 3U);
   empty(state);
}

BOOST_AUTO_TEST_CASE(endgame_does_not_start_on_an_isolated_busy_piece) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(2U), {}, 7U};
   state.set_selection(core::transfer::selection::sequential);
   const auto a = connect(state, {1U});
   const auto b = connect(state, {1U});
   const auto first = state.pick(a, at(0), 1U);
   BOOST_REQUIRE_EQUAL(first.size(), 1U);
   BOOST_CHECK(state.pick(b, at(0), 1U).empty());
   state.set_priority({0, 1}, 0U);
   const auto duplicate = state.pick(b, at(0), 10U);
   BOOST_REQUIRE_EQUAL(duplicate.size(), 1U);
   BOOST_CHECK(duplicate[0].piece == first[0].piece);
   BOOST_TEST(duplicate[0].id != first[0].id);
   BOOST_CHECK(state.pick(a, at(0), 10U).empty());
}

BOOST_AUTO_TEST_CASE(endgame_priority_precedes_copy_count_and_seed_rank) {
   using namespace transfer_test;
   auto limits = core::transfer::config{};
   limits.endgame_replicas = 3U;
   auto state = core::transfer{manifest(2U), limits, 41U};
   state.set_priority({0, 0}, 255U);
   const auto a = connect(state, {3U});
   const auto b = connect(state, {3U});
   const auto c = connect(state, {3U});
   BOOST_REQUIRE_EQUAL(state.pick(a, at(0), 2U).size(), 2U);
   const auto duplicate = state.pick(b, at(0), 2U);
   BOOST_REQUIRE_EQUAL(duplicate.size(), 1U);
   BOOST_TEST(duplicate[0].piece.index == 0U);
   const auto next = state.pick(c, at(0), 2U);
   BOOST_REQUIRE_EQUAL(next.size(), 1U);
   BOOST_TEST(next[0].piece.index == 0U);
   BOOST_TEST(state.inspect().duplicate_requests == 2U);
}

BOOST_AUTO_TEST_CASE(endgame_winner_cancel_and_late_response_all_orderings) {
   using namespace transfer_test;
   auto order = std::array{0U, 1U, 2U};
   do {
      auto state = core::transfer{manifest(1U), {}, 41U};
      const auto a = connect(state, {1U});
      const auto b = connect(state, {1U});
      const auto x = state.pick(a, at(0), 1U).at(0);
      const auto y = state.pick(b, at(0), 1U).at(0);
      BOOST_TEST(state.inspect().duplicate_requests == 1U);
      BOOST_TEST(state.inspect().request_bytes == 2U * core::block_size);
      auto accepted = 0U;
      auto ready = std::optional<core::transfer::verification>{};
      for (const auto event : order) {
         if (event == 2U) {
            static_cast<void>(state.cancel(a, x.id, at(1)));
            continue;
         }
         const auto& request = event == 0U ? x : y;
         const auto result = state.receive(request.peer, request.id, request.length, at(1));
         if (result.accepted) {
            ++accepted;
            ready = result.ready;
            for (const auto& canceled : result.canceled)
               BOOST_TEST(canceled.id != request.id);
         }
      }
      BOOST_TEST(accepted == 1U);
      BOOST_REQUIRE(ready);
      BOOST_TEST(state.inspect().requests == 0U);
      BOOST_TEST(state.inspect().duplicate_requests == 0U);
      BOOST_TEST(state.inspect().request_bytes == 0U);
      BOOST_CHECK(!state.cancel(b, y.id, at(2)));
      BOOST_CHECK(!state.receive(a, x.id, x.length, at(2)).accepted);
      BOOST_CHECK(state.finish(*ready, true) == core::transfer::verification_result::verified);
      BOOST_CHECK(state.finish(*ready, false) == core::transfer::verification_result::ignored);
      empty(state);
   } while (std::next_permutation(order.begin(), order.end()));
}

BOOST_AUTO_TEST_CASE(timeout_cancel_and_response_at_deadline_release_exactly_once) {
   using namespace transfer_test;
   auto order = std::array{0U, 1U, 2U};
   do {
      auto limits = core::transfer::config{};
      limits.request_timeout = 10ms;
      auto state = core::transfer{manifest(1U), limits, 6U};
      const auto a = connect(state, {1U});
      const auto request = state.pick(a, at(0), 1U).at(0);
      for (const auto event : order) {
         if (event == 0U)
            BOOST_CHECK(!state.receive(a, request.id, request.length, at(10)).accepted);
         if (event == 1U)
            static_cast<void>(state.expire(at(10)));
         if (event == 2U)
            static_cast<void>(state.cancel(a, request.id, at(10)));
      }
      BOOST_TEST(state.observations(a).timeouts == (order[0] == 2U ? 0U : 1U));
      BOOST_CHECK(!state.inspect().complete);
      empty(state);
      const auto replacement = state.pick(a, at(11), 1U).at(0);
      BOOST_TEST(replacement.id > request.id);
      BOOST_CHECK(!state.receive(a, request.id, request.length, at(12)).accepted);
      BOOST_TEST(state.inspect().requests == 1U);
      const auto result = state.receive(a, replacement.id, replacement.length, at(12));
      BOOST_REQUIRE(result.ready);
      BOOST_CHECK(state.finish(*result.ready, true) == core::transfer::verification_result::verified);
   } while (std::next_permutation(order.begin(), order.end()));
}

BOOST_AUTO_TEST_CASE(discard_retracts_verified_availability_and_rejects_old_verification) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(1U), {}, 1U};
   const auto a = connect(state, {1U});
   const auto request = state.pick(a, at(0), 1U).at(0);
   const auto first = state.receive(a, request.id, request.length, at(1));
   BOOST_REQUIRE(first.ready);
   BOOST_CHECK(state.discard({0, 0}, at(2)).empty());
   const auto retry = state.pick(a, at(2), 1U).at(0);
   const auto second = state.receive(a, retry.id, retry.length, at(3));
   BOOST_REQUIRE(second.ready);
   BOOST_TEST(second.ready->generation != first.ready->generation);
   BOOST_CHECK(state.finish(*first.ready, true) == core::transfer::verification_result::ignored);
   BOOST_CHECK(state.finish(*second.ready, true) == core::transfer::verification_result::verified);
   BOOST_CHECK(state.inspect().complete);
   BOOST_TEST(state.inspect().verified_pieces == 1U);
   BOOST_CHECK(state.discard({0, 0}, at(4)).empty());
   BOOST_CHECK(!state.verified({0, 0}));
   BOOST_CHECK(!state.inspect().complete);
   BOOST_TEST(state.inspect().verified_pieces == 0U);
   BOOST_CHECK(state.discard({0, 0}, at(4)).empty());
   BOOST_TEST(state.inspect().verified_pieces == 0U);
   BOOST_REQUIRE_EQUAL(state.pick(a, at(4), 1U).size(), 1U);
}

BOOST_AUTO_TEST_CASE(mixed_piece_corruption_has_no_arbitrary_peer_blame_and_retries) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(1U, 2U), {}, 1U};
   const auto a = connect(state, {1U});
   const auto b = connect(state, {1U});
   const auto first = state.pick(a, at(0), 1U).at(0);
   const auto second = state.pick(b, at(0), 1U).at(0);
   BOOST_TEST(first.offset != second.offset);
   BOOST_CHECK(state.receive(a, first.id, first.length, at(1)).accepted);
   const auto result = state.receive(b, second.id, second.length, at(1));
   BOOST_REQUIRE(result.ready);
   BOOST_CHECK(state.finish(*result.ready, false) == core::transfer::verification_result::retry);
   BOOST_TEST(state.inspect().integrity_failures == 1U);
   BOOST_TEST(state.inspect().unattributed_integrity_failures == 1U);
   BOOST_TEST(state.observations(a).integrity_failures == 0U);
   BOOST_TEST(state.observations(b).integrity_failures == 0U);
   BOOST_TEST(state.observations(a).useful_bytes == 0U);
   empty(state);
   const auto retry = state.pick(b, at(2), 2U);
   BOOST_REQUIRE_EQUAL(retry.size(), 2U);
   BOOST_CHECK(state.receive(b, retry[0].id, retry[0].length, at(3)).accepted);
   const auto complete = state.receive(b, retry[1].id, retry[1].length, at(3));
   BOOST_REQUIRE(complete.ready);
   BOOST_CHECK(state.finish(*result.ready, true) == core::transfer::verification_result::ignored);
   BOOST_CHECK(state.finish(*complete.ready, false) == core::transfer::verification_result::retry);
   BOOST_TEST(state.observations(b).integrity_failures == 1U);
   BOOST_TEST(state.observations(a).integrity_failures == 0U);
   empty(state);
}

BOOST_AUTO_TEST_CASE(received_but_unverified_reservations_bound_the_working_set) {
   using namespace transfer_test;
   auto limits = core::transfer::config{};
   limits.unfinished_pieces = 1U;
   limits.unfinished_bytes = 2U * core::block_size;
   auto state = core::transfer{manifest(2U, 2U), limits, 1U};
   state.set_selection(core::transfer::selection::sequential);
   const auto a = connect(state, {3U});
   const auto requests = state.pick(a, at(0), 100U);
   BOOST_REQUIRE_EQUAL(requests.size(), 2U);
   BOOST_TEST(state.inspect().unfinished_bytes == 2U * core::block_size);
   const auto x = state.receive(a, requests[0].id, requests[0].length, at(1));
   const auto y = state.receive(a, requests[1].id, requests[1].length, at(1));
   BOOST_CHECK(x.accepted);
   BOOST_REQUIRE(y.ready);
   BOOST_TEST(state.inspect().requests == 0U);
   BOOST_TEST(state.inspect().unfinished_pieces == 1U);
   BOOST_TEST(state.inspect().unfinished_bytes == limits.unfinished_bytes);
   BOOST_CHECK(state.pick(a, at(1), 100U).empty());
   BOOST_CHECK(state.finish(*y.ready, true) == core::transfer::verification_result::verified);
   BOOST_REQUIRE_EQUAL(state.pick(a, at(2), 100U).size(), 2U);
}

BOOST_AUTO_TEST_CASE(ordinary_and_endgame_share_global_and_per_peer_caps) {
   using namespace transfer_test;
   auto limits = core::transfer::config{};
   limits.requests = 2U;
   limits.requests_per_peer = 1U;
   limits.request_bytes = 2U * core::block_size;
   limits.request_bytes_per_peer = core::block_size;
   limits.endgame_requests = 1U;
   limits.endgame_replicas = 3U;
   auto state = core::transfer{manifest(1U), limits, 1U};
   const auto a = connect(state, {1U});
   const auto b = connect(state, {1U});
   const auto c = connect(state, {1U});
   const auto first = state.pick(a, at(0), 100U).at(0);
   BOOST_CHECK(state.pick(a, at(0), 100U).empty());
   const auto duplicate = state.pick(b, at(0), 100U).at(0);
   BOOST_CHECK(state.pick(c, at(0), 100U).empty());
   BOOST_TEST(state.inspect().requests == limits.requests);
   BOOST_TEST(state.inspect().request_bytes == limits.request_bytes);
   BOOST_CHECK(state.cancel(a, first.id, at(1)));
   BOOST_TEST(state.inspect().duplicate_requests == 0U);
   const auto replacement = state.pick(c, at(1), 100U).at(0);
   BOOST_TEST(state.inspect().duplicate_requests == 1U);
   BOOST_CHECK(!state.receive(a, first.id, first.length, at(2)).accepted);
   const auto winner = state.receive(c, replacement.id, replacement.length, at(2));
   BOOST_REQUIRE(winner.ready);
   BOOST_REQUIRE_EQUAL(winner.canceled.size(), 1U);
   BOOST_TEST(winner.canceled[0].id == duplicate.id);
   BOOST_CHECK(state.finish(*winner.ready, true) == core::transfer::verification_result::verified);
   empty(state);
}

BOOST_AUTO_TEST_CASE(byte_caps_allow_short_tail_without_admitting_full_block) {
   using namespace transfer_test;
   auto limits = core::transfer::config{};
   limits.request_bytes = 1U;
   limits.request_bytes_per_peer = 1U;
   auto state = core::transfer{manifest(2U, 1U, core::block_size - 1U), limits, 1U};
   state.set_selection(core::transfer::selection::sequential);
   const auto a = connect(state, {3U});
   const auto picks = state.pick(a, at(0), 100U);
   BOOST_REQUIRE_EQUAL(picks.size(), 1U);
   BOOST_TEST(picks[0].length == 1U);
   BOOST_TEST(picks[0].piece.index == 1U);
   BOOST_TEST(state.inspect().request_bytes == 1U);
   BOOST_CHECK(state.pick(a, at(0), 1U).empty());
}

BOOST_AUTO_TEST_CASE(stop_preserves_received_restart_evidence_but_invalidates_callbacks) {
   using namespace transfer_test;
   const auto description = manifest(2U, 2U);
   auto state = core::transfer{description, {}, 41U};
   state.set_selection(core::transfer::selection::sequential);
   const auto a = connect(state, {3U});
   const auto requests = state.pick(a, at(0), 4U);
   BOOST_REQUIRE_EQUAL(requests.size(), 4U);
   BOOST_CHECK(state.receive(a, requests[0].id, requests[0].length, at(1)).accepted);
   const auto ready = state.receive(a, requests[1].id, requests[1].length, at(1));
   BOOST_REQUIRE(ready.ready);
   BOOST_CHECK(state.receive(a, requests[2].id, requests[2].length, at(1)).accepted);
   const auto before = state.progress();
   const auto canceled = state.stop(at(2));
   BOOST_REQUIRE_EQUAL(canceled.size(), 1U);
   BOOST_TEST(canceled[0].id == requests[3].id);
   BOOST_CHECK(state.inspect().stopped);
   BOOST_CHECK(state.progress() == before);
   BOOST_TEST(state.inspect().requests == 0U);
   BOOST_TEST(state.inspect().request_bytes == 0U);
   BOOST_TEST(state.inspect().unfinished_bytes == 4U * core::block_size);
   BOOST_CHECK(!state.verification_for({0, 0}));
   BOOST_CHECK(state.finish(*ready.ready, true) == core::transfer::verification_result::ignored);
   BOOST_CHECK(!state.receive(a, requests[3].id, requests[3].length, at(3)).accepted);
   BOOST_CHECK(state.stop(at(3)).empty());
   BOOST_CHECK(state.pick(a, at(3), 1U).empty());
   core::validate_checkpoint(state.progress(), description);
   auto restarted = core::transfer{description, {}, 41U, state.progress()};
   const auto b = connect(restarted, {3U});
   const auto verification = restarted.verification_for({0, 0});
   BOOST_REQUIRE(verification);
   BOOST_CHECK(restarted.finish(*verification, false) == core::transfer::verification_result::retry);
   BOOST_TEST(restarted.inspect().unattributed_integrity_failures == 1U);
   BOOST_TEST(restarted.observations(b).integrity_failures == 0U);
}

BOOST_AUTO_TEST_CASE(choke_and_disconnect_cancel_only_their_owned_requests) {
   using namespace transfer_test;
   auto state = core::transfer{manifest(1U, 2U), {}, 41U};
   const auto a = connect(state, {1U});
   const auto b = connect(state, {1U});
   const auto x = state.pick(a, at(0), 1U).at(0);
   const auto y = state.pick(b, at(0), 1U).at(0);
   const auto canceled = state.set_choked(a, true, at(1));
   BOOST_REQUIRE_EQUAL(canceled.size(), 1U);
   BOOST_TEST(canceled[0].id == x.id);
   BOOST_TEST(state.inspect().requests == 1U);
   BOOST_CHECK(state.pick(a, at(1), 1U).empty());
   BOOST_CHECK(!state.receive(a, x.id, x.length, at(2)).accepted);
   const auto disconnected = state.remove_peer(b, at(2));
   BOOST_REQUIRE_EQUAL(disconnected.size(), 1U);
   BOOST_TEST(disconnected[0].id == y.id);
   BOOST_TEST(state.availability({0, 0}) == 1U);
   BOOST_CHECK(!state.receive(b, y.id, y.length, at(3)).accepted);
   const auto c = connect(state, {1U}, at(3));
   BOOST_TEST(c > b);
   empty(state);
}

BOOST_AUTO_TEST_CASE(long_run_cancellation_timeouts_retries_and_churn_do_not_retain_history) {
   using namespace transfer_test;
   auto limits = core::transfer::config{};
   limits.request_timeout = 2ms;
   auto state = core::transfer{manifest(1U), limits, 41U};
   auto last_id = std::uint64_t{};
   for (auto round = 0; round < 3000; ++round) {
      const auto time = round * 4;
      const auto a = connect(state, {1U}, at(time));
      const auto b = connect(state, {1U}, at(time));
      const auto x = state.pick(a, at(time), 1U).at(0);
      const auto y = state.pick(b, at(time), 1U).at(0);
      BOOST_TEST(x.id > last_id);
      last_id = y.id;
      if (round % 3 == 0) {
         const auto result = state.receive(a, x.id, x.length, at(time + 1));
         BOOST_REQUIRE(result.ready);
         BOOST_CHECK(state.finish(*result.ready, false) == core::transfer::verification_result::retry);
      } else if (round % 3 == 1) {
         BOOST_REQUIRE_EQUAL(state.expire(at(time + 2)).size(), 2U);
      } else {
         BOOST_REQUIRE_EQUAL(state.discard({0, 0}, at(time + 2)).size(), 2U);
      }
      BOOST_CHECK(!state.receive(b, y.id, y.length, at(time + 3)).accepted);
      BOOST_CHECK(state.remove_peer(a, at(time + 3)).empty());
      BOOST_CHECK(state.remove_peer(b, at(time + 3)).empty());
      BOOST_TEST(state.inspect().peers == 0U);
      empty(state);
      BOOST_CHECK(state.progress().unfinished.empty());
   }
}

BOOST_AUTO_TEST_CASE(malformed_inputs_and_rejected_times_leave_state_usable) {
   using namespace transfer_test;
   const auto description = manifest(1U);
   for (auto field = 0U; field < 12U; ++field) {
      auto limits = core::transfer::config{};
      if (field == 0U)
         limits.peers = 4097U;
      if (field == 1U)
         limits.requests = 65537U;
      if (field == 2U)
         limits.requests_per_peer = 0U;
      if (field == 3U)
         limits.request_bytes = std::numeric_limits<std::uint64_t>::max();
      if (field == 4U)
         limits.request_bytes_per_peer = 0U;
      if (field == 5U)
         limits.unfinished_pieces = core::max_unfinished_pieces + 1U;
      if (field == 6U)
         limits.unfinished_bytes = std::numeric_limits<std::uint64_t>::max();
      if (field == 7U)
         limits.request_timeout = 0ms;
      if (field == 8U)
         limits.endgame_blocks = 4097U;
      if (field == 9U)
         limits.endgame_replicas = 0U;
      if (field == 10U)
         limits.endgame_replicas = 5U;
      if (field == 11U)
         limits.endgame_requests = limits.requests + 1U;
      BOOST_CHECK_THROW((core::transfer{description, limits, 1U}), core::exceptions::invalid_transfer);
   }
   auto state = core::transfer{description, {}, 1U};
   BOOST_CHECK_THROW((void)state.add_peer(std::array<std::uint8_t, 1>{2U}), core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.add_peer(std::array<std::uint8_t, 2>{1U, 0U}), core::exceptions::invalid_transfer);
   const auto a = connect(state, {1U});
   BOOST_CHECK_THROW(state.set_availability(a, std::array<std::uint8_t, 1>{128U}), core::exceptions::invalid_transfer);
   BOOST_TEST(state.availability({0, 0}) == 1U);
   BOOST_CHECK_THROW(state.set_priority({1, 0}, 1U), core::exceptions::invalid_geometry);
   BOOST_CHECK_THROW(state.set_selection(static_cast<core::transfer::selection>(77)),
                     core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.pick(a, core::transfer_time::max(), 1U), core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.pick(a + 1U, at(999), 1U), core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.set_choked(a + 1U, true, at(999)), core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.discard({0, 1}, at(999)), core::exceptions::invalid_geometry);
   const auto request = state.pick(a, at(1), 1U).at(0);
   BOOST_CHECK_THROW((void)state.expire(at(0)), core::exceptions::invalid_transfer);
   BOOST_REQUIRE_EQUAL(state.stop(at(2)).size(), 1U);
   BOOST_CHECK(!state.receive(a, request.id, request.length, at(3)).accepted);
   empty(state);
}

BOOST_AUTO_TEST_CASE(peer_and_resume_metadata_have_independent_hard_bounds) {
   using namespace transfer_test;
   const auto description = manifest(2U, 2U);
   auto limits = core::transfer::config{};
   limits.peers = 1U;
   auto state = core::transfer{description, limits, 1U};
   static_cast<void>(connect(state, {3U}));
   BOOST_CHECK_THROW((void)state.add_peer(std::array<std::uint8_t, 1>{3U}), core::exceptions::limit_exceeded);
   auto resume = state.progress();
   resume.unfinished = {{{0, 0}, {1U}}, {{0, 1}, {1U}}};
   limits.unfinished_pieces = 1U;
   BOOST_CHECK_THROW((core::transfer{description, limits, 1U, resume}), core::exceptions::limit_exceeded);
   limits.unfinished_pieces = 2U;
   limits.unfinished_bytes = 2U * core::block_size;
   BOOST_CHECK_THROW((core::transfer{description, limits, 1U, resume}), core::exceptions::limit_exceeded);
   resume.unfinished.clear();
   resume.verified[0] = 128U;
   BOOST_CHECK_THROW((core::transfer{description, limits, 1U, resume}), core::exceptions::invalid_checkpoint);
}

BOOST_AUTO_TEST_CASE(empty_and_multiple_files_keep_existing_file_local_geometry) {
   using namespace transfer_test;
   auto description = manifest(1U, 1U, core::block_size - 1U);
   auto tail = description.files[0];
   tail.path = {"z"};
   description.files.insert(description.files.begin(), core::file_entry{{"a"}, 0U, {}, {}});
   description.files.push_back(tail);
   auto state = core::transfer{description, {}, 1U};
   state.set_selection(core::transfer::selection::sequential);
   const auto a = connect(state, {3U});
   const auto picks = state.pick(a, at(0), 10U);
   BOOST_REQUIRE_EQUAL(picks.size(), 2U);
   BOOST_CHECK(picks[0].piece == (core::piece_id{1, 0}));
   BOOST_CHECK(picks[1].piece == (core::piece_id{2, 0}));
   BOOST_TEST(picks[0].length == 1U);
   const auto all_empty = core::descriptor{core::descriptor_version::v1, core::block_size, {{{"a"}, 0U, {}, {}}}};
   auto empty_state = core::transfer{all_empty, {}, 1U};
   BOOST_CHECK(empty_state.inspect().complete);
   const auto b = connect(empty_state, {});
   BOOST_CHECK(empty_state.pick(b, at(0), 10U).empty());
   core::validate_checkpoint(empty_state.progress(), all_empty);
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(swarm_choker)

BOOST_AUTO_TEST_CASE(useful_throughput_uses_round_deltas_not_lifetime_totals) {
   using namespace transfer_test;
   auto limits = core::choker::config{};
   limits.slots = 1U;
   limits.optimistic = false;
   auto state = core::choker{limits, 41U};
   auto peers = std::array{candidate(1U, 100U), candidate(2U, 200U), candidate(3U)};
   auto plan = state.select(peers, at(0), false);
   BOOST_CHECK(plan.unchoked == std::vector<core::peer_id>{2U});
   peers[0].observation.received_bytes = peers[0].observation.useful_bytes = 400U;
   peers[1].observation.received_bytes = peers[1].observation.useful_bytes = 201U;
   plan = state.select(peers, at(10), false);
   BOOST_CHECK(plan.unchoked == std::vector<core::peer_id>{1U});
   BOOST_CHECK(!plan.optimistic);
   BOOST_TEST(plan.choked.size() == 2U);
}

BOOST_AUTO_TEST_CASE(timeout_integrity_and_latency_are_tie_inputs_not_admission_bans) {
   using namespace transfer_test;
   auto limits = core::choker::config{};
   limits.slots = 1U;
   limits.optimistic = false;
   for (auto dimension = 0U; dimension < 3U; ++dimension) {
      auto state = core::choker{limits, 41U};
      auto peers = std::array{candidate(1U, 100U), candidate(2U, 100U)};
      if (dimension == 0U)
         peers[1].observation.integrity_failures = 1U;
      if (dimension == 1U)
         peers[1].observation.timeouts = 1U;
      if (dimension == 2U)
         peers[1].observation.latency_milliseconds = 50U;
      BOOST_CHECK(state.select(peers, at(0), false).unchoked == std::vector<core::peer_id>{1U});
      peers[0].interested = false;
      const auto next = state.select(peers, at(1), false);
      BOOST_CHECK(next.unchoked == std::vector<core::peer_id>{2U});
      BOOST_CHECK(next.choked == std::vector<core::peer_id>{1U});
   }
}

BOOST_AUTO_TEST_CASE(optimistic_slot_is_inside_capacity_and_rotates_deterministically) {
   using namespace transfer_test;
   auto limits = core::choker::config{};
   limits.slots = 2U;
   limits.optimistic_interval = 10ms;
   auto left = core::choker{limits, 41U};
   auto right = core::choker{limits, 41U};
   auto peers = std::array{candidate(1U, 100U), candidate(2U), candidate(3U), candidate(4U)};
   auto reversed = peers;
   std::reverse(reversed.begin(), reversed.end());
   const auto initial = left.select(peers, at(0), false);
   BOOST_CHECK(initial == right.select(reversed, at(0), false));
   BOOST_REQUIRE(initial.optimistic);
   BOOST_REQUIRE_EQUAL(initial.unchoked.size(), 2U);
   BOOST_CHECK(includes(initial.unchoked, 1U));
   BOOST_CHECK(includes(initial.unchoked, *initial.optimistic));
   peers[0].observation.received_bytes = peers[0].observation.useful_bytes = 200U;
   const auto kept = left.select(peers, at(9), false);
   BOOST_CHECK(kept.optimistic == initial.optimistic);
   peers[0].observation.received_bytes = peers[0].observation.useful_bytes = 300U;
   const auto rotated = left.select(peers, at(10), false);
   BOOST_REQUIRE(rotated.optimistic);
   BOOST_CHECK(rotated.optimistic != initial.optimistic);
   BOOST_REQUIRE_EQUAL(rotated.unchoked.size(), 2U);
   BOOST_CHECK(includes(rotated.unchoked, 1U));
   for (auto& peer : peers)
      if (peer.peer == *rotated.optimistic)
         peer.interested = false;
   const auto uninterested = left.select(peers, at(11), false);
   BOOST_CHECK(!includes(uninterested.unchoked, *rotated.optimistic));
   BOOST_TEST(uninterested.unchoked.size() <= 2U);
}

BOOST_AUTO_TEST_CASE(seed_slot_rotates_after_both_quota_and_duration_without_reciprocity) {
   using namespace transfer_test;
   auto limits = core::choker::config{};
   limits.slots = 1U;
   limits.optimistic = false;
   limits.seed_rotation_interval = 10ms;
   limits.seed_quota_bytes = 100U;
   auto state = core::choker{limits, 41U};
   auto peers = std::array{candidate(1U), candidate(2U), candidate(3U)};
   const auto initial = state.select(peers, at(0), true);
   BOOST_REQUIRE_EQUAL(initial.unchoked.size(), 1U);
   const auto first = initial.unchoked[0];
   for (auto& peer : peers)
      if (peer.peer == first)
         peer.served_bytes = 200U;
   const auto before_interval = state.select(peers, at(1), true);
   BOOST_CHECK(before_interval.unchoked == initial.unchoked);
   for (auto& peer : peers)
      if (peer.peer == first)
         peer.served_bytes = 400U;
   const auto rotated = state.select(peers, at(10), true);
   BOOST_REQUIRE_EQUAL(rotated.unchoked.size(), 1U);
   BOOST_TEST(rotated.unchoked[0] != first);
   // A provider-only peer with no useful download still gets a serving slot.
   for (const auto& peer : peers)
      BOOST_TEST(peer.observation.useful_bytes == 0U);
}

BOOST_AUTO_TEST_CASE(long_running_churn_has_no_peer_tombstones_and_zero_slots_choke_all) {
   using namespace transfer_test;
   auto limits = core::choker::config{};
   limits.peers = 2U;
   limits.slots = 1U;
   limits.optimistic_interval = 1ms;
   auto state = core::choker{limits, 2U};
   for (auto i = 0U; i < 1000U; ++i) {
      const auto peers = std::array{candidate(2U * i + 1U), candidate(2U * i + 2U)};
      const auto plan = state.select(peers, at(i), true);
      BOOST_REQUIRE_EQUAL(plan.unchoked.size(), 1U);
      BOOST_REQUIRE_EQUAL(plan.choked.size(), 1U);
      BOOST_REQUIRE(plan.optimistic);
      BOOST_TEST(state.tracked_peers() == 2U);
   }
   BOOST_CHECK(state.select({}, at(1001), true).unchoked.empty());
   BOOST_TEST(state.tracked_peers() == 0U);
   limits.slots = 0U;
   auto disabled = core::choker{limits, 1U};
   const auto peers = std::array{candidate(1U), candidate(2U)};
   const auto plan = disabled.select(peers, at(0), false);
   BOOST_CHECK(plan.unchoked.empty());
   BOOST_CHECK(!plan.optimistic);
   BOOST_TEST(plan.choked.size() == 2U);
}

BOOST_AUTO_TEST_CASE(malformed_rounds_and_counter_regression_do_not_consume_state_or_time) {
   using namespace transfer_test;
   auto limits = core::choker::config{};
   limits.peers = 2U;
   limits.slots = 1U;
   auto state = core::choker{limits, 41U};
   auto peers = std::array{candidate(1U, 100U), candidate(2U, 100U)};
   const auto initial = state.select(peers, at(0), false);
   BOOST_REQUIRE_EQUAL(initial.unchoked.size(), 1U);
   auto bad = peers;
   bad[1].peer = bad[0].peer;
   BOOST_CHECK_THROW((void)state.select(bad, at(999), false), core::exceptions::invalid_transfer);
   bad = peers;
   bad[1].peer = 0U;
   BOOST_CHECK_THROW((void)state.select(bad, at(999), false), core::exceptions::invalid_transfer);
   bad = peers;
   bad[0].served_bytes = 0U;
   bad[0].observation.received_bytes = bad[0].observation.useful_bytes = 99U;
   BOOST_CHECK_THROW((void)state.select(bad, at(999), false), core::exceptions::invalid_transfer);
   bad = peers;
   bad[0].observation.useful_bytes = 101U;
   BOOST_CHECK_THROW((void)state.select(bad, at(999), false), core::exceptions::invalid_transfer);
   const auto too_many = std::array{candidate(1U), candidate(2U), candidate(3U)};
   BOOST_CHECK_THROW((void)state.select(too_many, at(999), false), core::exceptions::limit_exceeded);
   BOOST_CHECK_THROW((void)state.select(peers, at(0), false), core::exceptions::invalid_transfer);
   BOOST_CHECK_THROW((void)state.select(peers, at(-1), false), core::exceptions::invalid_transfer);
   BOOST_TEST(state.tracked_peers() == 2U);
   BOOST_CHECK_NO_THROW((void)state.select(peers, at(1), false));
   for (auto field = 0U; field < 5U; ++field) {
      auto invalid = limits;
      if (field == 0U)
         invalid.peers = 4097U;
      if (field == 1U)
         invalid.slots = limits.peers + 1U;
      if (field == 2U)
         invalid.optimistic_interval = 0ms;
      if (field == 3U)
         invalid.seed_rotation_interval = 0ms;
      if (field == 4U)
         invalid.seed_quota_bytes = 0U;
      BOOST_CHECK_THROW((core::choker{invalid, 41U}), core::exceptions::invalid_transfer);
   }
}

BOOST_AUTO_TEST_SUITE_END()
