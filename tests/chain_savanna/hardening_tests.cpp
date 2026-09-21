#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

import forge.chain.savanna.finalizer_safety;
import forge.chain.savanna.vote_accumulator;
import forge.chain.savanna.validation;
import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.exceptions;
import forge.raw.raw;

namespace {

namespace savanna = forge::chain::savanna;
namespace bls = forge::crypto::bls;

savanna::digest hardening_digest(std::uint64_t value) {
   auto result = savanna::digest{};
   result._hash[0] = value;
   return result;
}

bls::private_key hardening_key(std::uint8_t base) {
   auto seed = std::array<std::uint8_t, 32>{};
   for (auto index = std::size_t{}; index < seed.size(); ++index) {
      seed[index] = static_cast<std::uint8_t>(base + index);
   }
   return bls::private_key{std::span<const std::uint8_t>{seed}};
}

savanna::finalizer hardening_finalizer(
    const bls::private_key& key, std::uint64_t weight,
    std::string description) {
   return {
       .description = std::move(description),
       .weight = weight,
       .public_key = key.get_public_key(),
   };
}

savanna::finalizer_vote make_vote(
    const savanna::block_ref& candidate, const bls::private_key& key,
    savanna::vote_kind kind) {
   const auto message = savanna::message_for_vote(candidate.finality_digest, kind);
   return {
       .block = candidate.id,
       .finalizer = key.get_public_key(),
       .kind = kind,
       .signature = key.sign(message),
   };
}

savanna::qc_signature make_qc_signature(
    const savanna::block_ref& candidate, std::size_t policy_size,
    std::span<const std::reference_wrapper<const bls::private_key>> keys,
    std::span<const std::size_t> indices,
    std::span<const savanna::vote_kind> kinds) {
   BOOST_REQUIRE_EQUAL(keys.size(), kinds.size());
   BOOST_REQUIRE_EQUAL(keys.size(), indices.size());
   auto strong = savanna::vote_bitset{policy_size};
   auto weak = savanna::vote_bitset{policy_size};
   auto signature = bls::signature_accumulator{};
   for (auto index = std::size_t{}; index < keys.size(); ++index) {
      const auto message =
          savanna::message_for_vote(candidate.finality_digest, kinds[index]);
      signature.add(keys[index].get().sign(message));
      (kinds[index] == savanna::vote_kind::strong ? strong : weak)
          .set(indices[index]);
   }
   return {
       .strong_votes = strong.any() ? std::optional<savanna::vote_bitset>{std::move(strong)} : std::nullopt,
       .weak_votes = weak.any() ? std::optional<savanna::vote_bitset>{std::move(weak)} : std::nullopt,
       .signature = signature.finish(),
   };
}

savanna::block_ref hardening_ref(
    savanna::block_num_t num, savanna::block_slot_t slot,
    std::uint64_t identity) {
   return {
       .num = num,
       .id = hardening_digest(identity),
       .slot = slot,
       .finality_digest = hardening_digest(identity + 100U),
       .active_policy_generation = 1U,
   };
}

savanna::finality_core hardening_core(
    std::vector<savanna::block_ref> refs,
    savanna::block_num_t latest_qc) {
   auto links = std::vector<savanna::qc_link>{};
   links.reserve(refs.size() + 1U);
   for (auto source = savanna::block_num_t{};
        source <= refs.size(); ++source) {
      links.push_back({
          .source = source,
          .target = source == 0U ? 0U : std::min(source - 1U, latest_qc),
          .strong = source != 0U,
      });
   }
   auto result = savanna::finality_core{
       .links = std::move(links),
       .refs = std::move(refs),
       .genesis_slot = 10U,
   };
   savanna::validate(result);
   return result;
}

BOOST_AUTO_TEST_CASE(chain_savanna_validation_state_is_bounded_and_versioned) {
   constexpr auto retained_window = std::size_t{256U};
   constexpr auto block_count = std::uint32_t{100000U};

   auto state = savanna::make_validation({
       .num = 0U,
       .slot = 1U,
       .parent_slot = 0U,
       .finality_digest = hardening_digest(1U),
       .commitment = hardening_digest(2U),
   });

   for (auto num = std::uint32_t{1U}; num < block_count; ++num) {
      state = savanna::append(
          std::move(state),
          {
              .num = num,
              .slot = num + 1U,
              .parent_slot = num,
              .finality_digest = hardening_digest(num + 10U),
              .commitment = hardening_digest(num + 20U),
          });
      if (state.retained_size() > retained_window) {
         const auto root = state.root();
         state = savanna::advance_finalized(
             std::move(state), num - static_cast<std::uint32_t>(retained_window) + 1U);
         BOOST_TEST(state.root() == root);
      }
   }

   savanna::validate(state);
   BOOST_TEST(state.retained_size() == retained_window);
   BOOST_TEST(state.current_block_num() == block_count - 1U);
   BOOST_TEST(state.first_block_num() == block_count - retained_window);
   BOOST_TEST(forge::raw::pack(state).size() < 40000U);
   BOOST_CHECK_THROW(
       static_cast<void>(
           savanna::root_at(state, state.first_block_num() - 1U)),
       savanna::exceptions::validation_root_unavailable);

   const auto head_root = state.root();
   const auto head = state.current_block_num();
   state = savanna::advance_finalized(std::move(state), head);
   BOOST_TEST(state.retained_size() == 1U);
   BOOST_TEST(state.root() == head_root);
   savanna::validate(state);

   const auto encoded = forge::raw::pack(state);
   const auto restored =
       forge::raw::unpack<savanna::validation_state>(encoded);
   BOOST_TEST(restored.first_block_num() == state.first_block_num());
   BOOST_TEST(restored.root() == state.root());

   auto wrong_version = encoded;
   wrong_version[0] = 2U;
   BOOST_CHECK_THROW(
       static_cast<void>(
           forge::raw::unpack<savanna::validation_state>(wrong_version)),
       forge::raw::exceptions::codec_error);

   auto corrupted = encoded;
   corrupted.back() ^= 0x01U;
   BOOST_CHECK_THROW(
       static_cast<void>(
           forge::raw::unpack<savanna::validation_state>(corrupted)),
       forge::raw::exceptions::codec_error);

   const auto retained_leaf = savanna::validation_leaf{
       .num = 0U,
       .slot = 1U,
       .finality_digest = hardening_digest(1U),
       .commitment = hardening_digest(2U),
   };
   auto expected = forge::chain::core::incremental_merkle_tree{};
   expected.append(forge::crypto::digest::sha256::hash(retained_leaf));
   auto corrupted_frontier = forge::chain::core::incremental_merkle_tree{};
   corrupted_frontier.append(hardening_digest(999U));
   auto stream = forge::datastream<std::vector<std::uint8_t>>{};
   forge::raw::pack(stream, std::uint32_t{1U});
   forge::raw::pack(
       stream, forge::chain::core::incremental_merkle_tree{});
   forge::raw::pack(stream, corrupted_frontier);
   forge::raw::pack(stream, savanna::block_num_t{0U});
   forge::raw::pack(stream, std::vector<savanna::digest>{expected.root()});
   forge::raw::pack(
       stream, std::vector<savanna::validation_leaf>{retained_leaf});
   BOOST_CHECK_THROW(
       static_cast<void>(forge::raw::unpack<savanna::validation_state>(
           stream.storage())),
       forge::raw::exceptions::codec_error);

   auto phantom_prefix = forge::chain::core::incremental_merkle_tree{};
   phantom_prefix.append(hardening_digest(998U));
   auto phantom_current = phantom_prefix;
   phantom_current.append(
       forge::crypto::digest::sha256::hash(retained_leaf));
   auto phantom_stream = forge::datastream<std::vector<std::uint8_t>>{};
   forge::raw::pack(phantom_stream, std::uint32_t{1U});
   forge::raw::pack(phantom_stream, phantom_prefix);
   forge::raw::pack(phantom_stream, phantom_current);
   forge::raw::pack(phantom_stream, savanna::block_num_t{0U});
   forge::raw::pack(
       phantom_stream, std::vector<savanna::digest>{phantom_current.root()});
   forge::raw::pack(
       phantom_stream, std::vector<savanna::validation_leaf>{retained_leaf});
   BOOST_CHECK_THROW(
       static_cast<void>(forge::raw::unpack<savanna::validation_state>(
           phantom_stream.storage())),
       forge::raw::exceptions::codec_error);
}

BOOST_AUTO_TEST_CASE(chain_savanna_vote_accumulator_tracks_donor_states) {
   const auto first = hardening_key(1U);
   const auto second = hardening_key(33U);
   const auto third = hardening_key(65U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 3U,
       .finalizers =
           {
               hardening_finalizer(first, 2U, "first"),
               hardening_finalizer(second, 1U, "second"),
               hardening_finalizer(third, 1U, "third"),
           },
   };
   const auto proofs = std::array{
       first.proof_of_possession(),
       second.proof_of_possession(),
       third.proof_of_possession(),
   };
   const auto verified = savanna::validate(policy, proofs);
   const auto candidate = hardening_ref(9U, 20U, 90U);

   auto accumulator = savanna::vote_accumulator{candidate, verified};
   BOOST_CHECK(accumulator.add(
                   make_vote(candidate, first, savanna::vote_kind::strong)) ==
               savanna::vote_result::accepted);
   BOOST_CHECK(accumulator.status().active.state ==
               savanna::accumulator_state::unrestricted);
   BOOST_CHECK(accumulator.add(
                   make_vote(candidate, second, savanna::vote_kind::weak)) ==
               savanna::vote_result::accepted);
   BOOST_CHECK(accumulator.status().active.state ==
               savanna::accumulator_state::weak_achieved);
   BOOST_REQUIRE(accumulator.best().has_value());
   BOOST_TEST(accumulator.best()->active.weak());

   BOOST_CHECK(accumulator.add(
                   make_vote(candidate, second, savanna::vote_kind::weak)) ==
               savanna::vote_result::duplicate);
   BOOST_CHECK(accumulator.add(
                   make_vote(candidate, second, savanna::vote_kind::strong)) ==
               savanna::vote_result::conflicting);
   BOOST_CHECK(accumulator.add(
                   make_vote(candidate, third, savanna::vote_kind::weak)) ==
               savanna::vote_result::accepted);
   BOOST_CHECK(accumulator.status().active.state ==
               savanna::accumulator_state::weak_final);

   auto all_weak = savanna::vote_accumulator{candidate, verified};
   static_cast<void>(
       all_weak.add(make_vote(candidate, first, savanna::vote_kind::weak)));
   static_cast<void>(
       all_weak.add(make_vote(candidate, second, savanna::vote_kind::weak)));
   const auto all_weak_qc = all_weak.best();
   BOOST_REQUIRE(all_weak_qc.has_value());
   BOOST_CHECK(!all_weak_qc->active.strong_votes.has_value());
   const auto all_weak_keys = std::array{std::cref(first), std::cref(second)};
   const auto all_weak_indices = std::array<std::size_t, 2>{0U, 1U};
   const auto all_weak_kinds =
       std::array{savanna::vote_kind::weak, savanna::vote_kind::weak};
   const auto canonical_all_weak = savanna::quorum_certificate{
       .block = candidate.num,
       .active = make_qc_signature(
           candidate, policy.finalizers.size(), all_weak_keys,
           all_weak_indices, all_weak_kinds),
   };
   BOOST_TEST(forge::raw::pack(*all_weak_qc) ==
              forge::raw::pack(canonical_all_weak));

   auto noncanonical_all_weak = canonical_all_weak;
   noncanonical_all_weak.active.strong_votes =
       savanna::vote_bitset{policy.finalizers.size()};
   auto observed_all_weak =
       savanna::vote_accumulator{candidate, verified};
   BOOST_CHECK_THROW(
       static_cast<void>(observed_all_weak.observe(noncanonical_all_weak)),
       savanna::exceptions::invalid_qc);
   BOOST_CHECK(!observed_all_weak.best().has_value());

   auto restricted = savanna::vote_accumulator{candidate, verified};
   static_cast<void>(restricted.add(
       make_vote(candidate, second, savanna::vote_kind::weak)));
   static_cast<void>(restricted.add(
       make_vote(candidate, third, savanna::vote_kind::weak)));
   BOOST_CHECK(restricted.status().active.state ==
               savanna::accumulator_state::restricted);

   auto strong = savanna::vote_accumulator{candidate, verified};
   static_cast<void>(
       strong.add(make_vote(candidate, first, savanna::vote_kind::strong)));
   static_cast<void>(
       strong.add(make_vote(candidate, second, savanna::vote_kind::strong)));
   BOOST_CHECK(strong.status().active.state ==
               savanna::accumulator_state::strong);
   BOOST_REQUIRE(strong.best().has_value());
   BOOST_TEST(strong.best()->strong());

   auto wrong_block = make_vote(candidate, third, savanna::vote_kind::strong);
   wrong_block.block = hardening_digest(999U);
   BOOST_CHECK(strong.add(wrong_block) == savanna::vote_result::wrong_block);

   const auto outsider = hardening_key(97U);
   BOOST_CHECK(strong.add(
                   make_vote(candidate, outsider, savanna::vote_kind::strong)) ==
               savanna::vote_result::unknown_finalizer);

   auto invalid = make_vote(candidate, third, savanna::vote_kind::strong);
   invalid.signature =
       outsider.sign(savanna::message_for_vote(
           candidate.finality_digest, savanna::vote_kind::strong));
   BOOST_CHECK(strong.add(invalid) ==
               savanna::vote_result::invalid_signature);

   const auto weak_message = savanna::message_for_vote(
       candidate.finality_digest, savanna::vote_kind::weak);
   BOOST_REQUIRE_EQUAL(weak_message.size(), 36U);
   BOOST_TEST(weak_message[32] == static_cast<std::uint8_t>('W'));
   BOOST_TEST(weak_message[35] == static_cast<std::uint8_t>('K'));
}

BOOST_AUTO_TEST_CASE(
    chain_savanna_zero_weight_vote_does_not_finalize_weak_quorum) {
   const auto first = hardening_key(1U);
   const auto second = hardening_key(33U);
   const auto zero = hardening_key(65U);
   const auto fourth = hardening_key(97U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 2U,
       .finalizers =
           {
               hardening_finalizer(first, 1U, "first"),
               hardening_finalizer(second, 1U, "second"),
               hardening_finalizer(zero, 0U, "zero"),
               hardening_finalizer(fourth, 1U, "fourth"),
           },
   };
   const auto verified = savanna::validate(
       policy, std::array{first.proof_of_possession(),
                          second.proof_of_possession(),
                          zero.proof_of_possession(),
                          fourth.proof_of_possession()});
   const auto candidate = hardening_ref(9U, 20U, 90U);
   auto accumulator = savanna::vote_accumulator{candidate, verified};

   static_cast<void>(
       accumulator.add(make_vote(candidate, first, savanna::vote_kind::strong)));
   static_cast<void>(
       accumulator.add(make_vote(candidate, second, savanna::vote_kind::weak)));
   BOOST_CHECK(accumulator.status().active.state ==
               savanna::accumulator_state::weak_achieved);

   static_cast<void>(
       accumulator.add(make_vote(candidate, zero, savanna::vote_kind::weak)));
   BOOST_CHECK(accumulator.status().active.state ==
               savanna::accumulator_state::weak_achieved);

   static_cast<void>(
       accumulator.add(make_vote(candidate, fourth, savanna::vote_kind::strong)));
   BOOST_CHECK(accumulator.status().active.state ==
               savanna::accumulator_state::strong);
}

BOOST_AUTO_TEST_CASE(
    chain_savanna_vote_accumulator_requires_candidate_policy_generations) {
   const auto key = hardening_key(1U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 1U,
       .finalizers = {hardening_finalizer(key, 1U, "active")},
   };
   const auto active =
       savanna::validate(policy, std::array{key.proof_of_possession()});
   auto pending_policy = policy;
   pending_policy.generation = 2U;
   const auto pending = savanna::validate(
       pending_policy, std::array{key.proof_of_possession()});

   auto candidate = hardening_ref(10U, 30U, 100U);
   candidate.active_policy_generation = 2U;
   BOOST_CHECK_THROW(
       static_cast<void>(savanna::vote_accumulator{candidate, active}),
       savanna::exceptions::invalid_policy);

   candidate.active_policy_generation = 1U;
   candidate.pending_policy_generation = 2U;
   BOOST_CHECK_THROW(
       static_cast<void>(savanna::vote_accumulator{candidate, active}),
       savanna::exceptions::invalid_policy);

   candidate.pending_policy_generation = 0U;
   BOOST_CHECK_THROW(
       static_cast<void>(
           savanna::vote_accumulator{candidate, active, pending}),
       savanna::exceptions::invalid_policy);

   candidate.pending_policy_generation = 3U;
   BOOST_CHECK_THROW(
       static_cast<void>(
           savanna::vote_accumulator{candidate, active, pending}),
       savanna::exceptions::invalid_policy);

   candidate.pending_policy_generation = 2U;
   BOOST_CHECK_NO_THROW(static_cast<void>(
       savanna::vote_accumulator{candidate, active, pending}));
}

BOOST_AUTO_TEST_CASE(chain_savanna_vote_accumulator_is_atomic_and_thread_safe) {
   const auto first = hardening_key(1U);
   const auto second = hardening_key(33U);
   const auto third = hardening_key(65U);
   const auto fourth = hardening_key(97U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 3U,
       .finalizers =
           {
               hardening_finalizer(first, 1U, "first"),
               hardening_finalizer(second, 1U, "second"),
               hardening_finalizer(third, 1U, "third"),
               hardening_finalizer(fourth, 1U, "fourth"),
           },
   };
   const auto proofs = std::array{
       first.proof_of_possession(),
       second.proof_of_possession(),
       third.proof_of_possession(),
       fourth.proof_of_possession(),
   };
   const auto active = savanna::validate(policy, proofs);
   auto pending_policy = policy;
   pending_policy.generation = 2U;
   const auto pending = savanna::validate(pending_policy, proofs);
   auto candidate = hardening_ref(10U, 30U, 100U);
   candidate.pending_policy_generation = pending_policy.generation;
   auto accumulator =
       savanna::vote_accumulator{candidate, active, pending};

   auto votes = std::array{
       make_vote(candidate, first, savanna::vote_kind::strong),
       make_vote(candidate, second, savanna::vote_kind::strong),
       make_vote(candidate, third, savanna::vote_kind::strong),
       make_vote(candidate, fourth, savanna::vote_kind::strong),
   };
   auto results = std::array<savanna::vote_result, 4>{};
   auto threads = std::vector<std::thread>{};
   for (auto index = std::size_t{}; index < votes.size(); ++index) {
      threads.emplace_back([&, index] {
         results[index] = accumulator.add(votes[index]);
      });
   }
   for (auto& thread : threads) {
      thread.join();
   }
   for (const auto result : results) {
      BOOST_CHECK(result == savanna::vote_result::accepted);
   }

   const auto status = accumulator.status();
   BOOST_CHECK(status.active.state == savanna::accumulator_state::strong);
   BOOST_REQUIRE(status.pending.has_value());
   BOOST_CHECK(status.pending->state == savanna::accumulator_state::strong);
   const auto first_status = accumulator.status(first.get_public_key());
   BOOST_REQUIRE(first_status.active.has_value());
   BOOST_REQUIRE(first_status.pending.has_value());
   BOOST_CHECK(*first_status.active == savanna::vote_kind::strong);
   BOOST_CHECK(*first_status.pending == savanna::vote_kind::strong);
   BOOST_REQUIRE(accumulator.best().has_value());
   BOOST_TEST(accumulator.best()->strong());
}

BOOST_AUTO_TEST_CASE(chain_savanna_received_qc_beats_local_weak_qc) {
   const auto first = hardening_key(1U);
   const auto second = hardening_key(33U);
   const auto third = hardening_key(65U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 2U,
       .finalizers =
           {
               hardening_finalizer(first, 1U, "first"),
               hardening_finalizer(second, 1U, "second"),
               hardening_finalizer(third, 1U, "third"),
           },
   };
   const auto verified = savanna::validate(
       policy, std::array{first.proof_of_possession(),
                          second.proof_of_possession(),
                          third.proof_of_possession()});
   const auto candidate = hardening_ref(11U, 31U, 110U);
   auto accumulator = savanna::vote_accumulator{candidate, verified};
   static_cast<void>(
       accumulator.add(make_vote(candidate, first, savanna::vote_kind::strong)));
   static_cast<void>(
       accumulator.add(make_vote(candidate, second, savanna::vote_kind::weak)));
   BOOST_REQUIRE(accumulator.best().has_value());
   BOOST_TEST(accumulator.best()->active.weak());

   const auto strong_kinds =
       std::array{savanna::vote_kind::strong, savanna::vote_kind::strong};
   const auto signing_keys = std::array{std::cref(first), std::cref(second)};
   const auto signer_indices = std::array<std::size_t, 2>{0U, 1U};
   const auto certificate = savanna::quorum_certificate{
       .block = candidate.num,
       .active = make_qc_signature(
           candidate, policy.finalizers.size(), signing_keys, signer_indices,
           strong_kinds),
   };
   BOOST_TEST(accumulator.observe(certificate));
   BOOST_TEST(!accumulator.observe(certificate));
   BOOST_REQUIRE(accumulator.best().has_value());
   BOOST_TEST(accumulator.best()->strong());
}

BOOST_AUTO_TEST_CASE(chain_savanna_dual_policy_best_keeps_qc_halves_paired) {
   const auto shared = hardening_key(1U);
   const auto pending_only = hardening_key(33U);
   const auto active_policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 2U,
       .finalizers = {hardening_finalizer(shared, 2U, "shared")},
   };
   const auto pending_policy = savanna::finalizer_policy{
       .generation = 2U,
       .threshold = 2U,
       .finalizers =
           {
               hardening_finalizer(shared, 1U, "shared"),
               hardening_finalizer(pending_only, 1U, "pending"),
           },
   };
   const auto active = savanna::validate(
       active_policy, std::array{shared.proof_of_possession()});
   const auto pending = savanna::validate(
       pending_policy,
       std::array{shared.proof_of_possession(),
                  pending_only.proof_of_possession()});
   auto candidate = hardening_ref(12U, 32U, 120U);
   candidate.pending_policy_generation = pending_policy.generation;
   auto accumulator = savanna::vote_accumulator{candidate, active, pending};

   static_cast<void>(
       accumulator.add(make_vote(candidate, shared, savanna::vote_kind::strong)));
   BOOST_CHECK(accumulator.status().active.state ==
               savanna::accumulator_state::strong);
   BOOST_REQUIRE(accumulator.status().pending.has_value());
   BOOST_CHECK(accumulator.status().pending->state ==
               savanna::accumulator_state::unrestricted);

   const auto weak_kind = std::array{savanna::vote_kind::weak};
   const auto active_keys = std::array{std::cref(shared)};
   const auto active_indices = std::array<std::size_t, 1>{0U};
   const auto pending_kinds =
       std::array{savanna::vote_kind::weak, savanna::vote_kind::weak};
   const auto pending_keys = std::array{std::cref(shared), std::cref(pending_only)};
   const auto pending_indices = std::array<std::size_t, 2>{0U, 1U};
   const auto received = savanna::quorum_certificate{
       .block = candidate.num,
       .active = make_qc_signature(
           candidate, active_policy.finalizers.size(), active_keys,
           active_indices, weak_kind),
       .pending = make_qc_signature(
           candidate, pending_policy.finalizers.size(), pending_keys,
           pending_indices, pending_kinds),
   };
   BOOST_TEST(accumulator.observe(received));

   const auto best = accumulator.best();
   BOOST_REQUIRE(best.has_value());
   BOOST_TEST(best->active.weak());
   BOOST_REQUIRE(best->pending.has_value());
   BOOST_TEST(best->pending->weak());
   BOOST_CHECK_NO_THROW(static_cast<void>(savanna::verify(
       *best, active, std::optional<savanna::verified_finalizer_policy>{pending},
       candidate.finality_digest)));
}

BOOST_AUTO_TEST_CASE(chain_savanna_finalizer_safety_matches_spring_rules) {
   const auto ref0 = hardening_ref(0U, 10U, 1U);
   const auto ref1 = hardening_ref(1U, 11U, 2U);
   const auto branch_ref2 = hardening_ref(2U, 12U, 30U);

   const auto core2 = hardening_core({ref0, ref1}, 1U);
   const auto candidate2 = hardening_ref(2U, 12U, 4U);
   const auto initial = savanna::make_finalizer_safety(ref0);
   const auto first_plan =
       savanna::plan_vote(initial, core2, candidate2);
   BOOST_TEST(first_plan.monotonic);
   BOOST_TEST(first_plan.live);
   BOOST_CHECK(first_plan.decision == savanna::vote_decision::strong);
   BOOST_TEST(first_plan.next.lock().id == ref1.id);
   BOOST_TEST(first_plan.next.last_vote().id == candidate2.id);

   const auto repeated =
       savanna::plan_vote(first_plan.next, core2, candidate2);
   BOOST_TEST(!repeated.monotonic);
   BOOST_CHECK(repeated.decision == savanna::vote_decision::abstain);

   const auto branch_core3 =
       hardening_core({ref0, ref1, branch_ref2}, 1U);
   const auto branch_candidate3 = hardening_ref(3U, 13U, 31U);
   const auto branch_plan =
       savanna::plan_vote(first_plan.next, branch_core3, branch_candidate3);
   BOOST_TEST(branch_plan.monotonic);
   BOOST_TEST(!branch_plan.live);
   BOOST_TEST(branch_plan.safe);
   BOOST_CHECK(branch_plan.decision == savanna::vote_decision::weak);
   BOOST_TEST(branch_plan.next.other_branch_latest_slot() == candidate2.slot);

   const auto branch_core4 =
       hardening_core({ref0, ref1, branch_ref2, branch_candidate3}, 1U);
   const auto branch_candidate4 = hardening_ref(4U, 14U, 32U);
   const auto second_weak = savanna::plan_vote(
       branch_plan.next, branch_core4, branch_candidate4);
   BOOST_CHECK(second_weak.decision == savanna::vote_decision::weak);

   const auto restored =
       forge::raw::unpack<savanna::finalizer_safety_state>(
           forge::raw::pack(branch_plan.next));
   BOOST_TEST(restored.last_vote().id ==
              branch_plan.next.last_vote().id);
   BOOST_TEST(restored.lock().id == branch_plan.next.lock().id);
   BOOST_TEST(restored.other_branch_latest_slot() ==
              branch_plan.next.other_branch_latest_slot());

   auto corrupted = forge::raw::pack(branch_plan.next);
   corrupted[0] = 2U;
   BOOST_CHECK_THROW(
       static_cast<void>(
           forge::raw::unpack<savanna::finalizer_safety_state>(corrupted)),
       forge::raw::exceptions::codec_error);

   const auto same_core3 =
       hardening_core({ref0, ref1, candidate2}, 1U);
   const auto same_candidate3 = hardening_ref(3U, 13U, 5U);
   const auto same_branch =
       savanna::plan_vote(first_plan.next, same_core3, same_candidate3);
   BOOST_TEST(same_branch.safe);
   BOOST_CHECK(same_branch.decision == savanna::vote_decision::strong);
}

BOOST_AUTO_TEST_CASE(chain_savanna_verified_qc_advances_finalizer_safety) {
   const auto key = hardening_key(1U);
   const auto outsider = hardening_key(33U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 1U,
       .finalizers = {hardening_finalizer(key, 1U, "finalizer")},
   };
   const auto verified_policy =
       savanna::validate(policy, std::array{key.proof_of_possession()});
   const auto ref0 = hardening_ref(0U, 10U, 1U);
   const auto ref1 = hardening_ref(1U, 11U, 2U);
   const auto core = hardening_core({ref0, ref1}, 1U);
   const auto candidate = hardening_ref(2U, 12U, 3U);
   const auto kinds = std::array{savanna::vote_kind::strong};
   const auto signing_keys = std::array{std::cref(key)};
   const auto signer_indices = std::array<std::size_t, 1>{0U};
   const auto certificate = savanna::quorum_certificate{
       .block = candidate.num,
       .active = make_qc_signature(
           candidate, policy.finalizers.size(), signing_keys, signer_indices,
           kinds),
   };
   const auto verified_qc = savanna::verify(
       certificate, verified_policy, std::nullopt,
       candidate.finality_digest);
   BOOST_TEST(verified_qc.finality_digest() == candidate.finality_digest);
   BOOST_TEST(verified_qc.has_strong_vote(key.get_public_key()));

   const auto advanced = savanna::advance_from_qc(
       savanna::make_finalizer_safety(ref0), core, candidate, verified_qc,
       key.get_public_key());
   BOOST_TEST(advanced.lock().id == ref1.id);
   BOOST_TEST(advanced.last_vote().id == candidate.id);
   BOOST_TEST(advanced.other_branch_latest_slot() == 0U);

   const auto recovered = savanna::advance_from_qc(
       savanna::make_finalizer_safety(ref1), core, candidate, verified_qc,
       key.get_public_key());
   BOOST_TEST(recovered.lock().id == ref1.id);
   BOOST_TEST(recovered.last_vote().id == candidate.id);

   auto mismatched_candidate = candidate;
   mismatched_candidate.finality_digest = hardening_digest(999U);
   BOOST_CHECK_THROW(
       static_cast<void>(savanna::advance_from_qc(
           savanna::make_finalizer_safety(ref1), core, mismatched_candidate,
           verified_qc, key.get_public_key())),
       savanna::exceptions::invalid_qc);

   const auto competing = hardening_ref(candidate.num, candidate.slot, 99U);
   const auto competing_plan = savanna::plan_vote(recovered, core, competing);
   BOOST_TEST(!competing_plan.monotonic);
   BOOST_CHECK(competing_plan.decision == savanna::vote_decision::abstain);

   BOOST_CHECK_THROW(
       static_cast<void>(savanna::advance_from_qc(
           savanna::make_finalizer_safety(ref0), core, candidate, verified_qc,
           outsider.get_public_key())),
       savanna::exceptions::invalid_qc);
}

} // namespace
