module;

#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/use_future.hpp>
#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <functional>
#include <iterator>
#include <latch>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

module forge.chain.api.savanna_finality_verifier;

import forge.api.core.registry;
import forge.chain.api.contract_table_projection_verifier;
import forge.chain.api.exceptions;
import forge.chain.api.table_key;
import forge.chain.api.verified_client_factory;
import forge.chain.protocol.contract_commitment;
import forge.chain.savanna.finality_witness;
import forge.chain.savanna.qc;
import forge.chain.savanna.validation;
import forge.codec.hex;
import forge.crypto.asymmetric.secp256k1;
import forge.crypto.asymmetric.values;
import forge.crypto.bls;
import forge.crypto.digest.sha256;
import forge.db.authenticated.codec;
import forge.db.authenticated.hash;
import forge.db.authenticated.proof;
import forge.db.authenticated.types;
import forge.raw.raw;

#include "../../libraries/chain/api/details/savanna_finality_trust_store.hxx"

namespace {

namespace api = forge::chain::api;
namespace authenticated = forge::db::authenticated;
namespace asymmetric = forge::crypto::asymmetric;
namespace bls = forge::crypto::bls;
namespace protocol = forge::chain::protocol;
namespace savanna = forge::chain::savanna;

using producer_key = asymmetric::secp256k1::private_key;

template <typename Value> Value run(boost::asio::awaitable<Value> operation) {
   auto context = boost::asio::io_context{};
   auto result = boost::asio::co_spawn(context, std::move(operation), boost::asio::use_future);
   context.run();
   return result.get();
}

authenticated::bytes db_bytes(std::span<const std::byte> value) {
   return {value.begin(), value.end()};
}

authenticated::bytes db_bytes(const protocol::bytes& value) {
   auto result = authenticated::bytes{};
   result.reserve(value.size());
   std::ranges::transform(value, std::back_inserter(result),
                          [](std::uint8_t byte) { return static_cast<std::byte>(byte); });
   return result;
}

protocol::bytes wire_bytes(std::span<const std::byte> value) {
   auto result = protocol::bytes{};
   result.reserve(value.size());
   std::ranges::transform(value, std::back_inserter(result),
                          [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
   return result;
}

authenticated::proof_leaf make_leaf(authenticated::bytes key, authenticated::bytes value) {
   return {
       .key = std::move(key),
       .value_hash = authenticated::hash_value(value),
       .value = std::move(value),
   };
}

authenticated::proof_leaf make_change_leaf(const authenticated::mutation& mutation) {
   auto encoded = authenticated::encode_change_value(mutation);
   return {
       .key = mutation.key,
       .value_hash = authenticated::hash_value(encoded),
       .value = std::move(encoded),
   };
}

authenticated::digest leaf_hash(std::string_view domain, const authenticated::proof_leaf& leaf) {
   return authenticated::hash_leaf(domain, leaf.key, leaf.value_hash);
}

template <typename Proof> protocol::proof_blob proof_blob(std::string scheme, const Proof& proof) {
   return {
       .scheme = std::move(scheme),
       .version = 1U,
       .payload = wire_bytes(authenticated::encode(proof)),
   };
}

struct tree_summary {
   std::uint16_t height = 0;
   std::uint64_t size = 0;
   authenticated::bytes min_key;
   authenticated::bytes max_key;
   authenticated::bytes separator;
   authenticated::digest hash;
   authenticated::digest left_hash;
   authenticated::digest right_hash;
};

tree_summary summarize_tree(std::string_view domain, std::span<const authenticated::proof_leaf> leaves,
                            std::size_t begin, std::size_t end) {
   if (end - begin == 1U) {
      return {
          .height = 0U,
          .size = 1U,
          .min_key = leaves[begin].key,
          .max_key = leaves[begin].key,
          .hash = leaf_hash(domain, leaves[begin]),
      };
   }
   const auto middle = begin + (end - begin) / 2U;
   const auto left = summarize_tree(domain, leaves, begin, middle);
   const auto right = summarize_tree(domain, leaves, middle, end);
   const auto height = static_cast<std::uint16_t>(std::max(left.height, right.height) + 1U);
   return {
       .height = height,
       .size = left.size + right.size,
       .min_key = left.min_key,
       .max_key = right.max_key,
       .separator = right.min_key,
       .hash = authenticated::hash_inner(domain, height, left.size + right.size, left.min_key, right.max_key,
                                         right.min_key, left.hash, right.hash),
       .left_hash = left.hash,
       .right_hash = right.hash,
   };
}

authenticated::proof_sibling proof_sibling(const tree_summary& summary,
                                           std::span<const authenticated::proof_leaf> leaves, std::size_t begin,
                                           std::size_t end) {
   if (end - begin == 1U) {
      return leaves[begin];
   }
   return authenticated::proof_branch{
       .height = summary.height,
       .size = summary.size,
       .min_key = summary.min_key,
       .max_key = summary.max_key,
       .separator = summary.separator,
       .left_hash = summary.left_hash,
       .right_hash = summary.right_hash,
   };
}

void emit_range_tree(std::string_view domain, std::span<const authenticated::proof_leaf> leaves, std::size_t begin,
                     std::size_t end, std::vector<authenticated::range_proof_node>& output) {
   if (end - begin == 1U) {
      output.emplace_back(leaves[begin]);
      return;
   }
   const auto summary = summarize_tree(domain, leaves, begin, end);
   output.emplace_back(authenticated::range_inner{
       .height = summary.height,
       .size = summary.size,
       .min_key = summary.min_key,
       .max_key = summary.max_key,
       .separator = summary.separator,
   });
   const auto middle = begin + (end - begin) / 2U;
   emit_range_tree(domain, leaves, begin, middle, output);
   emit_range_tree(domain, leaves, middle, end, output);
}

void emit_point_path(std::string_view domain, std::span<const authenticated::proof_leaf> leaves, std::size_t begin,
                     std::size_t end, std::size_t target, std::vector<authenticated::proof_step>& output) {
   if (end - begin == 1U) {
      return;
   }
   const auto summary = summarize_tree(domain, leaves, begin, end);
   const auto middle = begin + (end - begin) / 2U;
   if (target < middle) {
      emit_point_path(domain, leaves, begin, middle, target, output);
      const auto sibling = summarize_tree(domain, leaves, middle, end);
      output.push_back({
          .child = authenticated::branch_side::left,
          .height = summary.height,
          .subtree_size = summary.size,
          .min_key = summary.min_key,
          .max_key = summary.max_key,
          .separator = summary.separator,
          .sibling = proof_sibling(sibling, leaves, middle, end),
      });
   } else {
      emit_point_path(domain, leaves, middle, end, target, output);
      const auto sibling = summarize_tree(domain, leaves, begin, middle);
      output.push_back({
          .child = authenticated::branch_side::right,
          .height = summary.height,
          .subtree_size = summary.size,
          .min_key = summary.min_key,
          .max_key = summary.max_key,
          .separator = summary.separator,
          .sibling = proof_sibling(sibling, leaves, begin, middle),
      });
   }
}

struct authenticated_tree_fixture {
   std::string domain;
   authenticated::proof_tree tree = authenticated::proof_tree::state;
   std::vector<authenticated::proof_leaf> leaves;

   [[nodiscard]] tree_summary summary() const {
      if (leaves.empty()) {
         throw std::logic_error{"test authenticated tree cannot be empty"};
      }
      return summarize_tree(domain, leaves, 0U, leaves.size());
   }

   [[nodiscard]] authenticated::range_proof range(authenticated::root anchor,
                                                  authenticated::range_request request) const {
      auto nodes = std::vector<authenticated::range_proof_node>{};
      emit_range_tree(domain, leaves, 0U, leaves.size(), nodes);
      return {
          .anchor = std::move(anchor),
          .tree = tree,
          .request = std::move(request),
          .nodes = std::move(nodes),
      };
   }

   [[nodiscard]] authenticated::point_proof point(authenticated::root anchor, const authenticated::bytes& key) const {
      const auto found = std::ranges::find(leaves, key, &authenticated::proof_leaf::key);
      if (found == leaves.end()) {
         throw std::logic_error{"test point proof key is absent"};
      }
      const auto target = static_cast<std::size_t>(std::distance(leaves.begin(), found));
      auto path = std::vector<authenticated::proof_step>{};
      emit_point_path(domain, leaves, 0U, leaves.size(), target, path);
      return {
          .anchor = std::move(anchor),
          .key = key,
          .terminal = *found,
          .path = std::move(path),
      };
   }
};

authenticated::bytes prefix_upper_bound(authenticated::bytes prefix) {
   for (auto position = prefix.size(); position > 0U; --position) {
      const auto index = position - 1U;
      const auto value = std::to_integer<std::uint8_t>(prefix[index]);
      if (value != 0xffU) {
         prefix[index] = static_cast<std::byte>(value + 1U);
         prefix.resize(position);
         return prefix;
      }
   }
   throw std::logic_error{"test prefix has no upper bound"};
}

struct finalizer_material {
   bls::private_key key;
   savanna::finalizer authority;
   bls::signature proof;
};

finalizer_material make_finalizer(std::uint8_t base = 1U) {
   auto seed = std::array<std::uint8_t, 32>{};
   for (auto index = std::size_t{}; index < seed.size(); ++index) {
      seed[index] = static_cast<std::uint8_t>(base + index);
   }
   auto key = bls::private_key{std::span<const std::uint8_t>{seed}};
   auto authority = savanna::finalizer{
       .description = "portable",
       .weight = 1U,
       .public_key = key.get_public_key(),
   };
   auto proof = key.proof_of_possession();
   return {
       .key = std::move(key),
       .authority = std::move(authority),
       .proof = std::move(proof),
   };
}

producer_key make_producer_key(std::uint64_t discriminator = 0U) {
   auto secret = forge::crypto::digest::sha256{};
   secret._hash[0] = 0x89ab'cdef'0123'4567ULL ^ discriminator;
   secret._hash[1] = 0x7654'3210'fedc'ba98ULL;
   secret._hash[2] = 0x1357'9bdf'2468'ace0ULL;
   secret._hash[3] = 0x0eca'8642'fdb9'7531ULL;
   return producer_key::regenerate(secret);
}

protocol::public_key public_key(const producer_key& key) {
   return asymmetric::k1_public_key{key.get_public_key().serialize()};
}

protocol::signature sign(const producer_key& key, const savanna::digest& digest) {
   return asymmetric::k1_signature{std::bit_cast<asymmetric::ecc_signature>(key.sign_digest(digest))};
}

savanna::digest make_digest(std::uint64_t value) {
   auto result = savanna::digest{};
   result._hash[0] = value;
   result._hash[1] = value ^ 0xa5a5'a5a5'a5a5'a5a5ULL;
   return result;
}

savanna::genesis make_genesis(const producer_key& producer, const finalizer_material& finalizer) {
   return {
       .timestamp = savanna::block_timestamp{12U},
       .system_key = public_key(producer),
       .proposers =
           {
               .version = 0U,
               .producers = {{
                   .producer_name = protocol::account_name{"eosio"},
                   .authority =
                       protocol::block_signing_authority_v0{
                           .threshold = 1U,
                           .keys = {{.key = public_key(producer), .weight = 1U}},
                       },
               }},
           },
       .finalizers = {.generation = 1U, .threshold = 1U, .finalizers = {finalizer.authority}},
       .finalizer_proofs = {finalizer.proof},
   };
}

savanna::quorum_certificate make_current_qc(const savanna::candidate& parent, const finalizer_material& finalizer) {
   auto votes = savanna::vote_bitset{1U};
   votes.set(0U);
   auto signatures = bls::signature_accumulator{};
   const auto finality_digest = parent.state.make_block_ref().finality_digest;
   signatures.add(finalizer.key.sign(finality_digest.to_uint8_span()));
   return {
       .block = parent.state.finality.current_block_num(),
       .active = {.strong_votes = std::move(votes), .signature = signatures.finish()},
   };
}

protocol::signed_block make_child(const savanna::candidate& parent, const producer_key& producer,
                                  savanna::state_commitment commitment,
                                  std::optional<savanna::quorum_certificate> certificate = std::nullopt) {
   auto block = protocol::signed_block{};
   block.timestamp = savanna::block_timestamp{parent.timestamp.slot + 1U};
   block.producer = protocol::account_name{"eosio"};
   block.confirmed = 0U;
   block.previous = parent.id;
   block.schedule_version = savanna::proper_savanna_schedule_version;
   const auto claim = certificate ? certificate->claim() : parent.state.finality.latest_qc_claim();
   block.header_extensions.emplace_back(savanna::finality_extension_id,
                                        forge::raw::pack(savanna::finality_extension{.claim = claim}));
   block.header_extensions.emplace_back(savanna::state_commitment_extension_id, forge::raw::pack(commitment));
   if (certificate) {
      block.block_extensions.emplace_back(
          savanna::quorum_certificate_extension_id,
          forge::raw::pack(savanna::quorum_certificate_extension{.certificate = std::move(*certificate)}));
   }
   block.transaction_mroot = protocol::calculate_transaction_mroot(block.transactions);
   const auto metadata = parent.state.finality.next_metadata(claim);
   block.action_mroot = parent.state.finality.is_genesis_block_num(metadata.latest_qc)
                            ? savanna::digest{}
                            : savanna::root_at(parent.validation, metadata.latest_qc);
   block.producer_signature = sign(producer, protocol::calculate_block_id(block));
   return block;
}

savanna::candidate admit_with_receipt(const savanna::candidate& parent, const protocol::signed_block& block,
                                      savanna::digest receipt) {
   auto next = savanna::admit(parent, block);
   next.action_receipt_root = receipt;
   next.validation = savanna::append(parent.validation, {
                                                            .num = next.num,
                                                            .slot = next.timestamp.slot,
                                                            .parent_slot = parent.timestamp.slot,
                                                            .finality_digest = next.state.finality_digest(),
                                                            .commitment = receipt,
                                                        });
   return next;
}

savanna::finality_witness_record make_record(const protocol::signed_block& block, savanna::digest receipt) {
   return {
       .header = static_cast<const protocol::signed_block_header&>(block),
       .block_extensions = block.block_extensions,
       .action_receipt_root = receipt,
   };
}

protocol::state_anchor make_anchor(const protocol::chain_id& chain, const savanna::candidate& value) {
   const auto commitment = savanna::decode_header_extensions(value.state.header.header_extensions).commitment;
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

struct rolling_witness_chain_fixture {
   struct forked_witness {
      protocol::state_anchor target;
      protocol::proof_blob proof;
   };

   protocol::chain_id chain;
   savanna::finality_trust genesis_trust;
   std::vector<savanna::candidate> candidates;
   std::vector<savanna::finality_witness_record> records;

   [[nodiscard]] const savanna::candidate& candidate(protocol::block_num block_num) const {
      if (block_num == 0U) {
         throw std::logic_error{"rolling witness fixture block numbers start at one"};
      }
      return candidates.at(static_cast<std::size_t>(block_num - 1U));
   }

   [[nodiscard]] protocol::state_anchor anchor(protocol::block_num block_num) const {
      return make_anchor(chain, candidate(block_num));
   }

   [[nodiscard]] protocol::proof_blob proof(protocol::block_num source, protocol::block_num finalized) const {
      if (source == 0U || finalized <= source || finalized > std::numeric_limits<protocol::block_num>::max() - 2U) {
         throw std::logic_error{"rolling witness fixture proof range is invalid"};
      }
      const auto terminal = static_cast<protocol::block_num>(finalized + 2U);
      if (terminal > candidates.size()) {
         throw std::logic_error{"rolling witness fixture proof exceeds the generated chain"};
      }
      const auto begin = static_cast<std::size_t>(source - 1U);
      const auto count = static_cast<std::size_t>(terminal - source);
      return savanna::encode_finality_witness(savanna::make_finality_witness(
          chain, candidate(source).id,
          std::span<const savanna::finality_witness_record>{records}.subspan(begin, count)));
   }

   [[nodiscard]] protocol::proof_blob bootstrap_proof(protocol::block_num source) const {
      return savanna::encode_finality_witness(savanna::make_finality_witness(
          chain, candidate(source).id, std::span<const savanna::finality_witness_record>{}));
   }

   [[nodiscard]] forked_witness conflicting_proof(protocol::block_num source, protocol::block_num finalized) const {
      if (source == 0U || finalized <= source || finalized > std::numeric_limits<protocol::block_num>::max() - 2U) {
         throw std::logic_error{"rolling witness fixture conflicting proof range is invalid"};
      }
      const auto terminal = static_cast<protocol::block_num>(finalized + 2U);
      if (terminal > candidates.size()) {
         throw std::logic_error{"rolling witness fixture conflicting proof exceeds the generated chain"};
      }

      const auto producer = make_producer_key(19U);
      const auto finalizer = make_finalizer(29U);
      auto parent = candidate(source);
      auto records = std::vector<savanna::finality_witness_record>{};
      records.reserve(static_cast<std::size_t>(terminal - source));
      auto target = std::optional<protocol::state_anchor>{};
      for (auto block_num = static_cast<protocol::block_num>(source + 1U); block_num <= terminal; ++block_num) {
         const auto commitment = block_num == source + 1U
                                     ? savanna::state_commitment{
                                           .state_root = make_digest(9'000U + source),
                                           .state_size = 2U,
                                           .change_root = make_digest(9'100U + finalized),
                                           .change_count = 1U,
                                       }
                                     : savanna::state_commitment{};
         const auto block =
             block_num == 2U ? make_child(parent, producer, std::move(commitment))
                             : make_child(parent, producer, std::move(commitment), make_current_qc(parent, finalizer));
         const auto receipt = make_digest(9'200U + block_num);
         auto next = admit_with_receipt(parent, block, receipt);
         if (block_num == finalized) {
            target = make_anchor(chain, next);
         }
         records.push_back(make_record(block, receipt));
         parent = std::move(next);
      }
      if (!target) {
         throw std::logic_error{"rolling witness fixture did not build its conflicting target"};
      }
      return {
          .target = std::move(*target),
          .proof =
              savanna::encode_finality_witness(savanna::make_finality_witness(chain, candidate(source).id, records)),
      };
   }
};

rolling_witness_chain_fixture make_rolling_witness_chain(protocol::block_num last_block_num = 21U) {
   if (last_block_num < 4U) {
      throw std::logic_error{"rolling witness fixture requires at least four blocks"};
   }

   const auto producer = make_producer_key(19U);
   const auto finalizer = make_finalizer(29U);
   const auto configuration = make_genesis(producer, finalizer);
   const auto genesis_commitment = savanna::state_commitment{
       .state_root = make_digest(701U),
       .state_size = 1U,
       .change_root = make_digest(702U),
       .change_count = 1U,
   };
   const auto genesis = savanna::make_genesis_candidate(configuration, genesis_commitment);
   auto fixture = rolling_witness_chain_fixture{
       .chain = savanna::calculate_chain_id(configuration),
       .genesis_trust =
           savanna::finality_genesis_bootstrap{
               .configuration = configuration,
               .commitment = genesis_commitment,
           },
       .candidates = {genesis.value},
   };
   fixture.candidates.reserve(last_block_num);
   fixture.records.reserve(static_cast<std::size_t>(last_block_num - 1U));

   for (auto block_num = protocol::block_num{2U}; block_num <= last_block_num; ++block_num) {
      const auto& parent = fixture.candidates.back();
      auto block = block_num == 2U
                       ? make_child(parent, producer, savanna::state_commitment{})
                       : make_child(parent, producer, savanna::state_commitment{}, make_current_qc(parent, finalizer));
      const auto receipt = make_digest(800U + block_num);
      auto next = admit_with_receipt(parent, block, receipt);
      fixture.records.push_back(make_record(block, receipt));
      fixture.candidates.push_back(std::move(next));
   }
   return fixture;
}

enum class trusted_chain_behavior {
   value,
   missing,
   public_invalid_request,
   public_wrong_chain,
   public_resource_exhausted,
   foreign_forge,
   bad_alloc,
   length_error,
   standard_error,
   nonstandard_error,
};

class preflight_finality_verifier final : public api::finality_verifier {
 public:
   preflight_finality_verifier(protocol::chain_id chain, trusted_chain_behavior behavior)
       : chain_{std::move(chain)}, behavior_{behavior} {}

   [[nodiscard]] std::optional<protocol::chain_id> trusted_chain() const override {
      switch (behavior_) {
      case trusted_chain_behavior::value:
         return chain_;
      case trusted_chain_behavior::missing:
         return std::nullopt;
      case trusted_chain_behavior::public_invalid_request:
         throw api::exceptions::invalid_request{"test public preflight failure"};
      case trusted_chain_behavior::public_wrong_chain:
         throw api::exceptions::wrong_chain{"test public preflight failure"};
      case trusted_chain_behavior::public_resource_exhausted:
         throw api::exceptions::resource_exhausted{"test public preflight failure"};
      case trusted_chain_behavior::foreign_forge:
         throw savanna::exceptions::invalid_header{"test foreign Forge preflight failure"};
      case trusted_chain_behavior::bad_alloc:
         throw std::bad_alloc{};
      case trusted_chain_behavior::length_error:
         throw std::length_error{"test preflight length failure"};
      case trusted_chain_behavior::standard_error:
         throw std::runtime_error{"test preflight standard failure"};
      case trusted_chain_behavior::nonstandard_error:
         throw 7;
      }
      throw std::logic_error{"unknown test preflight behavior"};
   }

   void verify(const protocol::state_anchor&, const protocol::proof_blob&) override {}

   void verify_ancestry(const protocol::state_anchor&, std::span<const protocol::state_anchor>,
                        const protocol::proof_blob&) override {}

 private:
   protocol::chain_id chain_;
   trusted_chain_behavior behavior_;
};

class fixture_state_service final : public api::state {
 public:
   using rows_handler = std::function<protocol::table_rows_response(const protocol::table_rows_request&)>;
   using changes_handler = std::function<protocol::table_changes_response(const protocol::table_changes_request&)>;

   fixture_state_service(protocol::table_rows_response rows, protocol::table_changes_response changes)
       : rows_{std::move(rows)}, changes_{std::move(changes)} {}

   fixture_state_service(rows_handler rows, changes_handler changes, protocol::response_context native_context,
                         protocol::proof_blob native_finality)
       : rows_handler_{std::move(rows)}, changes_handler_{std::move(changes)},
         native_context_{std::move(native_context)}, native_finality_{std::move(native_finality)} {}

   boost::asio::awaitable<protocol::account_response> get_account(protocol::account_request) override {
      auto response = protocol::account_response{};
      add_native_audit(response);
      co_return response;
   }
   boost::asio::awaitable<protocol::account_changes_response>
   get_account_changes(protocol::account_changes_request) override {
      co_return protocol::account_changes_response{};
   }
   boost::asio::awaitable<protocol::code_response> get_code(protocol::code_request) override {
      co_return protocol::code_response{};
   }
   boost::asio::awaitable<protocol::permission_links_response>
   get_permission_links(protocol::permission_links_request) override {
      co_return protocol::permission_links_response{};
   }
   boost::asio::awaitable<protocol::table_rows_response> get_table_rows(protocol::table_rows_request request) override {
      rows_requests_.push_back(request);
      co_return rows_handler_ ? rows_handler_(request) : rows_;
   }
   boost::asio::awaitable<protocol::table_changes_response>
   get_table_changes(protocol::table_changes_request request) override {
      changes_requests_.push_back(request);
      co_return changes_handler_ ? changes_handler_(request) : changes_;
   }
   boost::asio::awaitable<protocol::table_scope_response> get_table_scope(protocol::table_scope_request) override {
      auto response = protocol::table_scope_response{};
      add_native_audit(response);
      co_return response;
   }
   boost::asio::awaitable<protocol::currency_balance_response>
   get_currency_balance(protocol::currency_balance_request) override {
      co_return protocol::currency_balance_response{};
   }
   boost::asio::awaitable<protocol::currency_stats_response>
   get_currency_stats(protocol::currency_stats_request) override {
      co_return protocol::currency_stats_response{};
   }
   boost::asio::awaitable<protocol::scheduled_response>
   get_scheduled_transactions(protocol::scheduled_request) override {
      co_return protocol::scheduled_response{};
   }
   boost::asio::awaitable<protocol::authorizers_response>
   get_accounts_by_authorizers(protocol::authorizers_request) override {
      co_return protocol::authorizers_response{};
   }

   [[nodiscard]] const std::vector<protocol::table_rows_request>& rows_requests() const noexcept {
      return rows_requests_;
   }

   [[nodiscard]] const std::vector<protocol::table_changes_request>& changes_requests() const noexcept {
      return changes_requests_;
   }

 private:
   template <typename Response> void add_native_audit(Response& response) const {
      if (!native_context_ || !native_finality_) {
         return;
      }
      response.context = *native_context_;
      response.audit = protocol::audit_bundle{.finality = *native_finality_};
   }

   protocol::table_rows_response rows_;
   protocol::table_changes_response changes_;
   rows_handler rows_handler_;
   changes_handler changes_handler_;
   std::optional<protocol::response_context> native_context_;
   std::optional<protocol::proof_blob> native_finality_;
   std::vector<protocol::table_rows_request> rows_requests_;
   std::vector<protocol::table_changes_request> changes_requests_;
};

struct portable_fixture {
   std::string state_domain = "spine.chain.state";
   producer_key producer;
   finalizer_material finalizer;
   savanna::genesis configuration;
   savanna::finality_trust trust;
   protocol::chain_id chain;
   protocol::state_anchor anchor;
   protocol::proof_blob finality_proof;
   protocol::table_rows_request rows_request;
   protocol::table_rows_response rows_response;
   authenticated::range_proof rows_proof;
   protocol::table_changes_request changes_request;
   protocol::table_changes_response changes_response;
   authenticated::range_proof changes_proof;
};

portable_fixture make_portable_fixture() {
   const auto location = protocol::contract_table_location{
       .code = protocol::account_name{"contract"}.value,
       .scope = protocol::name{"scope"}.value,
       .table = protocol::table_name{"rows"}.value,
   };
   const auto primary = std::uint64_t{7U};
   const auto stored = protocol::primary_value{
       .payer = protocol::account_name{"payer"},
       .row = {0xdeU, 0xadU, 0xbeU, 0xefU},
   };
   const auto key = protocol::contract_primary_key(location, protocol::contract_table_family::primary, primary);
   const auto value = forge::raw::pack(stored);
   const auto state_leaf = make_leaf(db_bytes(key), db_bytes(value));
   const auto mutation = authenticated::mutation{.key = state_leaf.key, .value = *state_leaf.value};
   const auto change_leaf = make_change_leaf(mutation);
   const auto state_domain = std::string{"spine.chain.state"};
   const auto state_tree_domain = authenticated::canonical_tree_domain(state_domain, authenticated::proof_tree::state);
   const auto change_tree_domain =
       authenticated::canonical_tree_domain(state_domain, authenticated::proof_tree::changes);
   const auto root = authenticated::root{
       .state_root = leaf_hash(state_tree_domain, state_leaf),
       .state_size = 1U,
       .change_root = leaf_hash(change_tree_domain, change_leaf),
       .change_count = 1U,
   };

   auto producer = make_producer_key();
   auto finalizer = make_finalizer();
   auto configuration = make_genesis(producer, finalizer);
   const auto genesis_commitment = savanna::state_commitment{
       .state_root = make_digest(101U),
       .state_size = 1U,
       .change_root = make_digest(102U),
       .change_count = 1U,
   };
   const auto final_commitment = savanna::state_commitment{
       .state_root = root.state_root,
       .state_size = root.state_size,
       .change_root = root.change_root,
       .change_count = root.change_count,
   };
   const auto genesis = savanna::make_genesis_candidate(configuration, genesis_commitment);
   const auto chain = savanna::calculate_chain_id(configuration);

   const auto first_block = make_child(genesis.value, producer, savanna::state_commitment{});
   const auto first_receipt = make_digest(201U);
   const auto first = admit_with_receipt(genesis.value, first_block, first_receipt);
   const auto second_block =
       make_child(first, producer, savanna::state_commitment{}, make_current_qc(first, finalizer));
   const auto second_receipt = make_digest(202U);
   const auto second = admit_with_receipt(first, second_block, second_receipt);
   const auto third_block = make_child(second, producer, final_commitment, make_current_qc(second, finalizer));
   const auto third_receipt = make_digest(203U);
   const auto third = admit_with_receipt(second, third_block, third_receipt);
   const auto fourth_block =
       make_child(third, producer, savanna::state_commitment{}, make_current_qc(third, finalizer));
   const auto fourth_receipt = make_digest(204U);
   const auto fourth = admit_with_receipt(third, fourth_block, fourth_receipt);
   const auto fifth_block =
       make_child(fourth, producer, savanna::state_commitment{}, make_current_qc(fourth, finalizer));
   const auto fifth_receipt = make_digest(205U);
   const auto fifth = admit_with_receipt(fourth, fifth_block, fifth_receipt);

   const auto records = std::array{
       make_record(first_block, first_receipt), make_record(second_block, second_receipt),
       make_record(third_block, third_receipt), make_record(fourth_block, fourth_receipt),
       make_record(fifth_block, fifth_receipt),
   };
   const auto witness = savanna::make_finality_witness(chain, genesis.value.id, records);
   const auto anchor = make_anchor(chain, third);
   auto proof_root = root;
   proof_root.version = anchor.block_num;

   const auto prefix = protocol::contract_index_prefix(location, protocol::contract_table_family::primary);
   const auto lower = db_bytes(prefix);
   const auto upper = prefix_upper_bound(lower);
   const auto rows_request = protocol::table_rows_request{
       .code = protocol::account_name{location.code},
       .scope = protocol::name{location.scope},
       .table = protocol::table_name{location.table},
       .index = {.kind = protocol::table_index_kind::primary},
       .limit = 2U,
       .audit = protocol::audit_mode::required,
   };
   const auto rows_range_request = authenticated::range_request{
       .lower = lower,
       .upper = upper,
       .limit = rows_request.limit,
       .include_values = true,
   };
   const auto rows_proof = authenticated::range_proof{
       .anchor = proof_root,
       .tree = authenticated::proof_tree::state,
       .request = rows_range_request,
       .nodes = {state_leaf},
   };
   const auto finality_proof = savanna::encode_finality_witness(witness);
   const auto context = protocol::response_context{
       .chain = chain,
       .head = fifth.id,
       .finalized = anchor.block,
       .anchor = anchor,
   };
   auto rows_response = protocol::table_rows_response{};
   rows_response.context = context;
   rows_response.audit = protocol::audit_bundle{
       .finality = finality_proof,
       .state = {proof_blob("forge.db.authenticated.range", rows_proof)},
   };
   rows_response.rows = {{.value = stored.row, .payer = stored.payer}};

   const auto selector = protocol::table_change_selector{
       .code = rows_request.code,
       .scope = rows_request.scope,
       .table = rows_request.table,
   };
   const auto changes_request = protocol::table_changes_request{
       .from_block = anchor.block_num - 1U,
       .to_block = anchor.block_num,
       .tables = {selector},
       .limit = 2U,
       .audit = protocol::audit_mode::required,
   };
   const auto changes_location = protocol::contract_table_location{
       .code = selector.code.value,
       .scope = selector.scope.value,
       .table = selector.table.value,
   };
   const auto changes_prefix =
       protocol::contract_index_prefix(changes_location, protocol::contract_table_family::primary);
   const auto changes_lower = db_bytes(changes_prefix);
   const auto changes_upper = prefix_upper_bound(changes_lower);
   const auto changes_range_request = authenticated::range_request{
       .lower = changes_lower,
       .upper = changes_upper,
       .limit = changes_request.limit,
       .include_values = true,
   };
   const auto changes_proof = authenticated::range_proof{
       .anchor = proof_root,
       .tree = authenticated::proof_tree::changes,
       .request = changes_range_request,
       .nodes = {change_leaf},
   };
   auto changes_response = protocol::table_changes_response{};
   changes_response.context = context;
   changes_response.audit = protocol::audit_bundle{
       .finality = finality_proof,
       .state = {proof_blob("forge.db.authenticated.changes", changes_proof)},
   };
   changes_response.blocks = {{
       .anchor = anchor,
       .mutations = {{.table = selector,
                      .primary = primary,
                      .row = protocol::table_row{.value = stored.row, .payer = stored.payer}}},
   }};

   return {
       .state_domain = state_domain,
       .producer = std::move(producer),
       .finalizer = std::move(finalizer),
       .configuration = configuration,
       .trust =
           savanna::finality_genesis_bootstrap{
               .configuration = configuration,
               .commitment = genesis_commitment,
           },
       .chain = chain,
       .anchor = anchor,
       .finality_proof = finality_proof,
       .rows_request = rows_request,
       .rows_response = rows_response,
       .rows_proof = rows_proof,
       .changes_request = changes_request,
       .changes_response = changes_response,
       .changes_proof = changes_proof,
   };
}

api::verified_client make_client(const portable_fixture& fixture, protocol::table_rows_response rows,
                                 protocol::table_changes_response changes) {
   auto services = forge::api::core::registry{};
   services.install<api::state>(std::make_shared<fixture_state_service>(std::move(rows), std::move(changes)));
   auto raw = api::raw_client{api::service_handles{
       .state_queries = services.get<api::state>(api::state::ref()),
   }};
   return api::make_verified_client(std::move(raw), {
                                                        .chain = fixture.chain,
                                                        .state_domain = fixture.state_domain,
                                                        .finality = api::make_savanna_finality_verifier(fixture.trust),
                                                        .projections = api::make_contract_table_projection_verifier(),
                                                    });
}

api::verified_client make_rows_client(const portable_fixture& fixture, protocol::table_rows_response response) {
   return make_client(fixture, std::move(response), fixture.changes_response);
}

api::verified_client make_changes_client(const portable_fixture& fixture, protocol::table_changes_response response) {
   return make_client(fixture, fixture.rows_response, std::move(response));
}

protocol::contract_table_location location_of(const protocol::table_change_selector& selector) {
   return {
       .code = selector.code.value,
       .scope = selector.scope.value,
       .table = selector.table.value,
   };
}

void sort_leaves(std::vector<authenticated::proof_leaf>& leaves) {
   std::ranges::sort(leaves, {}, &authenticated::proof_leaf::key);
   for (auto index = std::size_t{1U}; index < leaves.size(); ++index) {
      if (leaves[index - 1U].key == leaves[index].key) {
         throw std::logic_error{"test authenticated leaves contain a duplicate key"};
      }
   }
}

struct rich_portable_fixture {
   std::string state_domain;
   protocol::chain_id chain;
   savanna::finality_trust genesis_trust;
   savanna::finality_trust checkpoint_trust;
   protocol::proof_blob genesis_finality;
   protocol::proof_blob checkpoint_finality;
   protocol::response_context context;
   protocol::state_anchor first_change_anchor;
   protocol::state_anchor target_anchor;
   protocol::contract_table_location rows_location;
   protocol::primary_value first_primary;
   protocol::primary_value second_primary;
   protocol::secondary_value first_secondary;
   protocol::secondary_value second_secondary;
   authenticated_tree_fixture state_tree;
   authenticated_tree_fixture first_changes_tree;
   authenticated_tree_fixture target_changes_tree;
   authenticated::root first_root;
   authenticated::root target_root;
   std::vector<protocol::table_change_selector> selectors;
   std::vector<protocol::table_mutation> first_block_mutations;
   std::vector<protocol::table_mutation> target_block_mutations;
};

rich_portable_fixture make_rich_portable_fixture() {
   const auto state_domain = std::string{"spine.chain.state"};
   const auto state_tree_domain = authenticated::canonical_tree_domain(state_domain, authenticated::proof_tree::state);
   const auto change_tree_domain =
       authenticated::canonical_tree_domain(state_domain, authenticated::proof_tree::changes);
   const auto code = protocol::account_name{"contract"};
   const auto scope = protocol::name{"scope"};
   const auto rows_table = protocol::table_name{"rows"};
   const auto other_table = protocol::table_name{"other"};
   const auto rows_location = protocol::contract_table_location{
       .code = code.value,
       .scope = scope.value,
       .table = rows_table.value,
   };
   const auto other_location = protocol::contract_table_location{
       .code = code.value,
       .scope = scope.value,
       .table = other_table.value,
   };
   const auto first_primary = protocol::primary_value{
       .payer = protocol::account_name{"payerone"},
       .row = {0x11U, 0x12U},
   };
   const auto second_primary = protocol::primary_value{
       .payer = protocol::account_name{"payertwo"},
       .row = {0x21U, 0x22U},
   };
   const auto other_primary = protocol::primary_value{
       .payer = protocol::account_name{"payerthree"},
       .row = {0x31U, 0x32U},
   };
   const auto first_secondary = protocol::secondary_value{
       .payer = protocol::account_name{"secpayera"},
       .primary = 7U,
   };
   const auto second_secondary = protocol::secondary_value{
       .payer = protocol::account_name{"secpayerb"},
       .primary = 9U,
   };
   const auto indexed_location = protocol::contract_table_location{
       .code = rows_location.code,
       .scope = rows_location.scope,
       .table = rows_location.table | 1U,
   };
   const auto first_secondary_key = api::encode_table_key(std::uint64_t{100U});
   const auto second_secondary_key = api::encode_table_key(std::uint64_t{200U});

   auto state_leaves = std::vector<authenticated::proof_leaf>{
       make_leaf(db_bytes(protocol::contract_table_key(rows_location)),
                 db_bytes(forge::raw::pack(protocol::table_value{
                     .payer = protocol::account_name{"tablepayer"},
                     .count = 2U,
                 }))),
       make_leaf(db_bytes(protocol::contract_primary_key(rows_location, protocol::contract_table_family::primary, 7U)),
                 db_bytes(forge::raw::pack(first_primary))),
       make_leaf(db_bytes(protocol::contract_primary_key(rows_location, protocol::contract_table_family::primary, 9U)),
                 db_bytes(forge::raw::pack(second_primary))),
       make_leaf(db_bytes(protocol::contract_table_key(other_location)),
                 db_bytes(forge::raw::pack(protocol::table_value{
                     .payer = protocol::account_name{"otherpayer"},
                     .count = 1U,
                 }))),
       make_leaf(
           db_bytes(protocol::contract_primary_key(other_location, protocol::contract_table_family::primary, 11U)),
           db_bytes(forge::raw::pack(other_primary))),
       make_leaf(
           db_bytes(protocol::contract_secondary_key(indexed_location, protocol::contract_table_family::secondary_u64,
                                                     db_bytes(first_secondary_key), 7U)),
           db_bytes(forge::raw::pack(first_secondary))),
       make_leaf(
           db_bytes(protocol::contract_secondary_key(indexed_location, protocol::contract_table_family::secondary_u64,
                                                     db_bytes(second_secondary_key), 9U)),
           db_bytes(forge::raw::pack(second_secondary))),
   };
   sort_leaves(state_leaves);
   auto state_tree = authenticated_tree_fixture{
       .domain = state_tree_domain,
       .tree = authenticated::proof_tree::state,
       .leaves = std::move(state_leaves),
   };

   auto selectors = std::vector<protocol::table_change_selector>{
       {.code = code, .scope = scope, .table = rows_table},
       {.code = code, .scope = scope, .table = other_table},
   };
   std::ranges::sort(selectors);
   const auto first_change_value = protocol::primary_value{
       .payer = protocol::account_name{"changeone"},
       .row = {0x41U},
   };
   const auto second_change_value = protocol::primary_value{
       .payer = protocol::account_name{"changetwo"},
       .row = {0x42U},
   };
   const auto target_change_value = protocol::primary_value{
       .payer = protocol::account_name{"changethree"},
       .row = {0x43U},
   };
   const auto first_mutation = authenticated::mutation{
       .key = db_bytes(
           protocol::contract_primary_key(location_of(selectors[0]), protocol::contract_table_family::primary, 21U)),
       .value = db_bytes(forge::raw::pack(first_change_value)),
   };
   const auto second_mutation = authenticated::mutation{
       .key = db_bytes(
           protocol::contract_primary_key(location_of(selectors[1]), protocol::contract_table_family::primary, 22U)),
       .value = db_bytes(forge::raw::pack(second_change_value)),
   };
   const auto target_mutation = authenticated::mutation{
       .key = db_bytes(
           protocol::contract_primary_key(location_of(selectors[0]), protocol::contract_table_family::primary, 31U)),
       .value = db_bytes(forge::raw::pack(target_change_value)),
   };
   auto first_change_leaves = std::vector{make_change_leaf(first_mutation), make_change_leaf(second_mutation)};
   auto target_change_leaves = std::vector{make_change_leaf(target_mutation)};
   sort_leaves(first_change_leaves);
   sort_leaves(target_change_leaves);
   auto first_changes_tree = authenticated_tree_fixture{
       .domain = change_tree_domain,
       .tree = authenticated::proof_tree::changes,
       .leaves = std::move(first_change_leaves),
   };
   auto target_changes_tree = authenticated_tree_fixture{
       .domain = change_tree_domain,
       .tree = authenticated::proof_tree::changes,
       .leaves = std::move(target_change_leaves),
   };
   const auto state_summary = state_tree.summary();
   const auto first_change_summary = first_changes_tree.summary();
   const auto target_change_summary = target_changes_tree.summary();
   auto first_root = authenticated::root{
       .state_root = state_summary.hash,
       .state_size = state_summary.size,
       .change_root = first_change_summary.hash,
       .change_count = first_change_summary.size,
   };
   auto target_root = authenticated::root{
       .state_root = state_summary.hash,
       .state_size = state_summary.size,
       .change_root = target_change_summary.hash,
       .change_count = target_change_summary.size,
   };

   auto producer = make_producer_key(7U);
   auto finalizer = make_finalizer(17U);
   auto configuration = make_genesis(producer, finalizer);
   const auto genesis_commitment = savanna::state_commitment{
       .state_root = make_digest(301U),
       .state_size = 1U,
       .change_root = make_digest(302U),
       .change_count = 1U,
   };
   const auto genesis = savanna::make_genesis_candidate(configuration, genesis_commitment);
   const auto chain = savanna::calculate_chain_id(configuration);
   const auto first_block = make_child(genesis.value, producer, savanna::state_commitment{});
   const auto first_receipt = make_digest(401U);
   const auto first = admit_with_receipt(genesis.value, first_block, first_receipt);
   const auto second_block =
       make_child(first, producer, savanna::state_commitment{}, make_current_qc(first, finalizer));
   const auto second_receipt = make_digest(402U);
   const auto second = admit_with_receipt(first, second_block, second_receipt);
   const auto first_change_commitment = savanna::state_commitment{
       .state_root = first_root.state_root,
       .state_size = first_root.state_size,
       .change_root = first_root.change_root,
       .change_count = first_root.change_count,
   };
   const auto third_block = make_child(second, producer, first_change_commitment, make_current_qc(second, finalizer));
   const auto third_receipt = make_digest(403U);
   const auto third = admit_with_receipt(second, third_block, third_receipt);
   const auto target_commitment = savanna::state_commitment{
       .state_root = target_root.state_root,
       .state_size = target_root.state_size,
       .change_root = target_root.change_root,
       .change_count = target_root.change_count,
   };
   const auto fourth_block = make_child(third, producer, target_commitment, make_current_qc(third, finalizer));
   const auto fourth_receipt = make_digest(404U);
   const auto fourth = admit_with_receipt(third, fourth_block, fourth_receipt);
   const auto fifth_block =
       make_child(fourth, producer, savanna::state_commitment{}, make_current_qc(fourth, finalizer));
   const auto fifth_receipt = make_digest(405U);
   const auto fifth = admit_with_receipt(fourth, fifth_block, fifth_receipt);
   const auto sixth_block = make_child(fifth, producer, savanna::state_commitment{}, make_current_qc(fifth, finalizer));
   const auto sixth_receipt = make_digest(406U);
   const auto sixth = admit_with_receipt(fifth, sixth_block, sixth_receipt);

   first_root.version = third.num;
   target_root.version = fourth.num;
   const auto first_change_anchor = make_anchor(chain, third);
   const auto target_anchor = make_anchor(chain, fourth);
   const auto all_records = std::array{
       make_record(first_block, first_receipt), make_record(second_block, second_receipt),
       make_record(third_block, third_receipt), make_record(fourth_block, fourth_receipt),
       make_record(fifth_block, fifth_receipt), make_record(sixth_block, sixth_receipt),
   };
   const auto checkpoint_records = std::array{
       make_record(third_block, third_receipt),
       make_record(fourth_block, fourth_receipt),
       make_record(fifth_block, fifth_receipt),
       make_record(sixth_block, sixth_receipt),
   };
   const auto checkpoint = savanna::checkpoint{
       .finalized = second.state.make_block_ref(),
       .state = second.state,
       .validation = savanna::advance_finalized(second.validation, second.num),
   };
   const auto genesis_witness = savanna::make_finality_witness(chain, genesis.value.id, all_records);
   const auto checkpoint_witness = savanna::make_finality_witness(chain, second.id, checkpoint_records);
   const auto first_block_mutations = std::vector<protocol::table_mutation>{
       {.table = selectors[0],
        .primary = 21U,
        .row = protocol::table_row{.value = first_change_value.row, .payer = first_change_value.payer}},
       {.table = selectors[1],
        .primary = 22U,
        .row = protocol::table_row{.value = second_change_value.row, .payer = second_change_value.payer}},
   };
   const auto target_block_mutations = std::vector<protocol::table_mutation>{
       {.table = selectors[0],
        .primary = 31U,
        .row = protocol::table_row{.value = target_change_value.row, .payer = target_change_value.payer}},
   };

   return {
       .state_domain = state_domain,
       .chain = chain,
       .genesis_trust =
           savanna::finality_genesis_bootstrap{
               .configuration = configuration,
               .commitment = genesis_commitment,
           },
       .checkpoint_trust =
           savanna::finality_checkpoint_bootstrap{
               .chain = chain,
               .value = checkpoint,
           },
       .genesis_finality = savanna::encode_finality_witness(genesis_witness),
       .checkpoint_finality = savanna::encode_finality_witness(checkpoint_witness),
       .context =
           protocol::response_context{
               .chain = chain,
               .head = sixth.id,
               .finalized = target_anchor.block,
               .anchor = target_anchor,
           },
       .first_change_anchor = first_change_anchor,
       .target_anchor = target_anchor,
       .rows_location = rows_location,
       .first_primary = first_primary,
       .second_primary = second_primary,
       .first_secondary = first_secondary,
       .second_secondary = second_secondary,
       .state_tree = std::move(state_tree),
       .first_changes_tree = std::move(first_changes_tree),
       .target_changes_tree = std::move(target_changes_tree),
       .first_root = first_root,
       .target_root = target_root,
       .selectors = selectors,
       .first_block_mutations = first_block_mutations,
       .target_block_mutations = target_block_mutations,
   };
}

authenticated::range_request rows_proof_request(const rich_portable_fixture& fixture,
                                                const protocol::table_rows_request& request) {
   const auto primary = request.index.kind == protocol::table_index_kind::primary;
   const auto family =
       primary ? protocol::contract_table_family::primary : protocol::contract_table_family::secondary_u64;
   const auto location = protocol::contract_table_location{
       .code = fixture.rows_location.code,
       .scope = fixture.rows_location.scope,
       .table = primary ? fixture.rows_location.table : fixture.rows_location.table | request.index.position,
   };
   const auto prefix = db_bytes(protocol::contract_index_prefix(location, family));
   auto lower = prefix;
   auto upper = prefix_upper_bound(prefix);
   if (request.cursor) {
      if (request.reverse) {
         upper = db_bytes(*request.cursor);
      } else {
         lower = db_bytes(*request.cursor);
      }
   }
   return {
       .lower = std::move(lower),
       .upper = std::move(upper),
       .limit = request.limit,
       .include_values = true,
       .reverse = request.reverse,
   };
}

protocol::table_rows_response make_rich_rows_response(const rich_portable_fixture& fixture,
                                                      const protocol::table_rows_request& request,
                                                      const protocol::proof_blob& finality) {
   const auto proof_request = rows_proof_request(fixture, request);
   const auto range_proof = fixture.state_tree.range(fixture.target_root, proof_request);
   const auto verified = authenticated::verify_range(fixture.state_domain, fixture.target_root, proof_request,
                                                     authenticated::proof_tree::state, range_proof);
   auto response = protocol::table_rows_response{};
   response.context = fixture.context;
   response.audit = protocol::audit_bundle{
       .finality = finality,
       .state = {proof_blob("forge.db.authenticated.range", range_proof)},
   };
   response.next = verified.next_key ? std::optional{wire_bytes(*verified.next_key)} : std::nullopt;
   for (const auto& item : verified.items) {
      BOOST_REQUIRE(item.value.has_value());
      if (request.index.kind == protocol::table_index_kind::primary) {
         const auto stored = forge::raw::unpack_exact<protocol::primary_value>(wire_bytes(*item.value));
         response.rows.push_back({.value = stored.row, .payer = stored.payer});
         continue;
      }
      const auto secondary = forge::raw::unpack_exact<protocol::secondary_value>(wire_bytes(*item.value));
      const auto primary_key = protocol::contract_primary_key(
          fixture.rows_location, protocol::contract_table_family::primary, secondary.primary);
      const auto point = fixture.state_tree.point(fixture.target_root, db_bytes(primary_key));
      response.audit->state.push_back(proof_blob("forge.db.authenticated.point", point));
      BOOST_REQUIRE(point.terminal.has_value());
      BOOST_REQUIRE(point.terminal->value.has_value());
      const auto stored = forge::raw::unpack_exact<protocol::primary_value>(wire_bytes(*point.terminal->value));
      response.rows.push_back({.value = stored.row, .payer = secondary.payer});
   }
   return response;
}

authenticated::range_request change_proof_request(const protocol::table_change_selector& selector,
                                                  std::uint32_t limit) {
   const auto prefix =
       db_bytes(protocol::contract_index_prefix(location_of(selector), protocol::contract_table_family::primary));
   return {
       .lower = prefix,
       .upper = prefix_upper_bound(prefix),
       .limit = limit,
       .include_values = true,
   };
}

std::vector<protocol::table_mutation> select_mutations(const std::vector<protocol::table_mutation>& mutations,
                                                       const protocol::table_change_selector& selector) {
   auto result = std::vector<protocol::table_mutation>{};
   std::ranges::copy_if(mutations, std::back_inserter(result),
                        [&](const auto& mutation) { return mutation.table == selector; });
   return result;
}

using change_cursor_wire =
    std::tuple<std::uint8_t, std::uint8_t, protocol::chain_id, protocol::block_id, protocol::digest, std::uint32_t,
               std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t, std::optional<protocol::bytes>, bool>;

protocol::digest change_request_fingerprint(const protocol::table_changes_request& request) {
   const auto packed = forge::raw::pack(std::tuple{request.from_block, request.to_block, request.tables, request.limit,
                                                   request.finality_from, request.audit});
   return forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{packed});
}

protocol::bytes change_cursor(const rich_portable_fixture& fixture, const protocol::table_changes_request& request,
                              std::uint32_t block, std::uint32_t selector) {
   return forge::raw::pack(change_cursor_wire{
       2U,
       1U,
       fixture.chain,
       fixture.target_anchor.block,
       change_request_fingerprint(request),
       request.from_block,
       request.to_block,
       block,
       selector,
       0U,
       std::nullopt,
       false,
   });
}

protocol::table_changes_response make_full_changes_response(const rich_portable_fixture& fixture,
                                                            const protocol::table_changes_request& request) {
   auto response = protocol::table_changes_response{};
   response.context = fixture.context;
   response.audit = protocol::audit_bundle{
       .finality = fixture.genesis_finality,
       .ancestry = fixture.genesis_finality,
   };
   auto remaining = request.limit;
   for (const auto& selector : request.tables) {
      const auto proof_request = change_proof_request(selector, remaining);
      const auto proof = fixture.first_changes_tree.range(fixture.first_root, proof_request);
      response.audit->state.push_back(proof_blob("forge.db.authenticated.changes", proof));
      remaining -= static_cast<std::uint32_t>(select_mutations(fixture.first_block_mutations, selector).size());
   }
   for (const auto& selector : request.tables) {
      const auto proof_request = change_proof_request(selector, remaining);
      const auto proof = fixture.target_changes_tree.range(fixture.target_root, proof_request);
      response.audit->state.push_back(proof_blob("forge.db.authenticated.changes", proof));
      remaining -= static_cast<std::uint32_t>(select_mutations(fixture.target_block_mutations, selector).size());
   }
   response.blocks = {
       {.anchor = fixture.first_change_anchor, .mutations = fixture.first_block_mutations},
       {.anchor = fixture.target_anchor, .mutations = fixture.target_block_mutations},
   };
   return response;
}

protocol::table_changes_response make_changes_page(const rich_portable_fixture& fixture,
                                                   const protocol::table_changes_request& request) {
   const auto first_cursor = change_cursor(fixture, request, fixture.first_change_anchor.block_num, 1U);
   const auto second_cursor = change_cursor(fixture, request, fixture.target_anchor.block_num, 0U);
   const auto third_cursor = change_cursor(fixture, request, fixture.target_anchor.block_num, 1U);
   auto page = std::size_t{};
   if (request.cursor) {
      if (*request.cursor == first_cursor) {
         page = 1U;
      } else if (*request.cursor == second_cursor) {
         page = 2U;
      } else if (*request.cursor == third_cursor) {
         page = 3U;
      } else {
         throw std::logic_error{"test service received an unknown table changes cursor"};
      }
   }
   const auto first_block = page < 2U;
   const auto selector_index = static_cast<std::size_t>(page % 2U);
   const auto& selector = request.tables[selector_index];
   const auto& tree = first_block ? fixture.first_changes_tree : fixture.target_changes_tree;
   const auto& root = first_block ? fixture.first_root : fixture.target_root;
   const auto& anchor = first_block ? fixture.first_change_anchor : fixture.target_anchor;
   const auto& mutations = first_block ? fixture.first_block_mutations : fixture.target_block_mutations;
   const auto proof_request = change_proof_request(selector, request.limit);
   const auto proof = tree.range(root, proof_request);

   auto response = protocol::table_changes_response{};
   response.context = fixture.context;
   response.audit = protocol::audit_bundle{
       .finality = fixture.genesis_finality,
       .state = {proof_blob("forge.db.authenticated.changes", proof)},
   };
   if (first_block) {
      response.audit->ancestry = fixture.genesis_finality;
   }
   response.blocks = {{.anchor = anchor, .mutations = select_mutations(mutations, selector)}};
   if (page == 0U) {
      response.next = first_cursor;
   } else if (page == 1U) {
      response.next = second_cursor;
   } else if (page == 2U) {
      response.next = third_cursor;
   }
   return response;
}

protocol::table_changes_response make_empty_changes_response(const rich_portable_fixture& fixture) {
   auto response = protocol::table_changes_response{};
   response.context = fixture.context;
   response.audit = protocol::audit_bundle{.finality = fixture.genesis_finality};
   return response;
}

struct rich_client_harness {
   api::verified_client client;
   std::shared_ptr<fixture_state_service> service;
   std::shared_ptr<api::savanna_finality_verifier> finality;
};

rich_client_harness make_rich_client(const rich_portable_fixture& fixture, savanna::finality_trust trust,
                                     std::vector<savanna::finality_trust> additional_trusts,
                                     protocol::proof_blob finality, fixture_state_service::rows_handler rows,
                                     fixture_state_service::changes_handler changes) {
   auto services = forge::api::core::registry{};
   auto service = std::make_shared<fixture_state_service>(std::move(rows), std::move(changes), fixture.context,
                                                          std::move(finality));
   services.install<api::state>(service);
   auto raw = api::raw_client{api::service_handles{
       .state_queries = services.get<api::state>(api::state::ref()),
   }};
   auto savanna_finality =
       api::make_savanna_finality_verifier_with_trusts(std::move(trust), std::move(additional_trusts));
   return {
       .client = api::make_verified_client(std::move(raw),
                                           {
                                               .chain = fixture.chain,
                                               .state_domain = fixture.state_domain,
                                               .finality = savanna_finality,
                                               .projections = api::make_contract_table_projection_verifier(),
                                           }),
       .service = std::move(service),
       .finality = std::move(savanna_finality),
   };
}

void flip_first_payload_byte(protocol::proof_blob& proof) {
   BOOST_REQUIRE(!proof.payload.empty());
   proof.payload.front() ^= 1U;
}

} // namespace

BOOST_AUTO_TEST_CASE(portable_verified_client_executes_rows_and_changes_with_real_savanna_finality) {
   const auto fixture = make_portable_fixture();
   const auto& selector = fixture.changes_request.tables.front();
   const auto changes_prefix = protocol::contract_index_prefix(
       {
           .code = selector.code.value,
           .scope = selector.scope.value,
           .table = selector.table.value,
       },
       protocol::contract_table_family::primary);
   const auto expected_changes_request = authenticated::range_request{
       .lower = db_bytes(changes_prefix),
       .upper = prefix_upper_bound(db_bytes(changes_prefix)),
       .limit = fixture.changes_request.limit,
       .include_values = true,
   };
   BOOST_CHECK(fixture.changes_proof.tree == authenticated::proof_tree::changes);
   BOOST_CHECK(fixture.changes_proof.request == expected_changes_request);

   auto client = make_client(fixture, fixture.rows_response, fixture.changes_response);

   const auto rows = run(client.get_table_rows(fixture.rows_request));
   BOOST_CHECK(rows == fixture.rows_response);
   const auto changes = run(client.get_table_changes(fixture.changes_request));
   BOOST_CHECK(changes == fixture.changes_response);
}

BOOST_AUTO_TEST_CASE(portable_savanna_multi_trust_prefers_max_and_selects_each_witness_bootstrap) {
   const auto fixture = make_rich_portable_fixture();
   const auto genesis_anchor = savanna::trust_anchor(fixture.genesis_trust);
   const auto checkpoint_anchor = savanna::trust_anchor(fixture.checkpoint_trust);
   const auto& checkpoint = std::get<savanna::finality_checkpoint_bootstrap>(fixture.checkpoint_trust);
   BOOST_REQUIRE(checkpoint.value.finalized.num > 1U);

   const auto verify_order = [&](std::vector<savanna::finality_trust> trusted) {
      const auto verifier = api::make_savanna_finality_verifier_with_trusts(trusted.front(), {trusted.back()});
      BOOST_REQUIRE(verifier->preferred_trust_anchor().has_value());
      BOOST_CHECK(*verifier->preferred_trust_anchor() == checkpoint_anchor.block);

      const auto intermediate = std::array{fixture.first_change_anchor};
      for (const auto& proof : {fixture.genesis_finality, fixture.checkpoint_finality}) {
         const auto state = verifier->replay_state(fixture.target_anchor, proof);
         BOOST_CHECK(state.id == fixture.target_anchor.block);
         BOOST_CHECK_NO_THROW(verifier->verify(fixture.target_anchor, proof));
         BOOST_CHECK_NO_THROW(verifier->verify_ancestry(fixture.target_anchor, intermediate, proof));
      }
   };

   verify_order({fixture.genesis_trust, fixture.checkpoint_trust});
   verify_order({fixture.checkpoint_trust, fixture.genesis_trust});

   const auto legacy = api::make_savanna_finality_verifier(fixture.genesis_trust);
   BOOST_REQUIRE(legacy->preferred_trust_anchor().has_value());
   BOOST_CHECK(*legacy->preferred_trust_anchor() == genesis_anchor.block);
}

BOOST_AUTO_TEST_CASE(portable_factory_accepts_multi_trust_and_requests_preferred_checkpoint) {
   const auto fixture = make_rich_portable_fixture();
   const auto checkpoint_anchor = savanna::trust_anchor(fixture.checkpoint_trust);
   for (const auto& finality : {fixture.genesis_finality, fixture.checkpoint_finality}) {
      auto harness = make_rich_client(
          fixture, fixture.genesis_trust, {fixture.checkpoint_trust}, finality,
          [&](const auto& request) { return make_rich_rows_response(fixture, request, finality); },
          [&](const auto&) { return make_empty_changes_response(fixture); });
      const auto rows = run(harness.client.get_table_rows(protocol::table_rows_request{
          .code = protocol::account_name{fixture.rows_location.code},
          .scope = protocol::name{fixture.rows_location.scope},
          .table = protocol::table_name{fixture.rows_location.table},
          .index = {.kind = protocol::table_index_kind::primary},
          .limit = 2U,
          .audit = protocol::audit_mode::required,
      }));
      BOOST_REQUIRE_EQUAL(rows.rows.size(), 2U);
      BOOST_REQUIRE_EQUAL(harness.service->rows_requests().size(), 1U);
      BOOST_REQUIRE(harness.service->rows_requests().front().finality_from.has_value());
      BOOST_CHECK(*harness.service->rows_requests().front().finality_from == checkpoint_anchor.block);
   }
}

BOOST_AUTO_TEST_CASE(portable_savanna_pure_advance_derives_checkpoint_and_rejects_invalid_inputs) {
   const auto fixture = make_rolling_witness_chain(5U);
   const auto proof = fixture.proof(1U, 2U);
   const auto witness = savanna::decode_finality_witness(proof);
   const auto trust_before = forge::raw::pack(fixture.genesis_trust);
   const auto expected_validation = savanna::advance_finalized(fixture.candidate(2U).validation, 2U);

   const auto advanced =
       savanna::advance_finality_trust_with_replay(fixture.genesis_trust, witness, fixture.anchor(2U));
   const auto checkpoint = savanna::advance_finality_trust(fixture.genesis_trust, proof, fixture.anchor(2U));
   BOOST_CHECK(advanced.checkpoint.chain == fixture.chain);
   BOOST_CHECK(advanced.checkpoint.value.finalized == fixture.candidate(2U).state.make_block_ref());
   BOOST_CHECK(forge::raw::pack(advanced.checkpoint.value.state) == forge::raw::pack(fixture.candidate(2U).state));
   BOOST_CHECK(forge::raw::pack(advanced.checkpoint.value.validation) == forge::raw::pack(expected_validation));
   BOOST_CHECK(forge::raw::pack(advanced.checkpoint) == forge::raw::pack(checkpoint));
   BOOST_CHECK(advanced.replay.anchors.front() == fixture.anchor(1U));
   BOOST_CHECK(std::ranges::find(advanced.replay.anchors, fixture.anchor(2U)) != advanced.replay.anchors.end());
   BOOST_CHECK(advanced.replay.finalized_block_num >= 2U);
   BOOST_CHECK(advanced.replay.validated_block_num >= 2U);
   BOOST_CHECK(forge::raw::pack(fixture.genesis_trust) == trust_before);

   auto wrong_anchor = fixture.anchor(2U);
   wrong_anchor.state_root._hash[0] ^= 1U;
   BOOST_CHECK_THROW(static_cast<void>(savanna::advance_finality_trust(fixture.genesis_trust, proof, wrong_anchor)),
                     savanna::exceptions::finality_anchor_mismatch);

   auto wrong_chain_witness = witness;
   wrong_chain_witness.chain._hash[0] ^= 1U;
   BOOST_CHECK_THROW(
       static_cast<void>(savanna::advance_finality_trust(
           fixture.genesis_trust, savanna::encode_finality_witness(wrong_chain_witness), fixture.anchor(2U))),
       savanna::exceptions::finality_witness_wrong_chain);

   auto bad_signature = witness;
   const auto outsider = make_producer_key(1U);
   bad_signature.records.front().header.producer_signature =
       sign(outsider, protocol::calculate_block_id(bad_signature.records.front().header));
   BOOST_CHECK_THROW(static_cast<void>(savanna::advance_finality_trust(
                         fixture.genesis_trust, savanna::encode_finality_witness(bad_signature), fixture.anchor(2U))),
                     savanna::exceptions::invalid_header);
}

BOOST_AUTO_TEST_CASE(portable_savanna_replay_reports_skipped_slot_policy_opportunities_and_rejects_wrong_producer) {
   const auto eosio = make_producer_key(19U);
   const auto alice = make_producer_key(31U);
   const auto finalizer = make_finalizer(29U);
   const auto configuration = make_genesis(eosio, finalizer);
   const auto commitment = savanna::state_commitment{
       .state_root = make_digest(11U),
       .state_size = 1U,
       .change_root = make_digest(12U),
       .change_count = 1U,
   };
   const auto genesis = savanna::make_genesis_candidate(configuration, commitment);
   const auto chain = savanna::calculate_chain_id(configuration);
   const auto policy = savanna::proposer_policy_diff{
       .version = 1U,
       .proposal_time = savanna::block_timestamp{13U},
       .producers =
           {
               .remove_indexes = {0U},
               .insert_indexes = {{
                   0U,
                   {
                       .producer_name = protocol::account_name{"alice"},
                       .authority =
                           protocol::block_signing_authority_v0{
                               .threshold = 1U,
                               .keys = {{.key = public_key(alice), .weight = 1U}},
                           },
                   },
               }},
           },
   };

   auto first_block = make_child(genesis.value, eosio, savanna::state_commitment{});
   first_block.header_extensions.front().second = forge::raw::pack(
       savanna::finality_extension{.claim = genesis.value.state.finality.latest_qc_claim(), .proposers = policy});
   first_block.producer_signature = sign(eosio, protocol::calculate_block_id(first_block));
   const auto first_receipt = make_digest(13U);
   const auto first = admit_with_receipt(genesis.value, first_block, first_receipt);

   auto second_block = make_child(first, eosio, savanna::state_commitment{}, make_current_qc(first, finalizer));
   second_block.timestamp = savanna::block_timestamp{24U};
   second_block.producer_signature = sign(eosio, protocol::calculate_block_id(second_block));
   const auto second_receipt = make_digest(14U);
   const auto second = admit_with_receipt(first, second_block, second_receipt);

   const auto third_timestamp = savanna::block_timestamp{36U};
   BOOST_CHECK(savanna::scheduled_producer(second.state, third_timestamp).producer_name ==
               protocol::account_name{"eosio"});
   auto third_block = make_child(second, eosio, savanna::state_commitment{}, make_current_qc(second, finalizer));
   third_block.timestamp = third_timestamp;
   third_block.producer_signature = sign(eosio, protocol::calculate_block_id(third_block));
   const auto third_receipt = make_digest(15U);
   const auto third = admit_with_receipt(second, third_block, third_receipt);

   const auto fourth_timestamp = savanna::block_timestamp{48U};
   BOOST_CHECK(savanna::scheduled_producer(third.state, fourth_timestamp).producer_name ==
               protocol::account_name{"alice"});
   auto fourth_block = make_child(third, alice, savanna::state_commitment{}, make_current_qc(third, finalizer));
   fourth_block.timestamp = fourth_timestamp;
   fourth_block.producer = protocol::account_name{"alice"};
   fourth_block.producer_signature = sign(alice, protocol::calculate_block_id(fourth_block));
   const auto fourth_receipt = make_digest(16U);
   static_cast<void>(admit_with_receipt(third, fourth_block, fourth_receipt));

   const auto witness = savanna::make_finality_witness(
       chain, genesis.value.id,
       std::array{make_record(first_block, first_receipt), make_record(second_block, second_receipt),
                  make_record(third_block, third_receipt), make_record(fourth_block, fourth_receipt)});
   const auto trust = savanna::finality_genesis_bootstrap{.configuration = configuration, .commitment = commitment};
   const auto replay = savanna::replay_finality_witness(trust, witness);
   BOOST_REQUIRE_EQUAL(replay.producer_opportunities.size(), fourth_timestamp.slot - genesis.value.timestamp.slot);
   for (auto position = std::size_t{1U}; position < replay.producer_opportunities.size(); ++position) {
      BOOST_TEST(replay.producer_opportunities[position].timestamp.slot ==
                 replay.producer_opportunities[position - 1U].timestamp.slot + 1U);
   }
   BOOST_CHECK(replay.producer_opportunities[0].expected_producer == protocol::account_name{"eosio"});
   BOOST_REQUIRE(replay.producer_opportunities[0].produced_block.has_value());
   BOOST_REQUIRE(replay.producer_opportunities[0].produced_block_num.has_value());
   BOOST_TEST(*replay.producer_opportunities[0].produced_block == first.id);
   BOOST_TEST(*replay.producer_opportunities[0].produced_block_num == first.num);
   BOOST_TEST(replay.producer_opportunities[1].timestamp.slot == first_block.timestamp.slot + 1U);
   BOOST_CHECK(replay.producer_opportunities[1].expected_producer == protocol::account_name{"eosio"});
   BOOST_TEST(!replay.producer_opportunities[1].produced_block.has_value());
   BOOST_TEST(!replay.producer_opportunities[1].produced_block_num.has_value());
   BOOST_TEST(replay.producer_opportunities[11].timestamp.slot == second_block.timestamp.slot);
   BOOST_REQUIRE(replay.producer_opportunities[11].produced_block.has_value());
   BOOST_TEST(*replay.producer_opportunities[11].produced_block == second.id);
   BOOST_TEST(replay.producer_opportunities[12].timestamp.slot == second_block.timestamp.slot + 1U);
   BOOST_CHECK(replay.producer_opportunities[12].expected_producer == protocol::account_name{"eosio"});
   BOOST_TEST(!replay.producer_opportunities[12].produced_block.has_value());
   BOOST_TEST(replay.producer_opportunities[23].timestamp.slot == third_block.timestamp.slot);
   BOOST_REQUIRE(replay.producer_opportunities[23].produced_block.has_value());
   BOOST_TEST(*replay.producer_opportunities[23].produced_block == third.id);
   BOOST_TEST(replay.producer_opportunities[24].timestamp.slot == third_block.timestamp.slot + 1U);
   BOOST_CHECK(replay.producer_opportunities[24].expected_producer == protocol::account_name{"eosio"});
   BOOST_TEST(!replay.producer_opportunities[24].produced_block.has_value());
   BOOST_TEST(replay.producer_opportunities.back().timestamp.slot == fourth_timestamp.slot);
   BOOST_CHECK(replay.producer_opportunities.back().expected_producer == protocol::account_name{"alice"});
   BOOST_REQUIRE(replay.producer_opportunities.back().produced_block.has_value());
   BOOST_TEST(*replay.producer_opportunities.back().produced_block == protocol::calculate_block_id(fourth_block));
   BOOST_TEST(std::count_if(replay.producer_opportunities.begin(), replay.producer_opportunities.end(),
                            [](const auto& value) { return value.produced_block.has_value(); }) == 4U);
   BOOST_CHECK_THROW(static_cast<void>(savanna::replay_finality_witness(
                         trust, witness, savanna::finality_witness_limits{.max_producer_slots = 35U})),
                     savanna::exceptions::finality_witness_limit_exceeded);
   BOOST_TEST(first_block.schedule_version == savanna::proper_savanna_schedule_version);
   BOOST_TEST(policy.version != first_block.schedule_version);

   auto malicious = witness;
   malicious.records.back().header.producer = protocol::account_name{"eosio"};
   malicious.records.back().header.producer_signature =
       sign(eosio, protocol::calculate_block_id(malicious.records.back().header));
   BOOST_CHECK_THROW(static_cast<void>(savanna::replay_finality_witness(trust, malicious)),
                     savanna::exceptions::invalid_header);
}

BOOST_AUTO_TEST_CASE(portable_savanna_verifier_ratchets_without_rolling_back_or_promoting_tampered_proofs) {
   const auto fixture = make_rich_portable_fixture();
   const auto verifier =
       api::make_savanna_finality_verifier_with_trusts(fixture.genesis_trust, {fixture.checkpoint_trust});
   const auto checkpoint_anchor = savanna::trust_anchor(fixture.checkpoint_trust);

   BOOST_REQUIRE(verifier->preferred_trust_anchor().has_value());
   BOOST_CHECK(*verifier->preferred_trust_anchor() == checkpoint_anchor.block);
   BOOST_CHECK_NO_THROW(verifier->verify(fixture.target_anchor, fixture.genesis_finality));
   BOOST_REQUIRE(verifier->preferred_trust_anchor().has_value());
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.target_anchor.block);

   const auto state = verifier->replay_state(fixture.target_anchor, fixture.genesis_finality);
   BOOST_CHECK(state.id == fixture.target_anchor.block);
   const auto verified_replay = verifier->verified_replay(fixture.target_anchor, fixture.genesis_finality);
   BOOST_CHECK(std::ranges::find(verified_replay.anchors, fixture.target_anchor) != verified_replay.anchors.end());
   BOOST_CHECK(!verified_replay.producer_opportunities.empty());
   const auto earlier_replay = verifier->verified_replay(fixture.first_change_anchor, fixture.genesis_finality);
   BOOST_REQUIRE(!earlier_replay.anchors.empty());
   BOOST_CHECK(earlier_replay.anchors.back() == fixture.first_change_anchor);
   BOOST_REQUIRE(!earlier_replay.producer_opportunities.empty());
   BOOST_REQUIRE(earlier_replay.producer_opportunities.back().produced_block.has_value());
   BOOST_CHECK(*earlier_replay.producer_opportunities.back().produced_block == fixture.first_change_anchor.block);
   BOOST_TEST(earlier_replay.finalized_block_num == fixture.first_change_anchor.block_num);
   BOOST_TEST(earlier_replay.validated_block_num == fixture.first_change_anchor.block_num);
   BOOST_CHECK_NO_THROW(verifier->verify(fixture.target_anchor, fixture.genesis_finality));
   const auto promoted_trust = forge::raw::pack(verifier->preferred_trust());

   auto late_lower_witness = savanna::decode_finality_witness(fixture.checkpoint_finality);
   BOOST_REQUIRE(!late_lower_witness.records.empty());
   late_lower_witness.records.pop_back();
   const auto late_lower = savanna::encode_finality_witness(late_lower_witness);
   const auto lower_only = api::make_savanna_finality_verifier(fixture.checkpoint_trust);
   BOOST_CHECK_NO_THROW(lower_only->verify(fixture.first_change_anchor, late_lower));
   BOOST_CHECK_NO_THROW(verifier->verify(fixture.first_change_anchor, late_lower));
   BOOST_CHECK(verifier->replay_state(fixture.first_change_anchor, late_lower).id == fixture.first_change_anchor.block);
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.target_anchor.block);
   BOOST_CHECK(forge::raw::pack(verifier->preferred_trust()) == promoted_trust);

   auto tampered_witness = savanna::decode_finality_witness(fixture.genesis_finality);
   BOOST_REQUIRE(!tampered_witness.records.empty());
   const auto outsider = make_producer_key(1U);
   tampered_witness.records.front().header.producer_signature =
       sign(outsider, protocol::calculate_block_id(tampered_witness.records.front().header));
   const auto tampered = savanna::encode_finality_witness(tampered_witness);
   BOOST_CHECK_THROW(verifier->verify(fixture.target_anchor, tampered), api::exceptions::invalid_finality);
   BOOST_CHECK_THROW(static_cast<void>(verifier->verified_replay(fixture.target_anchor, tampered)),
                     api::exceptions::invalid_finality);
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.target_anchor.block);
   BOOST_CHECK(forge::raw::pack(verifier->preferred_trust()) == promoted_trust);
}

BOOST_AUTO_TEST_CASE(portable_savanna_rolling_chain_respects_limits_window_and_replay_cache) {
   const auto fixture = make_rolling_witness_chain();
   const auto limits = savanna::finality_witness_limits{
       .max_blocks = 3U,
       .max_bytes = savanna::finality_witness_hard_max_bytes,
   };
   const auto verifier = api::make_savanna_finality_verifier(fixture.genesis_trust, limits);

   const auto oversized = fixture.proof(1U, 3U);
   BOOST_REQUIRE_EQUAL(savanna::decode_finality_witness(oversized).records.size(), 4U);
   BOOST_CHECK_THROW(verifier->verify(fixture.anchor(3U), oversized), api::exceptions::resource_exhausted);
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(1U).id);

   auto proofs = std::vector<std::optional<protocol::proof_blob>>(20U);
   for (auto finalized = protocol::block_num{2U}; finalized <= 18U; ++finalized) {
      auto proof = fixture.proof(finalized - 1U, finalized);
      BOOST_REQUIRE_EQUAL(savanna::decode_finality_witness(proof).records.size(), limits.max_blocks);
      BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(finalized), proof));
      BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(finalized).id);
      proofs[finalized] = std::move(proof);
   }

   BOOST_REQUIRE(proofs[3U].has_value());
   BOOST_CHECK(verifier->replay_state(fixture.anchor(3U), *proofs[3U]).id == fixture.candidate(3U).id);

   auto newest = fixture.proof(18U, 19U);
   BOOST_REQUIRE_EQUAL(savanna::decode_finality_witness(newest).records.size(), limits.max_blocks);
   BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(19U), newest));
   proofs[19U] = std::move(newest);
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(19U).id);

   const auto preferred_before_selection = forge::raw::pack(verifier->preferred_trust());
   BOOST_CHECK(!verifier->trust_anchor_at_or_before(0U).has_value());
   BOOST_CHECK(verifier->trust_anchor_at_or_before(1U) == fixture.candidate(1U).id);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(3U) == fixture.candidate(1U).id);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(4U) == fixture.candidate(4U).id);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(18U) == fixture.candidate(18U).id);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(19U) == fixture.candidate(19U).id);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(100U) == fixture.candidate(19U).id);
   BOOST_CHECK(forge::raw::pack(verifier->preferred_trust()) == preferred_before_selection);

   for (auto retained = protocol::block_num{4U}; retained <= 19U; ++retained) {
      BOOST_CHECK(verifier->trust_anchor_at_or_before(retained) == fixture.candidate(retained).id);
   }

   BOOST_CHECK_THROW(static_cast<void>(verifier->replay_state(fixture.anchor(3U), *proofs[3U])),
                     api::exceptions::trust_required);
   BOOST_CHECK_THROW(verifier->verify(fixture.anchor(3U), fixture.bootstrap_proof(3U)),
                     api::exceptions::trust_required);

   BOOST_REQUIRE(proofs[2U].has_value());
   BOOST_CHECK(verifier->replay_state(fixture.anchor(2U), *proofs[2U]).id == fixture.candidate(2U).id);

   BOOST_CHECK_THROW(verifier->verify(fixture.anchor(2U), *proofs[2U]), api::exceptions::trust_required);
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(19U).id);

   const auto preferred = forge::raw::pack(verifier->preferred_trust());
   for (auto retained = protocol::block_num{4U}; retained <= 19U; ++retained) {
      BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(retained), fixture.bootstrap_proof(retained)));
   }
   BOOST_CHECK(forge::raw::pack(verifier->preferred_trust()) == preferred);
}

BOOST_AUTO_TEST_CASE(portable_savanna_historical_witness_from_a_retained_checkpoint_does_not_rollback) {
   const auto fixture = make_rolling_witness_chain(36U);
   const auto limits = savanna::finality_witness_limits{
       .max_blocks = 4U,
       .max_bytes = savanna::finality_witness_hard_max_bytes,
   };
   const auto verifier = api::make_savanna_finality_verifier(fixture.genesis_trust, limits);

   BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(2U), fixture.proof(1U, 2U)));
   for (auto finalized = protocol::block_num{4U}; finalized <= 18U; finalized += 2U) {
      const auto proof = fixture.proof(finalized - 2U, finalized);
      BOOST_REQUIRE_EQUAL(savanna::decode_finality_witness(proof).records.size(), limits.max_blocks);
      BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(finalized), proof));
   }
   BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(19U), fixture.proof(18U, 19U)));
   const auto preferred = forge::raw::pack(verifier->preferred_trust());
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(19U).id);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(17U) == fixture.candidate(16U).id);

   const auto historical = fixture.proof(16U, 17U);
   BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(17U), historical));
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(19U).id);
   BOOST_CHECK(forge::raw::pack(verifier->preferred_trust()) == preferred);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(17U) == fixture.candidate(16U).id);
   BOOST_CHECK(verifier->replay_state(fixture.anchor(17U), historical).id == fixture.candidate(17U).id);

   const auto conflicting = fixture.conflicting_proof(16U, 17U);
   BOOST_CHECK(conflicting.target.block != fixture.candidate(17U).id);
   BOOST_CHECK_THROW(verifier->verify(conflicting.target, conflicting.proof), api::exceptions::invalid_finality);
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(19U).id);
   BOOST_CHECK(forge::raw::pack(verifier->preferred_trust()) == preferred);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(17U) == fixture.candidate(16U).id);

   for (auto finalized = protocol::block_num{20U}; finalized <= 33U; ++finalized) {
      BOOST_CHECK_NO_THROW(verifier->verify(fixture.anchor(finalized), fixture.proof(finalized - 1U, finalized)));
   }
   BOOST_CHECK(*verifier->preferred_trust_anchor() == fixture.candidate(33U).id);
   BOOST_CHECK(verifier->trust_anchor_at_or_before(16U) == fixture.candidate(1U).id);
   BOOST_CHECK(verifier->replay_state(fixture.anchor(17U), historical).id == fixture.candidate(17U).id);
   BOOST_CHECK_THROW(static_cast<void>(verifier->replay_state(conflicting.target, conflicting.proof)),
                     api::exceptions::trust_required);
}

BOOST_AUTO_TEST_CASE(portable_savanna_concurrent_completion_order_is_monotonic) {
   const auto fixture = make_rolling_witness_chain(6U);
   const auto checkpoint = savanna::finality_trust{
       savanna::advance_finality_trust(fixture.genesis_trust, fixture.proof(1U, 2U), fixture.anchor(2U))};
   const auto lower_proof = fixture.proof(2U, 3U);
   const auto higher_proof = fixture.proof(2U, 4U);

   struct completion_result {
      std::exception_ptr lower_error;
      std::exception_ptr higher_error;
      std::optional<protocol::block_id> after_first;
      protocol::bytes after_first_trust;
      std::optional<protocol::block_id> final;
      protocol::bytes final_trust;
   };

   enum class release_order {
      higher_first,
      lower_first,
      simultaneous,
   };

   const auto run_order = [&](release_order order) {
      const auto verifier = api::make_savanna_finality_verifier(checkpoint);
      auto ready = std::latch{2};
      auto release_lower = std::latch{1};
      auto release_higher = std::latch{1};
      auto lower_done = std::latch{1};
      auto higher_done = std::latch{1};
      auto result = completion_result{};

      auto lower = std::thread{[&] {
         ready.count_down();
         release_lower.wait();
         try {
            verifier->verify(fixture.anchor(3U), lower_proof);
         } catch (...) {
            result.lower_error = std::current_exception();
         }
         lower_done.count_down();
      }};
      auto higher = std::thread{[&] {
         ready.count_down();
         release_higher.wait();
         try {
            verifier->verify(fixture.anchor(4U), higher_proof);
         } catch (...) {
            result.higher_error = std::current_exception();
         }
         higher_done.count_down();
      }};

      ready.wait();
      switch (order) {
      case release_order::higher_first:
         release_higher.count_down();
         higher_done.wait();
         break;
      case release_order::lower_first:
         release_lower.count_down();
         lower_done.wait();
         break;
      case release_order::simultaneous:
         release_lower.count_down();
         release_higher.count_down();
         lower_done.wait();
         higher_done.wait();
         break;
      }
      result.after_first = verifier->preferred_trust_anchor();
      result.after_first_trust = forge::raw::pack(verifier->preferred_trust());
      if (order == release_order::higher_first) {
         release_lower.count_down();
      } else if (order == release_order::lower_first) {
         release_higher.count_down();
      }
      lower.join();
      higher.join();
      result.final = verifier->preferred_trust_anchor();
      result.final_trust = forge::raw::pack(verifier->preferred_trust());
      return result;
   };

   const auto is_invalid_finality = [](const std::exception_ptr& error) {
      if (!error) {
         return false;
      }
      try {
         std::rethrow_exception(error);
      } catch (const api::exceptions::invalid_finality&) {
         return true;
      } catch (...) {
         return false;
      }
   };

   const auto higher_first = run_order(release_order::higher_first);
   BOOST_CHECK(!higher_first.higher_error);
   BOOST_CHECK(!higher_first.lower_error);
   BOOST_CHECK(higher_first.after_first == fixture.candidate(4U).id);
   BOOST_CHECK(higher_first.final == fixture.candidate(4U).id);
   BOOST_CHECK(higher_first.final_trust == higher_first.after_first_trust);

   const auto lower_first = run_order(release_order::lower_first);
   BOOST_CHECK(!lower_first.lower_error);
   BOOST_CHECK(!lower_first.higher_error);
   BOOST_CHECK(lower_first.after_first == fixture.candidate(3U).id);
   BOOST_CHECK(lower_first.final == fixture.candidate(4U).id);
   BOOST_CHECK(lower_first.final_trust != lower_first.after_first_trust);

   const auto simultaneous = run_order(release_order::simultaneous);
   BOOST_CHECK(!simultaneous.higher_error);
   BOOST_CHECK(!simultaneous.lower_error || is_invalid_finality(simultaneous.lower_error));
   BOOST_CHECK(simultaneous.final == fixture.candidate(4U).id);
}

BOOST_AUTO_TEST_CASE(portable_savanna_stale_snapshot_commit_is_monotonic) {
   const auto fixture = make_rolling_witness_chain(6U);
   const auto checkpoint = savanna::finality_trust{
       savanna::advance_finality_trust(fixture.genesis_trust, fixture.proof(1U, 2U), fixture.anchor(2U))};
   const auto lower_proof = fixture.proof(2U, 3U);
   const auto higher_proof = fixture.proof(2U, 4U);
   const auto limits = savanna::finality_witness_limits{};
   auto store = api::detail::savanna_finality_trust_store{checkpoint, {}, limits};
   const auto lower_witness = savanna::decode_finality_witness(lower_proof, limits);
   const auto higher_witness = savanna::decode_finality_witness(higher_proof, limits);
   const auto lower_snapshot = store.take_snapshot(fixture.anchor(3U), lower_witness);
   const auto higher_snapshot = store.take_snapshot(fixture.anchor(4U), higher_witness);

   BOOST_REQUIRE(lower_snapshot.preferred.block == fixture.candidate(2U).id);
   BOOST_REQUIRE(higher_snapshot.preferred.block == fixture.candidate(2U).id);

   auto higher_advance = savanna::advance_finality_trust_with_replay(*higher_snapshot.source_trust, higher_witness,
                                                                     fixture.anchor(4U), limits);
   auto higher_state = higher_advance.checkpoint.value.state;
   store.install_verified(std::move(higher_advance.checkpoint), std::move(higher_advance.replay), fixture.anchor(4U),
                          forge::crypto::digest::sha256::hash(higher_proof), std::move(higher_state));
   const auto after_higher_anchor = store.preferred_trust_anchor();
   const auto after_higher_trust = forge::raw::pack(store.preferred_trust());

   auto lower_advance = savanna::advance_finality_trust_with_replay(*lower_snapshot.source_trust, lower_witness,
                                                                    fixture.anchor(3U), limits);
   auto lower_state = lower_advance.checkpoint.value.state;
   const auto lower_digest = forge::crypto::digest::sha256::hash(lower_proof);
   BOOST_CHECK_NO_THROW(store.install_verified(std::move(lower_advance.checkpoint), std::move(lower_advance.replay),
                                               fixture.anchor(3U), lower_digest, std::move(lower_state)));
   BOOST_CHECK(after_higher_anchor == fixture.candidate(4U).id);
   BOOST_CHECK(store.preferred_trust_anchor() == fixture.candidate(4U).id);
   BOOST_CHECK(forge::raw::pack(store.preferred_trust()) == after_higher_trust);
   const auto cached_lower = store.cached_replay_state(fixture.anchor(3U), lower_digest);
   BOOST_REQUIRE(cached_lower);
   BOOST_CHECK(cached_lower->id == fixture.candidate(3U).id);
}

BOOST_AUTO_TEST_CASE(portable_savanna_direct_construction_translates_invalid_trust) {
   const auto fixture = make_rich_portable_fixture();

   auto malformed = std::get<savanna::finality_checkpoint_bootstrap>(fixture.checkpoint_trust);
   malformed.value.state.id._hash[0] ^= 1U;
   BOOST_CHECK_THROW(static_cast<void>(api::savanna_finality_verifier{savanna::finality_trust{std::move(malformed)}}),
                     api::exceptions::trust_required);

   auto foreign = std::get<savanna::finality_checkpoint_bootstrap>(fixture.checkpoint_trust);
   foreign.chain._hash[0] ^= 1U;
   BOOST_CHECK_THROW(static_cast<void>(api::savanna_finality_verifier{
                         fixture.genesis_trust, std::vector<savanna::finality_trust>{std::move(foreign)}}),
                     api::exceptions::wrong_chain);

   BOOST_CHECK_NO_THROW(static_cast<void>(api::savanna_finality_verifier{
       fixture.genesis_trust, std::vector<savanna::finality_trust>{fixture.genesis_trust}}));
}

BOOST_AUTO_TEST_CASE(portable_factory_requires_a_chain_bound_finality_verifier) {
   const auto fixture = make_portable_fixture();
   const auto projections = api::make_contract_table_projection_verifier();
   BOOST_CHECK_THROW(static_cast<void>(api::make_verified_client(api::raw_client{api::service_handles{}},
                                                                 {
                                                                     .chain = fixture.chain,
                                                                     .state_domain = fixture.state_domain,
                                                                     .finality = {},
                                                                     .projections = projections,
                                                                 })),
                     api::exceptions::trust_required);

   auto wrong_chain = fixture.chain;
   wrong_chain._hash[0] ^= 1U;
   BOOST_CHECK_THROW(
       static_cast<void>(api::make_verified_client(api::raw_client{api::service_handles{}},
                                                   {
                                                       .chain = wrong_chain,
                                                       .state_domain = fixture.state_domain,
                                                       .finality = api::make_savanna_finality_verifier(fixture.trust),
                                                       .projections = projections,
                                                   })),
       api::exceptions::wrong_chain);
}

BOOST_AUTO_TEST_CASE(portable_factory_translates_trusted_chain_failures) {
   const auto fixture = make_portable_fixture();
   const auto make_client = [&](trusted_chain_behavior behavior) {
      return api::make_verified_client(
          api::raw_client{api::service_handles{}},
          {
              .chain = fixture.chain,
              .state_domain = fixture.state_domain,
              .finality = std::make_shared<preflight_finality_verifier>(fixture.chain, behavior),
              .projections = api::make_contract_table_projection_verifier(),
          });
   };

   BOOST_CHECK_NO_THROW(static_cast<void>(make_client(trusted_chain_behavior::value)));
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::missing)), api::exceptions::trust_required);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::public_invalid_request)),
                     api::exceptions::invalid_request);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::public_wrong_chain)),
                     api::exceptions::wrong_chain);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::public_resource_exhausted)),
                     api::exceptions::resource_exhausted);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::foreign_forge)),
                     api::exceptions::trust_required);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::bad_alloc)),
                     api::exceptions::resource_exhausted);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::length_error)),
                     api::exceptions::resource_exhausted);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::standard_error)),
                     api::exceptions::trust_required);
   BOOST_CHECK_THROW(static_cast<void>(make_client(trusted_chain_behavior::nonstandard_error)),
                     api::exceptions::trust_required);
}

BOOST_AUTO_TEST_CASE(portable_factory_rejects_cross_chain_trust_before_selecting_preferred_anchor) {
   const auto fixture = make_rich_portable_fixture();
   auto foreign_checkpoint = std::get<savanna::finality_checkpoint_bootstrap>(fixture.checkpoint_trust);
   foreign_checkpoint.chain._hash[0] ^= 1U;

   BOOST_CHECK_THROW(
       static_cast<void>(make_rich_client(
           fixture, fixture.genesis_trust, {savanna::finality_trust{std::move(foreign_checkpoint)}},
           fixture.genesis_finality,
           [&](const auto& request) { return make_rich_rows_response(fixture, request, fixture.genesis_finality); },
           [&](const auto&) { return make_empty_changes_response(fixture); })),
       api::exceptions::wrong_chain);
}

BOOST_AUTO_TEST_CASE(portable_savanna_restores_an_identical_configured_checkpoint_without_ratchet) {
   const auto fixture = make_rich_portable_fixture();
   const auto configured = api::make_savanna_finality_verifier(fixture.checkpoint_trust);
   const auto configured_trust = forge::raw::pack(configured->preferred_trust());
   const auto configured_anchor = configured->preferred_trust_anchor();
   BOOST_REQUIRE(configured_anchor.has_value());
   BOOST_CHECK(*configured_anchor == savanna::trust_anchor(fixture.checkpoint_trust).block);

   // The second identical root models restoring the last persisted checkpoint at process restart.
   const auto restored =
       api::make_savanna_finality_verifier_with_trusts(fixture.checkpoint_trust, {fixture.checkpoint_trust});
   BOOST_CHECK(forge::raw::pack(restored->preferred_trust()) == configured_trust);
   BOOST_REQUIRE(restored->preferred_trust_anchor().has_value());
   BOOST_CHECK(*restored->preferred_trust_anchor() == *configured_anchor);
}

BOOST_AUTO_TEST_CASE(portable_savanna_multi_trust_rejects_distinct_public_trust_sets) {
   const auto fixture = make_rich_portable_fixture();
   auto foreign_checkpoint = std::get<savanna::finality_checkpoint_bootstrap>(fixture.checkpoint_trust);
   foreign_checkpoint.chain._hash[0] ^= 1U;
   const auto mixed = std::vector<savanna::finality_trust>{
       fixture.genesis_trust,
       savanna::finality_trust{std::move(foreign_checkpoint)},
   };
   BOOST_CHECK_THROW(
       static_cast<void>(api::make_savanna_finality_verifier_with_trusts(fixture.genesis_trust, {mixed.back()})),
       api::exceptions::wrong_chain);

   auto same_height_checkpoint = std::get<savanna::finality_checkpoint_bootstrap>(fixture.checkpoint_trust);
   same_height_checkpoint.value.state.header.transaction_mroot._hash[0] ^= 1U;
   same_height_checkpoint.value.state.id = protocol::calculate_block_id(same_height_checkpoint.value.state.header);
   same_height_checkpoint.value.state.block = same_height_checkpoint.value.state.make_block_ref();
   same_height_checkpoint.value.finalized = same_height_checkpoint.value.state.block;
   const auto same_height = savanna::finality_trust{std::move(same_height_checkpoint)};
   BOOST_REQUIRE(savanna::trust_anchor(same_height).chain == savanna::trust_anchor(fixture.checkpoint_trust).chain);
   BOOST_REQUIRE(savanna::trust_anchor(same_height).block != savanna::trust_anchor(fixture.checkpoint_trust).block);
   BOOST_REQUIRE(std::get<savanna::finality_checkpoint_bootstrap>(same_height).value.finalized.num ==
                 std::get<savanna::finality_checkpoint_bootstrap>(fixture.checkpoint_trust).value.finalized.num);
   BOOST_CHECK_THROW(
       static_cast<void>(api::make_savanna_finality_verifier_with_trusts(fixture.checkpoint_trust, {same_height})),
       api::exceptions::invalid_request);
   BOOST_CHECK_THROW(
       static_cast<void>(api::make_savanna_finality_verifier_with_trusts(same_height, {fixture.checkpoint_trust})),
       api::exceptions::invalid_request);

   const auto verifier =
       api::make_savanna_finality_verifier_with_trusts(fixture.genesis_trust, {fixture.checkpoint_trust});
   auto unknown_bootstrap = savanna::trust_anchor(fixture.genesis_trust).block;
   unknown_bootstrap._hash[0] ^= 1U;
   const auto unknown =
       savanna::encode_finality_witness(savanna::make_finality_witness(fixture.chain, unknown_bootstrap, {}));
   BOOST_CHECK_THROW(verifier->verify(fixture.target_anchor, unknown), api::exceptions::trust_required);
   BOOST_CHECK_THROW(static_cast<void>(verifier->replay_state(fixture.target_anchor, unknown)),
                     api::exceptions::trust_required);

   auto wrong_chain = fixture.chain;
   wrong_chain._hash[0] ^= 1U;
   const auto foreign = savanna::encode_finality_witness(
       savanna::make_finality_witness(wrong_chain, savanna::trust_anchor(fixture.genesis_trust).block, {}));
   BOOST_CHECK_THROW(verifier->verify(fixture.target_anchor, foreign), api::exceptions::wrong_chain);
   BOOST_CHECK_THROW(static_cast<void>(verifier->replay_state(fixture.target_anchor, foreign)),
                     api::exceptions::wrong_chain);

   auto malformed = fixture.genesis_finality;
   malformed.payload.clear();
   BOOST_CHECK_THROW(static_cast<void>(verifier->replay_state(fixture.target_anchor, malformed)),
                     api::exceptions::invalid_finality);
}

BOOST_AUTO_TEST_CASE(portable_factory_accepts_checkpoint_and_bidirectional_primary_secondary_pagination) {
   const auto fixture = make_rich_portable_fixture();
   auto harness = make_rich_client(
       fixture, fixture.genesis_trust, {}, fixture.genesis_finality,
       [&](const auto& request) { return make_rich_rows_response(fixture, request, fixture.genesis_finality); },
       [&](const auto&) { return make_empty_changes_response(fixture); });

   for (const auto kind : {protocol::table_index_kind::primary, protocol::table_index_kind::secondary_u64}) {
      for (const auto reverse : {false, true}) {
         auto request = protocol::table_rows_request{
             .code = protocol::account_name{fixture.rows_location.code},
             .scope = protocol::name{fixture.rows_location.scope},
             .table = protocol::table_name{fixture.rows_location.table},
             .index = {.kind = kind,
                       .position = static_cast<std::uint8_t>(kind == protocol::table_index_kind::primary ? 0U : 1U)},
             .limit = 1U,
             .reverse = reverse,
             .audit = protocol::audit_mode::required,
         };
         const auto request_offset = harness.service->rows_requests().size();
         const auto first = run(harness.client.get_table_rows(request));
         BOOST_REQUIRE_EQUAL(first.rows.size(), 1U);
         BOOST_REQUIRE(first.next.has_value());
         BOOST_REQUIRE(first.audit.has_value());
         const auto first_proof = authenticated::decode_range(db_bytes(first.audit->state.front().payload));
         BOOST_CHECK(first_proof.request == rows_proof_request(fixture, harness.service->rows_requests().back()));
         BOOST_TEST(first_proof.request.reverse == reverse);

         request.cursor = first.next;
         const auto second = run(harness.client.get_table_rows(request));
         BOOST_REQUIRE_EQUAL(second.rows.size(), 1U);
         BOOST_TEST(!second.next.has_value());
         BOOST_CHECK(second.rows.front() != first.rows.front());
         BOOST_REQUIRE_EQUAL(harness.service->rows_requests().size(), request_offset + 2U);
         BOOST_CHECK(harness.service->rows_requests().back().cursor == first.next);
         BOOST_REQUIRE(second.audit.has_value());
         const auto second_proof = authenticated::decode_range(db_bytes(second.audit->state.front().payload));
         BOOST_CHECK(second_proof.request == rows_proof_request(fixture, harness.service->rows_requests().back()));
         if (reverse) {
            BOOST_REQUIRE(second_proof.request.upper.has_value());
            BOOST_CHECK(*second_proof.request.upper == db_bytes(*first.next));
         } else {
            BOOST_REQUIRE(second_proof.request.lower.has_value());
            BOOST_CHECK(*second_proof.request.lower == db_bytes(*first.next));
         }
         if (kind == protocol::table_index_kind::secondary_u64) {
            BOOST_REQUIRE_EQUAL(first.audit->state.size(), 2U);
            BOOST_REQUIRE_EQUAL(second.audit->state.size(), 2U);
         }
      }
   }

   auto checkpoint = make_rich_client(
       fixture, fixture.checkpoint_trust, {}, fixture.checkpoint_finality,
       [&](const auto& request) { return make_rich_rows_response(fixture, request, fixture.checkpoint_finality); },
       [&](const auto&) { return make_empty_changes_response(fixture); });
   const auto checkpoint_rows = run(checkpoint.client.get_table_rows(protocol::table_rows_request{
       .code = protocol::account_name{fixture.rows_location.code},
       .scope = protocol::name{fixture.rows_location.scope},
       .table = protocol::table_name{fixture.rows_location.table},
       .index = {.kind = protocol::table_index_kind::primary},
       .limit = 2U,
       .audit = protocol::audit_mode::required,
   }));
   BOOST_REQUIRE_EQUAL(checkpoint_rows.rows.size(), 2U);
   BOOST_REQUIRE_EQUAL(checkpoint.service->rows_requests().size(), 1U);
   BOOST_CHECK(checkpoint.service->rows_requests().front().finality_from ==
               savanna::trust_anchor(fixture.checkpoint_trust).block);
}

BOOST_AUTO_TEST_CASE(portable_factory_accepts_multi_block_changes_cursors_empty_interval_and_fails_closed) {
   const auto fixture = make_rich_portable_fixture();
   auto harness = make_rich_client(
       fixture, fixture.genesis_trust, {}, fixture.genesis_finality,
       [&](const auto& request) { return make_rich_rows_response(fixture, request, fixture.genesis_finality); },
       [&](const auto& request) {
          if (request.from_block == request.to_block) {
             return make_empty_changes_response(fixture);
          }
          return request.limit == 1U ? make_changes_page(fixture, request)
                                     : make_full_changes_response(fixture, request);
       });

   const auto full_request = protocol::table_changes_request{
       .from_block = fixture.first_change_anchor.block_num - 1U,
       .to_block = fixture.target_anchor.block_num,
       .tables = fixture.selectors,
       .limit = 10U,
       .audit = protocol::audit_mode::required,
   };
   const auto full = run(harness.client.get_table_changes(full_request));
   BOOST_REQUIRE_EQUAL(full.blocks.size(), 2U);
   BOOST_REQUIRE(full.audit.has_value());
   BOOST_REQUIRE_EQUAL(full.audit->state.size(), 4U);
   BOOST_CHECK(full.blocks[0].mutations == fixture.first_block_mutations);
   BOOST_CHECK(full.blocks[1].mutations == fixture.target_block_mutations);
   for (const auto& encoded : full.audit->state) {
      const auto proof = authenticated::decode_range(db_bytes(encoded.payload));
      BOOST_CHECK(proof.tree == authenticated::proof_tree::changes);
      BOOST_REQUIRE(proof.request.lower.has_value());
      BOOST_REQUIRE(proof.request.upper.has_value());
      BOOST_TEST(*proof.request.lower < *proof.request.upper);
   }

   auto page_request = full_request;
   page_request.limit = 1U;
   auto seen = std::vector<std::uint64_t>{};
   auto pages = std::size_t{};
   do {
      const auto expected_cursor = page_request.cursor;
      const auto page = run(harness.client.get_table_changes(page_request));
      ++pages;
      BOOST_REQUIRE(!page.blocks.empty());
      BOOST_CHECK(harness.service->changes_requests().back().cursor == expected_cursor);
      for (const auto& mutation : page.blocks.front().mutations) {
         seen.push_back(mutation.primary);
      }
      page_request.cursor = page.next;
   } while (page_request.cursor);
   BOOST_TEST(pages == 4U);
   BOOST_CHECK(seen == std::vector<std::uint64_t>({21U, 22U, 31U}));
   BOOST_CHECK(harness.service->changes_requests().back().cursor.has_value());

   const auto empty = run(harness.client.get_table_changes(protocol::table_changes_request{
       .from_block = fixture.target_anchor.block_num,
       .to_block = fixture.target_anchor.block_num,
       .tables = fixture.selectors,
       .limit = 1U,
       .audit = protocol::audit_mode::required,
   }));
   BOOST_TEST(empty.blocks.empty());
   BOOST_TEST(!empty.next.has_value());
   BOOST_REQUIRE(empty.audit.has_value());
   BOOST_TEST(empty.audit->state.empty());

   auto scope_request = protocol::table_scope_request{
       .code = protocol::account_name{fixture.rows_location.code},
       .table = protocol::table_name{fixture.rows_location.table},
       .audit = protocol::audit_mode::required,
   };
   BOOST_CHECK_THROW(run(harness.client.get_table_scope(scope_request)), api::exceptions::audit_not_supported);
   auto account_request = protocol::account_request{};
   account_request.key = protocol::account_name{"alice"};
   account_request.audit = protocol::audit_mode::required;
   BOOST_CHECK_THROW(run(harness.client.get_account(account_request)), api::exceptions::audit_not_supported);
}

BOOST_AUTO_TEST_CASE(portable_factory_uses_a_retained_early_anchor_for_historical_changes_after_ratchet) {
   const auto fixture = make_rich_portable_fixture();
   const auto checkpoint_anchor = savanna::trust_anchor(fixture.checkpoint_trust);
   auto harness = make_rich_client(
       fixture, fixture.genesis_trust, {fixture.checkpoint_trust}, fixture.checkpoint_finality,
       [&](const auto& request) { return make_rich_rows_response(fixture, request, fixture.checkpoint_finality); },
       [&](const auto& request) {
          auto response = make_full_changes_response(fixture, request);
          response.audit->finality = fixture.checkpoint_finality;
          response.audit->ancestry = fixture.checkpoint_finality;
          return response;
       });

   static_cast<void>(run(harness.client.get_table_rows(protocol::table_rows_request{
       .code = protocol::account_name{fixture.rows_location.code},
       .scope = protocol::name{fixture.rows_location.scope},
       .table = protocol::table_name{fixture.rows_location.table},
       .index = {.kind = protocol::table_index_kind::primary},
       .limit = 2U,
       .audit = protocol::audit_mode::required,
   })));
   BOOST_REQUIRE(harness.finality->preferred_trust_anchor().has_value());
   BOOST_CHECK(*harness.finality->preferred_trust_anchor() == fixture.target_anchor.block);
   BOOST_CHECK(harness.finality->trust_anchor_at_or_before(fixture.first_change_anchor.block_num) ==
               checkpoint_anchor.block);

   const auto changes = run(harness.client.get_table_changes(protocol::table_changes_request{
       .from_block = protocol::calculate_block_num_from_id(checkpoint_anchor.block),
       .to_block = fixture.target_anchor.block_num,
       .tables = fixture.selectors,
       .limit = 10U,
       .audit = protocol::audit_mode::required,
   }));
   BOOST_REQUIRE_EQUAL(changes.blocks.size(), 2U);
   BOOST_REQUIRE_EQUAL(harness.service->changes_requests().size(), 1U);
   BOOST_REQUIRE(harness.service->changes_requests().front().finality_from.has_value());
   BOOST_CHECK(*harness.service->changes_requests().front().finality_from == checkpoint_anchor.block);
}

BOOST_AUTO_TEST_CASE(portable_verified_client_rejects_malicious_rows_keys_payers_cursors_proofs_and_anchors) {
   const auto fixture = make_portable_fixture();
   const auto reject = [&](protocol::table_rows_response response) {
      auto client = make_rows_client(fixture, std::move(response));
      BOOST_CHECK_THROW(run(client.get_table_rows(fixture.rows_request)), api::exceptions::invalid_state_proof);
   };

   auto row = fixture.rows_response;
   row.rows.front().value.front() ^= 1U;
   reject(std::move(row));

   auto payer = fixture.rows_response;
   payer.rows.front().payer = protocol::account_name{"attacker"};
   reject(std::move(payer));

   auto cursor = fixture.rows_response;
   cursor.next = protocol::bytes{0x01U};
   reject(std::move(cursor));

   auto key = fixture.rows_response;
   auto key_proof = fixture.rows_proof;
   auto& key_leaf = std::get<authenticated::proof_leaf>(key_proof.nodes.front());
   key_leaf.key.back() ^= std::byte{1U};
   key.audit->state.front() = proof_blob("forge.db.authenticated.range", key_proof);
   reject(std::move(key));

   auto proof = fixture.rows_response;
   flip_first_payload_byte(proof.audit->state.front());
   reject(std::move(proof));

   auto anchor = fixture.rows_response;
   anchor.context.anchor->state_size += 1U;
   auto anchor_client = make_rows_client(fixture, std::move(anchor));
   BOOST_CHECK_THROW(run(anchor_client.get_table_rows(fixture.rows_request)), api::exceptions::invalid_finality);
}

BOOST_AUTO_TEST_CASE(portable_verified_client_rejects_malicious_changes_and_tombstones) {
   const auto fixture = make_portable_fixture();
   const auto reject = [&](protocol::table_changes_response response) {
      auto client = make_changes_client(fixture, std::move(response));
      BOOST_CHECK_THROW(run(client.get_table_changes(fixture.changes_request)), api::exceptions::invalid_state_proof);
   };

   auto row = fixture.changes_response;
   row.blocks.front().mutations.front().row->value.front() ^= 1U;
   reject(std::move(row));

   auto payer = fixture.changes_response;
   payer.blocks.front().mutations.front().row->payer = protocol::account_name{"attacker"};
   reject(std::move(payer));

   auto primary = fixture.changes_response;
   primary.blocks.front().mutations.front().primary += 1U;
   reject(std::move(primary));

   auto tombstone = fixture.changes_response;
   tombstone.blocks.front().mutations.front().row.reset();
   reject(std::move(tombstone));

   auto cursor = fixture.changes_response;
   cursor.next = protocol::bytes{0x01U};
   reject(std::move(cursor));

   auto key = fixture.changes_response;
   auto key_proof = fixture.changes_proof;
   auto& key_leaf = std::get<authenticated::proof_leaf>(key_proof.nodes.front());
   key_leaf.key.back() ^= std::byte{1U};
   key.audit->state.front() = proof_blob("forge.db.authenticated.changes", key_proof);
   reject(std::move(key));

   auto proof = fixture.changes_response;
   flip_first_payload_byte(proof.audit->state.front());
   reject(std::move(proof));

   auto anchor = fixture.changes_response;
   anchor.blocks.front().anchor.change_count += 1U;
   reject(std::move(anchor));
}

BOOST_AUTO_TEST_CASE(portable_savanna_verifier_maps_signature_qc_witness_and_chain_failures_to_public_errors) {
   const auto fixture = make_portable_fixture();

   auto bad_signature = fixture.rows_response;
   auto signature_witness = savanna::decode_finality_witness(*bad_signature.audit->finality);
   const auto outsider = make_producer_key(1U);
   signature_witness.records.front().header.producer_signature =
       sign(outsider, protocol::calculate_block_id(signature_witness.records.front().header));
   bad_signature.audit->finality = savanna::encode_finality_witness(signature_witness);
   auto signature_client = make_rows_client(fixture, std::move(bad_signature));
   BOOST_CHECK_THROW(run(signature_client.get_table_rows(fixture.rows_request)), api::exceptions::invalid_finality);

   auto bad_qc = fixture.rows_response;
   auto qc_witness = savanna::decode_finality_witness(*bad_qc.audit->finality);
   BOOST_REQUIRE(qc_witness.records.size() > 1U);
   BOOST_REQUIRE(!qc_witness.records[1].block_extensions.empty());
   qc_witness.records[1].block_extensions.front().second.back() ^= 1U;
   bad_qc.audit->finality = savanna::encode_finality_witness(qc_witness);
   auto qc_client = make_rows_client(fixture, std::move(bad_qc));
   BOOST_CHECK_THROW(run(qc_client.get_table_rows(fixture.rows_request)), api::exceptions::invalid_finality);

   auto oversized_count = fixture.rows_response;
   auto& payload = oversized_count.audit->finality->payload;
   const auto count_offset =
       forge::raw::pack(fixture.chain).size() + forge::raw::pack(savanna::trust_anchor(fixture.trust).block).size();
   BOOST_REQUIRE(count_offset < payload.size());
   payload.erase(payload.begin() + static_cast<std::ptrdiff_t>(count_offset));
   payload.insert(payload.begin() + static_cast<std::ptrdiff_t>(count_offset), {0x81U, 0x20U});
   auto count_client = make_rows_client(fixture, std::move(oversized_count));
   BOOST_CHECK_THROW(run(count_client.get_table_rows(fixture.rows_request)), api::exceptions::resource_exhausted);

   auto wrong_chain = fixture.rows_response;
   wrong_chain.context.chain._hash[0] ^= 1U;
   auto chain_client = make_rows_client(fixture, std::move(wrong_chain));
   BOOST_CHECK_THROW(run(chain_client.get_table_rows(fixture.rows_request)), api::exceptions::wrong_chain);
}

BOOST_AUTO_TEST_CASE(portable_savanna_public_replay_preparation_translates_malformed_proofs) {
   const auto fixture = make_rolling_witness_chain(6U);
   const auto checkpoint = savanna::finality_trust{
       savanna::advance_finality_trust(fixture.genesis_trust, fixture.proof(1U, 2U), fixture.anchor(2U))};
   const auto verifier = api::make_savanna_finality_verifier(checkpoint);
   auto malformed = fixture.proof(2U, 3U);
   malformed.payload.clear();
   const auto preferred = forge::raw::pack(verifier->preferred_trust());

   BOOST_CHECK_THROW(static_cast<void>(verifier->replay_state(fixture.anchor(3U), malformed)),
                     api::exceptions::invalid_finality);
   BOOST_CHECK_THROW(verifier->verify(fixture.anchor(3U), malformed), api::exceptions::invalid_finality);
   BOOST_CHECK(forge::raw::pack(verifier->preferred_trust()) == preferred);
}

BOOST_AUTO_TEST_CASE(portable_savanna_trust_rejects_invalid_bls_subgroup_through_public_exception) {
   constexpr auto wrong_subgroup_hex =
       "9eb987464f483a62537c33715426bd5fd50c7f5e85f51c634d85081974df95794fc79e95ee5aafa38578ad42f502b1124"
       "ddf24f4172a370ce94e3cc81eaa698b9bf4762d4a31f01015b179eae0ee37aa6b07a8edc1246defae68c4f7139bb40d";
   auto bytes = bls::public_key::data_type{};
   BOOST_REQUIRE_EQUAL(forge::codec::hex::decode(wrong_subgroup_hex, bytes), bytes.size());
   const auto malformed = bls::public_key{bytes};
   BOOST_TEST(!bls::valid(malformed));

   const auto fixture = make_portable_fixture();
   auto trust = std::get<savanna::finality_genesis_bootstrap>(fixture.trust);
   trust.configuration.finalizers.finalizers.front().public_key = malformed;
   auto services = forge::api::core::registry{};
   services.install<api::state>(
       std::make_shared<fixture_state_service>(fixture.rows_response, fixture.changes_response));
   auto raw = api::raw_client{api::service_handles{
       .state_queries = services.get<api::state>(api::state::ref()),
   }};
   static_cast<void>(raw);
   BOOST_CHECK_THROW(static_cast<void>(api::make_savanna_finality_verifier(savanna::finality_trust{std::move(trust)})),
                     api::exceptions::trust_required);
}

BOOST_AUTO_TEST_CASE(portable_savanna_bounded_decode_rejects_malformed_nested_lengths_before_allocation) {
   const auto fixture = make_portable_fixture();
   auto proof = fixture.finality_proof;
   const auto witness = savanna::decode_finality_witness(proof);
   const auto count_offset =
       forge::raw::pack(fixture.chain).size() + forge::raw::pack(savanna::trust_anchor(fixture.trust).block).size();
   proof.payload.erase(proof.payload.begin() + static_cast<std::ptrdiff_t>(count_offset));
   proof.payload.insert(proof.payload.begin() + static_cast<std::ptrdiff_t>(count_offset), {0x81U, 0x20U});
   BOOST_CHECK_THROW(static_cast<void>(savanna::decode_finality_witness(proof)),
                     savanna::exceptions::finality_witness_limit_exceeded);

   auto nested_length = fixture.finality_proof;
   const auto first_record_offset = count_offset + 1U;
   const auto extensions_offset = first_record_offset + forge::raw::pack(witness.records.front().header).size();
   BOOST_REQUIRE(extensions_offset < nested_length.payload.size());
   BOOST_TEST(nested_length.payload[extensions_offset] == 0U);
   nested_length.payload.erase(nested_length.payload.begin() + static_cast<std::ptrdiff_t>(extensions_offset));
   nested_length.payload.insert(nested_length.payload.begin() + static_cast<std::ptrdiff_t>(extensions_offset),
                                {0x01U, 0x02U, 0x00U, 0x80U, 0x80U, 0x40U});
   BOOST_CHECK_THROW(static_cast<void>(savanna::decode_finality_witness(nested_length)),
                     savanna::exceptions::finality_witness_limit_exceeded);

   const auto malformed_vector = protocol::bytes{0xffU, 0xffU, 0xffU, 0xffU, 0x0fU};
   const auto headers = protocol::extensions{
       {savanna::protocol_feature_extension_id, malformed_vector},
       {savanna::finality_extension_id, forge::raw::pack(savanna::finality_extension{})},
       {savanna::state_commitment_extension_id, forge::raw::pack(savanna::state_commitment{})},
   };
   BOOST_CHECK_THROW(static_cast<void>(savanna::decode_header_extensions(headers)),
                     savanna::exceptions::invalid_extension);

   const auto blocks = protocol::extensions{{savanna::additional_signatures_extension_id, malformed_vector}};
   BOOST_CHECK_THROW(static_cast<void>(savanna::decode_block_extensions(blocks)),
                     savanna::exceptions::invalid_extension);
}
