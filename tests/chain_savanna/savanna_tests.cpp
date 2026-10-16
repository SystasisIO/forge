#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

import forge.chain.savanna.finality_core;
import forge.chain.savanna.extensions;
import forge.chain.savanna.exceptions;
import forge.chain.protocol.types;
import forge.chain.savanna.policy;
import forge.chain.savanna.qc;
import forge.chain.savanna.rank;
import forge.chain.savanna.validation;
import forge.crypto.digest.sha256;
import forge.raw.exceptions;
import forge.raw.raw;

namespace {

namespace savanna = forge::chain::savanna;
namespace bls = forge::crypto::bls;

savanna::digest make_digest(std::uint64_t value) {
   auto result = savanna::digest{};
   result._hash[0] = value;
   return result;
}

bls::private_key make_private_key(std::uint8_t base) {
   auto seed = std::array<std::uint8_t, 32>{};
   for (auto index = std::size_t{}; index < seed.size(); ++index) {
      seed[index] = static_cast<std::uint8_t>(base + index);
   }
   return bls::private_key{std::span<const std::uint8_t>{seed}};
}

savanna::finalizer make_finalizer(const bls::private_key& key, std::uint64_t weight, std::string description) {
   return {
       .description = std::move(description),
       .weight = weight,
       .public_key = key.get_public_key(),
   };
}

std::vector<std::uint8_t> weak_message(savanna::digest value) {
   auto result = std::vector<std::uint8_t>{};
   const auto bytes = value.to_uint8_span();
   result.insert(result.end(), bytes.begin(), bytes.end());
   result.insert(result.end(), {'W', 'E', 'A', 'K'});
   return result;
}

savanna::digest packed_finality_digest(const savanna::finality_core& core) {
   auto encoder = forge::crypto::digest::sha256::encoder{};
   core.pack_for_digest(encoder);
   return encoder.result();
}

BOOST_AUTO_TEST_CASE(chain_savanna_counter_commitment_v4_rejects_previous_v3_history) {
   static_assert(savanna::state_commitment_version == 4U);
   auto extensions = forge::chain::protocol::extensions{
       {savanna::finality_extension_id, forge::raw::pack(savanna::finality_extension{})},
       {savanna::state_commitment_extension_id, forge::raw::pack(savanna::state_commitment{})},
   };
   BOOST_CHECK_EQUAL(savanna::decode_header_extensions(extensions).commitment.version, 4U);
   extensions.back().second = forge::raw::pack(savanna::state_commitment{.version = 3U});
   const auto original_bytes = extensions.back().second;
   BOOST_CHECK_THROW(savanna::decode_header_extensions(extensions), savanna::exceptions::invalid_extension);
   BOOST_CHECK(extensions.back().second == original_bytes);
}

BOOST_AUTO_TEST_CASE(chain_savanna_policy_validates_strict_majority_and_keys) {
   const auto first = make_private_key(1U);
   const auto second = make_private_key(33U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 2U,
       .finalizers =
           {
               make_finalizer(first, 1U, "first"),
               make_finalizer(second, 2U, "second"),
           },
   };
   const auto proofs = std::array{first.proof_of_possession(), second.proof_of_possession()};
   const auto verified = savanna::validate(policy, proofs);
   BOOST_TEST(verified.get().generation == policy.generation);

   auto empty = policy;
   empty.finalizers.clear();
   BOOST_CHECK_THROW(static_cast<void>(savanna::validate(empty, {})), savanna::exceptions::invalid_policy);

   auto half = policy;
   half.finalizers[0].weight = 2U;
   half.threshold = 2U;
   BOOST_CHECK_THROW(static_cast<void>(savanna::validate(half, proofs)), savanna::exceptions::invalid_policy);

   auto duplicate = policy;
   duplicate.finalizers[1].public_key = duplicate.finalizers[0].public_key;
   BOOST_CHECK_THROW(static_cast<void>(savanna::validate(duplicate, proofs)), savanna::exceptions::duplicate_finalizer);

   auto identity = policy;
   identity.finalizers[0].public_key = {};
   BOOST_CHECK_THROW(static_cast<void>(savanna::validate(identity, proofs)), savanna::exceptions::invalid_policy);

   auto overflow = policy;
   overflow.finalizers[0].weight = std::numeric_limits<std::uint64_t>::max();
   overflow.finalizers[1].weight = 1U;
   overflow.threshold = std::numeric_limits<std::uint64_t>::max();
   BOOST_CHECK_THROW(static_cast<void>(savanna::validate(overflow, proofs)),
                     savanna::exceptions::policy_weight_overflow);

   const auto wrong_proofs = std::array{second.proof_of_possession(), first.proof_of_possession()};
   BOOST_CHECK_THROW(static_cast<void>(savanna::validate(policy, wrong_proofs)),
                     savanna::exceptions::invalid_proof_of_possession);
}

BOOST_AUTO_TEST_CASE(chain_savanna_policy_diff_is_ordered_and_sequential) {
   const auto first = make_private_key(1U);
   const auto second = make_private_key(33U);
   const auto third = make_private_key(65U);
   const auto replacement = make_private_key(97U);
   const auto source = savanna::finalizer_policy{
       .generation = 4U,
       .threshold = 2U,
       .finalizers =
           {
               make_finalizer(first, 1U, "first"),
               make_finalizer(second, 1U, "second"),
               make_finalizer(third, 1U, "third"),
           },
   };
   const auto source_proofs = std::array{
       first.proof_of_possession(),
       second.proof_of_possession(),
       third.proof_of_possession(),
   };
   const auto verified_source = savanna::validate(source, source_proofs);

   const auto updated =
       savanna::apply(verified_source,
                      {
                          .generation = 5U,
                          .threshold = 2U,
                          .finalizers =
                              {
                                  .remove_indexes = {1U},
                                  .insert_indexes = {{1U, make_finalizer(replacement, 1U, "replacement")}},
                              },
                      },
                      std::array{replacement.proof_of_possession()});
   const auto& updated_policy = updated.get();
   BOOST_TEST(updated_policy.generation == 5U);
   BOOST_REQUIRE_EQUAL(updated_policy.finalizers.size(), 3U);
   BOOST_TEST(updated_policy.finalizers[0].description == "first");
   BOOST_TEST(updated_policy.finalizers[1].description == "replacement");
   BOOST_TEST(updated_policy.finalizers[2].description == "third");

   auto skipped = savanna::finalizer_policy_diff{
       .generation = 6U,
       .threshold = 2U,
   };
   BOOST_CHECK_THROW(static_cast<void>(savanna::apply(verified_source, skipped, {})),
                     savanna::exceptions::invalid_policy);

   auto unordered = savanna::finalizer_policy_diff{
       .generation = 5U,
       .threshold = 2U,
       .finalizers = {.remove_indexes = {1U, 1U}},
   };
   BOOST_CHECK_THROW(static_cast<void>(savanna::apply(verified_source, unordered, {})),
                     savanna::exceptions::invalid_policy);

   const auto wrong_replacement_proof = std::array{first.proof_of_possession()};
   BOOST_CHECK_THROW(static_cast<void>(savanna::apply(
                         verified_source,
                         {
                             .generation = 5U,
                             .threshold = 2U,
                             .finalizers = {.insert_indexes = {{1U, make_finalizer(replacement, 1U, "replacement")}}},
                         },
                         wrong_replacement_proof)),
                     savanna::exceptions::invalid_proof_of_possession);
}

BOOST_AUTO_TEST_CASE(chain_savanna_qc_claim_raw_layout_matches_spring) {
   const auto bytes = forge::raw::pack(savanna::qc_claim{.block = 0x01020304U, .strong = true});
   const auto expected = std::vector<std::uint8_t>{0x04U, 0x03U, 0x02U, 0x01U, 0x01U};
   BOOST_TEST(bytes == expected, boost::test_tools::per_element());
}

BOOST_AUTO_TEST_CASE(chain_savanna_finality_matches_strong_and_weak_sequences) {
   const auto genesis_id = make_digest(1U);
   auto core = savanna::finality_core::genesis(0U, 10U);
   auto slot = savanna::block_slot_t{10U};

   auto next = [&](savanna::qc_claim claim) {
      const auto current = core.current_block_num();
      const auto current_id = current == 0U ? genesis_id : make_digest(current + 1U);
      core = core.next(
          {
              .num = current,
              .id = current_id,
              .slot = slot,
              .finality_digest = make_digest(current + 100U),
              .active_policy_generation = 1U,
          },
          claim);
      ++slot;
      BOOST_TEST(core.current_block_num() == current + 1U);
      BOOST_CHECK(core.latest_qc_claim() == claim);
   };

   next({.block = 0U, .strong = true});
   next({.block = 1U, .strong = true});
   next({.block = 2U, .strong = true});
   BOOST_TEST(core.last_final_block_num() == 1U);

   next({.block = 2U, .strong = true});
   next({.block = 3U, .strong = false});
   next({.block = 3U, .strong = false});
   BOOST_TEST(core.last_final_block_num() == 1U);
   next({.block = 3U, .strong = true});
   BOOST_TEST(core.last_final_block_num() == 2U);
   next({.block = 4U, .strong = true});
   next({.block = 6U, .strong = true});
   BOOST_TEST(core.last_final_block_num() == 3U);
}

BOOST_AUTO_TEST_CASE(chain_savanna_finality_digest_preserves_spring_preimage) {
   auto core = savanna::finality_core{
       .links =
           {
               {.source = 5U, .target = 5U, .strong = false},
               {.source = 6U, .target = 5U, .strong = true},
           },
       .refs = {{
           .num = 5U,
           .id = make_digest(1U),
           .slot = 11U,
           .finality_digest = make_digest(2U),
           .active_policy_generation = 3U,
           .pending_policy_generation = 4U,
       }},
       .genesis_slot = 9U,
   };
   const auto expected = savanna::digest{"8f6d62c3e4284755526e1f782e93c645bbd57d6c9eb98e3c73f574b2834c4d6b"};
   BOOST_TEST(core.digest_for_finality() == expected);

   auto compatibility_only = core;
   compatibility_only.refs[0].num = 99U;
   compatibility_only.refs[0].active_policy_generation = 30U;
   compatibility_only.refs[0].pending_policy_generation = 40U;
   BOOST_TEST(packed_finality_digest(compatibility_only) == expected);

   compatibility_only.refs[0].slot = 12U;
   BOOST_TEST(packed_finality_digest(compatibility_only) != expected);

   auto malformed = core;
   malformed.refs.clear();
   BOOST_CHECK_THROW(static_cast<void>(malformed.digest_for_finality()), savanna::exceptions::invalid_finality_state);
}

BOOST_AUTO_TEST_CASE(chain_savanna_reversible_root_matches_donor_layout) {
   const auto core = savanna::finality_core{
       .links =
           {
               {.source = 5U, .target = 5U, .strong = true},
               {.source = 6U, .target = 5U, .strong = true},
               {.source = 7U, .target = 5U, .strong = true},
               {.source = 8U, .target = 5U, .strong = true},
           },
       .refs =
           {
               {.num = 5U, .id = make_digest(5U), .slot = 10U, .finality_digest = make_digest(10U)},
               {.num = 6U, .id = make_digest(6U), .slot = 11U, .finality_digest = make_digest(11U)},
               {.num = 7U, .id = make_digest(7U), .slot = 12U, .finality_digest = make_digest(12U)},
           },
       .genesis_slot = 1U,
   };
   savanna::validate(core);
   BOOST_TEST(core.reversible_blocks_root() ==
              savanna::digest{"0a207008b0e24951416c8eabe657e2fafb4141506adda5d3ad5c10d727592888"});
}

BOOST_AUTO_TEST_CASE(chain_savanna_verifies_strong_weak_and_pending_qcs) {
   const auto first = make_private_key(1U);
   const auto second = make_private_key(33U);
   const auto third = make_private_key(65U);
   const auto policy = savanna::finalizer_policy{
       .generation = 1U,
       .threshold = 3U,
       .finalizers =
           {
               make_finalizer(first, 2U, "first"),
               make_finalizer(second, 1U, "second"),
               make_finalizer(third, 1U, "third"),
           },
   };
   const auto verified_policy = savanna::validate(policy, std::array{
                                                              first.proof_of_possession(),
                                                              second.proof_of_possession(),
                                                              third.proof_of_possession(),
                                                          });
   const auto digest = make_digest(42U);

   auto strong_votes = savanna::vote_bitset{3U};
   strong_votes.set(0U);
   strong_votes.set(1U);
   auto strong_accumulator = bls::signature_accumulator{};
   strong_accumulator.add(first.sign(digest.to_uint8_span()));
   strong_accumulator.add(second.sign(digest.to_uint8_span()));
   const auto strong_signature = strong_accumulator.finish();
   const auto strong = savanna::qc_signature{
       .strong_votes = strong_votes,
       .signature = strong_signature,
   };
   savanna::verify_signature(strong, verified_policy, digest);
   const auto packed_certificate = forge::raw::pack(savanna::quorum_certificate{.block = 9U, .active = strong});
   BOOST_REQUIRE_EQUAL(packed_certificate.size(), 203U);
   BOOST_TEST(packed_certificate[0] == 0x09U);
   BOOST_TEST(packed_certificate[4] == 0x01U);
   BOOST_TEST(packed_certificate[5] == 0x03U);
   BOOST_TEST(packed_certificate[6] == 0x03U);
   BOOST_TEST(packed_certificate[7] == 0x00U);
   BOOST_TEST(packed_certificate[8] == 0xc0U);
   BOOST_TEST(packed_certificate[9] == 0x01U);
   BOOST_TEST(packed_certificate.back() == 0x00U);
   BOOST_TEST(forge::crypto::digest::sha256::hash(packed_certificate).str() ==
              "5fecab0ef7d336ce3c3f6ba6b52fb91f7e0864d7f379e0be3992eea62ed95098");

   auto weak_strong_votes = savanna::vote_bitset{3U};
   weak_strong_votes.set(0U);
   auto weak_votes = savanna::vote_bitset{3U};
   weak_votes.set(1U);
   auto weak_accumulator = bls::signature_accumulator{};
   weak_accumulator.add(first.sign(digest.to_uint8_span()));
   const auto weak_bytes = weak_message(digest);
   weak_accumulator.add(second.sign(weak_bytes));
   const auto weak_signature = weak_accumulator.finish();
   const auto weak = savanna::qc_signature{
       .strong_votes = weak_strong_votes,
       .weak_votes = weak_votes,
       .signature = weak_signature,
   };
   savanna::verify_signature(weak, verified_policy, digest);

   const auto certificate = savanna::quorum_certificate{
       .block = 9U,
       .active = strong,
       .pending = strong,
   };
   static_cast<void>(
       savanna::verify(certificate, verified_policy, verified_policy, digest));
   BOOST_TEST(certificate.strong());
   const auto expected_claim = savanna::qc_claim{
       .block = 9U,
       .strong = true,
   };
   BOOST_CHECK(certificate.claim() == expected_claim);

   auto inconsistent_dual_vote = certificate;
   inconsistent_dual_vote.pending = weak;
   BOOST_CHECK_THROW(
       static_cast<void>(savanna::verify(
           inconsistent_dual_vote, verified_policy, verified_policy, digest)),
       savanna::exceptions::invalid_qc);

   BOOST_CHECK_THROW(savanna::verify_signature(strong, verified_policy, make_digest(43U)),
                     savanna::exceptions::invalid_qc_signature);

   auto malformed = strong;
   malformed.strong_votes = savanna::vote_bitset{2U};
   BOOST_CHECK_THROW(savanna::verify_basic(malformed, verified_policy), savanna::exceptions::invalid_qc);

   auto overlap = weak;
   overlap.weak_votes->set(0U);
   BOOST_CHECK_THROW(savanna::verify_basic(overlap, verified_policy), savanna::exceptions::invalid_qc);

   auto empty_weak = strong;
   empty_weak.weak_votes = savanna::vote_bitset{3U};
   BOOST_CHECK_THROW(savanna::verify_basic(empty_weak, verified_policy), savanna::exceptions::invalid_qc);

   auto downgraded = strong;
   downgraded.weak_votes = savanna::vote_bitset{3U};
   downgraded.weak_votes->set(2U);
   auto downgraded_accumulator = bls::signature_accumulator{};
   downgraded_accumulator.add(downgraded.signature);
   downgraded_accumulator.add(third.sign(weak_bytes));
   downgraded.signature = downgraded_accumulator.finish();
   BOOST_CHECK_THROW(savanna::verify_signature(downgraded, verified_policy, digest), savanna::exceptions::invalid_qc);
}

BOOST_AUTO_TEST_CASE(chain_savanna_validation_and_rank_are_deterministic) {
   const auto first = savanna::validation_leaf{
       .num = 5U,
       .slot = 10U,
       .parent_slot = 9U,
       .finality_digest = make_digest(1U),
       .commitment = make_digest(2U),
   };
   auto validation = savanna::make_validation(first);
   BOOST_TEST(savanna::root_at(validation, 5U) ==
              savanna::digest{"05e8db53e2d7fb3796ac15410da91f71debba651be277d8a9593e4b1b4986e8d"});
   validation = savanna::append(std::move(validation), {
                                                           .num = 6U,
                                                           .slot = 11U,
                                                           .parent_slot = 10U,
                                                           .finality_digest = make_digest(3U),
                                                           .commitment = make_digest(4U),
                                                       });
   savanna::validate(validation);
   BOOST_TEST(savanna::root_at(validation, 6U) == validation.root());
   BOOST_TEST(validation.root() ==
              savanna::digest{"86c45f2da153d610b48601b783c016799698c1e2a43da4b1a58a7bf77609ec8e"});

   const auto roundtrip = forge::raw::unpack<savanna::validation_state>(forge::raw::pack(validation));
   savanna::validate(roundtrip);
   BOOST_TEST(savanna::root_at(roundtrip, 5U) ==
              savanna::digest{"05e8db53e2d7fb3796ac15410da91f71debba651be277d8a9593e4b1b4986e8d"});

   BOOST_CHECK_THROW(static_cast<void>(savanna::append(validation, {.num = 8U})),
                     savanna::exceptions::invalid_validation_state);

   auto corrupted = forge::raw::pack(validation);
   corrupted.back() ^= 0x01U;
   BOOST_CHECK_THROW(
       static_cast<void>(
           forge::raw::unpack<savanna::validation_state>(corrupted)),
       forge::raw::exceptions::codec_error);

   auto core = savanna::finality_core::genesis(5U, 10U);
   const auto block = savanna::block_ref{
       .num = 5U,
       .id = make_digest(6U),
       .slot = 11U,
   };
   const auto first_rank = savanna::make_rank(core, block);
   auto later = first_rank;
   later.block = 12U;
   BOOST_TEST(savanna::better(later, first_rank));
}

} // namespace
