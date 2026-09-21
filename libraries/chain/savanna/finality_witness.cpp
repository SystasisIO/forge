module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

module forge.chain.savanna.finality_witness;

import forge.raw.exceptions;
import forge.raw.raw;

namespace forge::chain::savanna {
namespace {

namespace protocol = forge::chain::protocol;

void validate_limits(finality_witness_limits limits) {
   if (limits.max_blocks == 0U || limits.max_blocks > finality_witness_hard_max_blocks ||
       limits.max_producer_slots == 0U || limits.max_producer_slots > finality_witness_hard_max_producer_slots ||
       limits.max_bytes == 0U || limits.max_bytes > finality_witness_hard_max_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                            "Savanna finality witness limits exceed the hard bounds",
                            forge::exceptions::ctx("max_blocks", limits.max_blocks),
                            forge::exceptions::ctx("max_producer_slots", limits.max_producer_slots),
                            forge::exceptions::ctx("max_bytes", limits.max_bytes));
   }
}

void validate_shape(const finality_witness& witness, finality_witness_limits limits) {
   if (witness.chain.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality_witness, "Savanna finality witness chain id is empty");
   }
   if (witness.trusted_bootstrap.empty()) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality_witness, "Savanna finality witness bootstrap id is empty");
   }
   if (witness.records.size() > limits.max_blocks) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                            "Savanna finality witness block count exceeds the configured limit",
                            forge::exceptions::ctx("blocks", witness.records.size()),
                            forge::exceptions::ctx("limit", limits.max_blocks));
   }

   auto parent = witness.trusted_bootstrap;
   for (auto index = std::size_t{}; index < witness.records.size(); ++index) {
      const auto& header = witness.records[index].header;
      if (header.previous != parent) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality_witness,
                               "Savanna finality witness blocks are not contiguous",
                               forge::exceptions::ctx("block_index", index));
      }
      parent = protocol::calculate_block_id(header);
   }
}

template <typename Stream> void pack_payload(Stream& stream, const finality_witness& witness) {
   forge::raw::pack(stream, witness.chain);
   forge::raw::pack(stream, witness.trusted_bootstrap);
   forge::raw::pack(stream, forge::unsigned_int{static_cast<std::uint32_t>(witness.records.size())});
   for (const auto& record : witness.records) {
      forge::raw::pack(stream, record.header);
      forge::raw::pack(stream, record.block_extensions);
      forge::raw::pack(stream, record.action_receipt_root);
   }
}

std::size_t measure_payload(const finality_witness& witness, finality_witness_limits limits) {
   auto stream = forge::datastream<std::size_t>{};
   forge::raw::pack(stream, witness.chain);
   forge::raw::pack(stream, witness.trusted_bootstrap);
   forge::raw::pack(stream, forge::unsigned_int{static_cast<std::uint32_t>(witness.records.size())});
   if (stream.tellp() > limits.max_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                            "Savanna finality witness prefix exceeds the configured byte limit");
   }
   for (const auto& record : witness.records) {
      forge::raw::pack(stream, record.header);
      forge::raw::pack(stream, record.block_extensions);
      forge::raw::pack(stream, record.action_receipt_root);
      if (stream.tellp() > limits.max_bytes) {
         FORGE_THROW_EXCEPTION(
             exceptions::finality_witness_limit_exceeded, "Savanna finality witness exceeds the configured byte limit",
             forge::exceptions::ctx("bytes", stream.tellp()), forge::exceptions::ctx("limit", limits.max_bytes));
      }
   }
   return stream.tellp();
}

protocol::bytes encode_payload(const finality_witness& witness, finality_witness_limits limits) {
   validate_limits(limits);
   validate_shape(witness, limits);
   const auto size = measure_payload(witness, limits);
   auto payload = protocol::bytes(size);
   auto stream = forge::datastream<std::uint8_t*>{payload.data(), payload.size()};
   pack_payload(stream, witness);
   return payload;
}

finality_witness decode_payload(std::span<const std::uint8_t> payload, finality_witness_limits limits) {
   validate_limits(limits);
   if (payload.size() > limits.max_bytes) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                            "Savanna finality witness payload exceeds the configured byte limit",
                            forge::exceptions::ctx("bytes", payload.size()),
                            forge::exceptions::ctx("limit", limits.max_bytes));
   }

   try {
      const auto payload_budget = static_cast<std::uint32_t>(payload.size());
      const auto witness_limits = forge::raw::unpack_limits{
          .max_container_elements = payload_budget,
          .max_total_container_elements = payload_budget,
          .max_bytes = payload_budget,
          .first_container_elements = std::min(limits.max_blocks, payload_budget),
      };
      auto witness = forge::raw::unpack_exact<finality_witness>(payload, witness_limits);

      validate_shape(witness, limits);
      const auto canonical = encode_payload(witness, limits);
      if (canonical.size() != payload.size() || !std::ranges::equal(canonical, payload)) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality_witness,
                               "Savanna finality witness is not canonically encoded");
      }
      return witness;
   } catch (const forge::raw::exceptions::allocation_limit& error) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                            "Savanna finality witness exceeds its decoding allocation budget",
                            forge::exceptions::ctx("reason", error.what()));
   } catch (const exceptions::finality_witness_limit_exceeded&) {
      throw;
   } catch (const exceptions::invalid_finality_witness&) {
      throw;
   } catch (const std::bad_alloc&) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                            "Savanna finality witness decoding exhausted its allocation budget");
   } catch (const std::length_error&) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                            "Savanna finality witness decoding requested an oversized allocation");
   } catch (const std::exception& error) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_finality_witness, "Savanna finality witness payload is malformed",
                            forge::exceptions::ctx("reason", error.what()));
   }
}

protocol::state_anchor make_anchor(const protocol::chain_id& chain, const candidate& value) {
   const auto commitment = decode_header_extensions(value.state.header.header_extensions).commitment;
   return {
       .chain = chain,
       .block = value.id,
       .block_num = value.num,
       .transaction_root = value.state.header.transaction_mroot,
       .state_root = commitment.state_root,
       .state_size = commitment.state_size,
       .change_root = commitment.change_root,
       .change_count = commitment.change_count,
   };
}

struct replay_seed {
   protocol::chain_id chain;
   candidate root;
   block_num finalized = 0;
};

replay_seed make_seed(const finality_genesis_bootstrap& bootstrap) {
   if (bootstrap.commitment.version != state_commitment_version) {
      FORGE_THROW_EXCEPTION(exceptions::untrusted_finality_bootstrap,
                            "Savanna trusted genesis uses an unsupported state commitment version");
   }
   auto genesis = make_genesis_candidate(bootstrap.configuration, bootstrap.commitment);
   const auto finalized = genesis.value.num;
   return {
       .chain = calculate_chain_id(bootstrap.configuration),
       .root = std::move(genesis.value),
       .finalized = finalized,
   };
}

void validate_policy_state(const finalizer_policy_state& value) {
   static_cast<void>(validate(value));
}

replay_seed make_seed(const finality_checkpoint_bootstrap& bootstrap) {
   const auto& value = bootstrap.value;
   if (bootstrap.chain.empty() || value.finalized.empty() || value.state.id != value.finalized.id ||
       value.state.num() != value.finalized.num || protocol::calculate_block_id(value.state.header) != value.state.id ||
       value.state.block != value.finalized || value.state.make_block_ref() != value.finalized) {
      FORGE_THROW_EXCEPTION(exceptions::untrusted_finality_bootstrap,
                            "Savanna trusted checkpoint identity is inconsistent");
   }

   forge::chain::savanna::validate(value.state.finality);
   forge::chain::savanna::validate(value.validation);
   if (value.validation.first_block_num() != value.finalized.num ||
       value.validation.current_block_num() != value.finalized.num) {
      FORGE_THROW_EXCEPTION(exceptions::untrusted_finality_bootstrap,
                            "Savanna trusted checkpoint validation range is inconsistent");
   }
   validate_policy_state(value.state.active_finalizers);
   for (const auto& [block, policy] : value.state.proposed_finalizers) {
      static_cast<void>(block);
      validate_policy_state(policy);
   }
   if (value.state.pending_finalizers) {
      validate_policy_state(value.state.pending_finalizers->second);
   }
   if (value.state.latest_qc_finalizers) {
      validate_policy_state(*value.state.latest_qc_finalizers);
   }
   static_cast<void>(decode_header_extensions(value.state.header.header_extensions));

   return {
       .chain = bootstrap.chain,
       .root =
           {
               .id = value.finalized.id,
               .previous = value.state.header.previous,
               .num = value.finalized.num,
               .timestamp = block_timestamp{value.finalized.slot},
               .fork_rank = forge::chain::savanna::make_rank(value.state.finality, value.state.block),
               .state = value.state,
               .validation = value.validation,
           },
       .finalized = value.finalized.num,
   };
}

replay_seed make_seed(const finality_trust& trust) {
   return std::visit([](const auto& value) { return make_seed(value); }, trust);
}

const protocol::state_anchor& require_anchor(const finality_replay& replay, const protocol::state_anchor& expected,
                                             bool require_finalized) {
   if (replay.anchors.empty() || expected.block_num < replay.anchors.front().block_num) {
      FORGE_THROW_EXCEPTION(exceptions::finality_anchor_mismatch,
                            "Savanna finality witness does not cover the expected anchor");
   }
   const auto offset = static_cast<std::uint64_t>(expected.block_num) - replay.anchors.front().block_num;
   if (offset >= replay.anchors.size()) {
      FORGE_THROW_EXCEPTION(exceptions::finality_anchor_mismatch,
                            "Savanna finality witness does not cover the expected anchor");
   }
   const auto& actual = replay.anchors[static_cast<std::size_t>(offset)];
   if (actual != expected) {
      FORGE_THROW_EXCEPTION(exceptions::finality_anchor_mismatch,
                            "Savanna finality witness anchor does not match the expected state");
   }
   if (require_finalized &&
       (expected.block_num > replay.finalized_block_num || expected.block_num > replay.validated_block_num)) {
      FORGE_THROW_EXCEPTION(exceptions::finality_anchor_mismatch,
                            "Savanna finality witness does not finalize the expected anchor");
   }
   return actual;
}

protocol::signed_block make_header_only_block(const finality_witness_record& record) {
   auto block = protocol::signed_block{};
   static_cast<protocol::signed_block_header&>(block) = record.header;
   block.block_extensions = record.block_extensions;
   return block;
}

struct replay_result {
   finality_replay replay;
   std::optional<candidate> expected;
};

replay_result replay(const finality_trust& trust, const finality_witness& witness,
                     const std::optional<protocol::state_anchor>& expected, finality_witness_limits limits) {
   validate_limits(limits);
   validate_shape(witness, limits);
   static_cast<void>(measure_payload(witness, limits));

   auto seed = make_seed(trust);
   if (seed.chain != witness.chain) {
      FORGE_THROW_EXCEPTION(exceptions::finality_witness_wrong_chain,
                            "Savanna finality witness belongs to another chain");
   }
   if (seed.root.id != witness.trusted_bootstrap) {
      FORGE_THROW_EXCEPTION(exceptions::untrusted_finality_bootstrap,
                            "Savanna finality witness does not start at the caller-supplied bootstrap");
   }

   auto result = replay_result{
       .replay =
           {
               .anchors = {make_anchor(seed.chain, seed.root)},
               .finalized_block_num = seed.finalized,
               .validated_block_num = seed.finalized,
           },
   };
   if (expected && result.replay.anchors.front() == *expected) {
      result.expected = seed.root;
   }
   result.replay.anchors.reserve(witness.records.size() + 1U);
   result.replay.producer_opportunities.reserve(witness.records.size());
   auto parent = std::move(seed.root);
   for (const auto& record : witness.records) {
      auto block = make_header_only_block(record);
      auto prepared = prepare(parent, block);
      if (record.header.action_mroot != prepared.expected_action_root) {
         FORGE_THROW_EXCEPTION(exceptions::invalid_finality_witness,
                               "Savanna finality witness action root does not match parent validation state");
      }
      verify_signature(block, prepared);
      verify_qc(parent, prepared);
      const auto slot_count = static_cast<std::uint64_t>(record.header.timestamp.slot) - parent.timestamp.slot;
      const auto populated_slots = static_cast<std::uint64_t>(result.replay.producer_opportunities.size());
      const auto producer_slot_limit = static_cast<std::uint64_t>(limits.max_producer_slots);
      if (populated_slots > producer_slot_limit || slot_count > producer_slot_limit - populated_slots) {
         FORGE_THROW_EXCEPTION(exceptions::finality_witness_limit_exceeded,
                               "Savanna finality witness producer slots exceed the configured limit",
                               forge::exceptions::ctx("slots", slot_count),
                               forge::exceptions::ctx("limit", limits.max_producer_slots));
      }
      const auto produced_by = prepared.producer.producer_name;
      auto next = finish(parent, block, std::move(prepared));
      next.action_receipt_root = record.action_receipt_root;
      next.validation =
          forge::chain::savanna::append(parent.validation, {
                                                               .num = next.num,
                                                               .slot = next.timestamp.slot,
                                                               .parent_slot = parent.timestamp.slot,
                                                               .finality_digest = next.state.finality_digest(),
                                                               .commitment = next.action_receipt_root,
                                                           });
      result.replay.finalized_block_num =
          std::max(result.replay.finalized_block_num, next.state.finality.last_final_block_num());
      result.replay.validated_block_num =
          std::max(result.replay.validated_block_num, next.state.finality.latest_qc_claim().block);
      const auto anchor = make_anchor(seed.chain, next);
      if (expected && anchor == *expected) {
         result.expected = next;
      }
      result.replay.anchors.push_back(std::move(anchor));
      for (auto slot = parent.timestamp.slot + 1U; slot < record.header.timestamp.slot; ++slot) {
         const auto timestamp = block_timestamp{slot};
         result.replay.producer_opportunities.push_back({
             .timestamp = timestamp,
             .expected_producer = scheduled_producer(parent.state, timestamp).producer_name,
         });
      }
      result.replay.producer_opportunities.push_back({
          .timestamp = record.header.timestamp,
          .expected_producer = produced_by,
          .produced_block = next.id,
          .produced_block_num = next.num,
      });
      parent = std::move(next);
   }
   return result;
}

} // namespace

finality_witness make_finality_witness(protocol::chain_id chain, block_id trusted_bootstrap,
                                       std::span<const finality_witness_record> records,
                                       finality_witness_limits limits) {
   auto witness = finality_witness{
       .chain = std::move(chain),
       .trusted_bootstrap = std::move(trusted_bootstrap),
       .records = std::vector<finality_witness_record>{records.begin(), records.end()},
   };
   validate_limits(limits);
   validate_shape(witness, limits);
   static_cast<void>(measure_payload(witness, limits));
   return witness;
}

protocol::proof_blob encode_finality_witness(const finality_witness& witness, finality_witness_limits limits) {
   return {
       .scheme = std::string{finality_witness_scheme},
       .version = finality_witness_version,
       .payload = encode_payload(witness, limits),
   };
}

finality_witness decode_finality_witness(const protocol::proof_blob& proof, finality_witness_limits limits) {
   if (proof.scheme != finality_witness_scheme || proof.version != finality_witness_version) {
      FORGE_THROW_EXCEPTION(
          exceptions::invalid_finality_witness, "Savanna finality witness proof scheme or version is unsupported",
          forge::exceptions::ctx("scheme", proof.scheme), forge::exceptions::ctx("version", proof.version));
   }
   return decode_payload(std::span<const std::uint8_t>{proof.payload.data(), proof.payload.size()}, limits);
}

finality_replay replay_finality_witness(const finality_trust& trust, const finality_witness& witness,
                                        finality_witness_limits limits) {
   return replay(trust, witness, std::nullopt, limits).replay;
}

header_state replay_finality_witness_state(const finality_trust& trust, const finality_witness& witness,
                                           const protocol::state_anchor& expected, finality_witness_limits limits) {
   auto result = replay(trust, witness, expected, limits);
   static_cast<void>(require_anchor(result.replay, expected, true));
   if (!result.expected) {
      FORGE_THROW_EXCEPTION(exceptions::finality_anchor_mismatch,
                            "Savanna finality witness state is unavailable for the expected anchor");
   }
   return std::move(result.expected->state);
}

finality_trust_advance advance_finality_trust_with_replay(const finality_trust& trust, const finality_witness& witness,
                                                          const protocol::state_anchor& finalized,
                                                          finality_witness_limits limits) {
   auto result = replay(trust, witness, finalized, limits);
   static_cast<void>(require_anchor(result.replay, finalized, true));
   if (!result.expected) {
      FORGE_THROW_EXCEPTION(exceptions::finality_anchor_mismatch,
                            "Savanna finality witness checkpoint state is unavailable for the finalized anchor");
   }

   auto candidate = std::move(*result.expected);
   return {
       .checkpoint =
           {
               .chain = finalized.chain,
               .value =
                   {
                       .finalized = candidate.state.make_block_ref(),
                       .state = std::move(candidate.state),
                       .validation = advance_finalized(std::move(candidate.validation), finalized.block_num),
                   },
           },
       .replay = std::move(result.replay),
   };
}

finality_checkpoint_bootstrap advance_finality_trust(const finality_trust& trust, const finality_witness& witness,
                                                     const protocol::state_anchor& finalized,
                                                     finality_witness_limits limits) {
   return advance_finality_trust_with_replay(trust, witness, finalized, limits).checkpoint;
}

finality_checkpoint_bootstrap advance_finality_trust(const finality_trust& trust, const protocol::proof_blob& proof,
                                                     const protocol::state_anchor& finalized,
                                                     finality_witness_limits limits) {
   return advance_finality_trust(trust, decode_finality_witness(proof, limits), finalized, limits);
}

finality_trust_anchor trust_anchor(const finality_trust& trust) {
   auto seed = make_seed(trust);
   return {
       .chain = std::move(seed.chain),
       .block = std::move(seed.root.id),
   };
}

void verify_finality_witness(const finality_trust& trust, const protocol::proof_blob& proof,
                             const protocol::state_anchor& expected, finality_witness_limits limits) {
   const auto witness = decode_finality_witness(proof, limits);
   const auto replay = replay_finality_witness(trust, witness, limits);
   static_cast<void>(require_anchor(replay, expected, true));
}

void verify_finality_ancestry_witness(const finality_trust& trust, const protocol::proof_blob& proof,
                                      const protocol::state_anchor& finalized,
                                      std::span<const protocol::state_anchor> intermediate,
                                      finality_witness_limits limits) {
   const auto witness = decode_finality_witness(proof, limits);
   const auto replay = replay_finality_witness(trust, witness, limits);
   static_cast<void>(require_anchor(replay, finalized, true));

   auto previous = std::optional<block_num>{};
   for (const auto& anchor : intermediate) {
      if (anchor.block_num >= finalized.block_num || (previous && anchor.block_num <= *previous)) {
         FORGE_THROW_EXCEPTION(
             exceptions::finality_anchor_mismatch,
             "Savanna finality ancestry anchors are not strictly ordered before the finalized anchor");
      }
      static_cast<void>(require_anchor(replay, anchor, false));
      previous = anchor.block_num;
   }
}

} // namespace forge::chain::savanna
