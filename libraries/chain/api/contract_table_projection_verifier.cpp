module;

#include <forge/exceptions/macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

module forge.chain.api.contract_table_projection_verifier;

import forge.chain.api.exceptions;
import forge.chain.api.table_key;
import forge.crypto.digest.sha256;
import forge.raw.raw;

namespace forge::chain::api {
namespace {

namespace protocol = forge::chain::protocol;
namespace commitment = forge::chain::protocol;

[[noreturn]] void reject(std::string_view message) {
   FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof, std::string{message});
}

commitment::commitment_bytes state_bytes(const protocol::bytes& value) {
   auto result = commitment::commitment_bytes{};
   result.reserve(value.size());
   std::ranges::transform(value, std::back_inserter(result),
                          [](std::uint8_t byte) { return static_cast<std::byte>(byte); });
   return result;
}

protocol::bytes protocol_bytes(std::span<const std::byte> value) {
   auto result = protocol::bytes{};
   result.reserve(value.size());
   std::ranges::transform(value, std::back_inserter(result),
                          [](std::byte byte) { return std::to_integer<std::uint8_t>(byte); });
   return result;
}

enum class typed_change_kind : std::uint8_t {
   table = 1U,
};

enum class typed_change_phase : std::uint8_t {
   table = 0U,
};

struct typed_change_position {
   protocol::chain_id chain;
   protocol::block_id target;
   protocol::digest request;
   std::uint32_t from_block = 0;
   std::uint32_t to_block = 0;
   std::uint32_t block = 0;
   std::uint32_t selector = 0;
   typed_change_phase phase = typed_change_phase::table;
   std::optional<protocol::bytes> key;
   bool changed = false;
};

using typed_change_cursor_wire =
    std::tuple<std::uint8_t, std::uint8_t, protocol::chain_id, protocol::block_id, protocol::digest, std::uint32_t,
               std::uint32_t, std::uint32_t, std::uint32_t, std::uint8_t, std::optional<protocol::bytes>, bool>;

template <typename Value> protocol::digest request_fingerprint(const Value& value) {
   const auto encoded = forge::raw::pack(value);
   return forge::crypto::digest::sha256::hash(std::span<const std::uint8_t>{encoded});
}

protocol::bytes encode_cursor(typed_change_kind kind, const typed_change_position& value) {
   return forge::raw::pack(typed_change_cursor_wire{
       2U,
       static_cast<std::uint8_t>(kind),
       value.chain,
       value.target,
       value.request,
       value.from_block,
       value.to_block,
       value.block,
       value.selector,
       static_cast<std::uint8_t>(value.phase),
       value.key,
       value.changed,
   });
}

typed_change_position decode_cursor(const protocol::bytes& encoded, typed_change_kind expected_kind,
                                    protocol::digest expected_request, std::uint32_t from_block,
                                    std::uint32_t to_block) {
   try {
      const auto value = forge::raw::unpack_exact<typed_change_cursor_wire>(encoded);
      if (forge::raw::pack(value) != encoded || std::get<0>(value) != 2U ||
          std::get<1>(value) != static_cast<std::uint8_t>(expected_kind) || std::get<4>(value) != expected_request ||
          std::get<5>(value) != from_block || std::get<6>(value) != to_block ||
          std::get<9>(value) != static_cast<std::uint8_t>(typed_change_phase::table)) {
         reject("Savanna typed state changes cursor does not match its audited request");
      }
      return {
          .chain = std::get<2>(value),
          .target = std::get<3>(value),
          .request = std::get<4>(value),
          .from_block = std::get<5>(value),
          .to_block = std::get<6>(value),
          .block = std::get<7>(value),
          .selector = std::get<8>(value),
          .phase = static_cast<typed_change_phase>(std::get<9>(value)),
          .key = std::get<10>(value),
          .changed = std::get<11>(value),
      };
   } catch (const exceptions::invalid_state_proof&) {
      throw;
   } catch (const std::exception&) {
      reject("Savanna typed state changes cursor has invalid canonical bytes");
   }
}

protocol::digest table_request_fingerprint(const protocol::table_changes_request& value) {
   return request_fingerprint(
       std::tuple{value.from_block, value.to_block, value.tables, value.limit, value.finality_from, value.audit});
}

bool advance_position(typed_change_position& position, std::size_t selector_count) {
   position.key.reset();
   position.changed = false;
   position.phase = typed_change_phase::table;
   ++position.selector;
   if (position.selector < selector_count) {
      return true;
   }
   position.selector = 0U;
   if (position.block == position.to_block) {
      return false;
   }
   ++position.block;
   return true;
}

commitment::commitment_bytes prefix_upper_bound(commitment::commitment_bytes prefix) {
   for (auto position = prefix.size(); position > 0U; --position) {
      const auto index = position - 1U;
      const auto value = std::to_integer<std::uint8_t>(prefix[index]);
      if (value != 0xffU) {
         prefix[index] = static_cast<std::byte>(value + 1U);
         prefix.resize(position);
         return prefix;
      }
   }
   reject("Savanna contract table prefix has no upper bound");
}

commitment::commitment_bytes table_change_prefix(const protocol::table_change_selector& selector) {
   return commitment::contract_index_prefix(
       {
           .code = selector.code.value,
           .scope = selector.scope.value,
           .table = selector.table.value,
       },
       commitment::contract_table_family::primary);
}

bool has_prefix(std::span<const std::byte> value, std::span<const std::byte> prefix) {
   return value.size() >= prefix.size() && std::ranges::equal(prefix, value.first(prefix.size()));
}

bool key_less(std::span<const std::byte> left, std::span<const std::byte> right) {
   return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
}

std::uint64_t decode_unsigned(std::span<const std::byte> value) {
   auto result = std::uint64_t{};
   for (const auto byte : value) {
      result = (result << 8U) | std::to_integer<std::uint8_t>(byte);
   }
   return result;
}

struct secondary_index_spec {
   commitment::contract_table_family family;
   std::size_t size = 0;
};

secondary_index_spec select_secondary_index(protocol::table_index_kind kind) {
   switch (kind) {
   case protocol::table_index_kind::secondary_u64:
      return {commitment::contract_table_family::secondary_u64, 8U};
   case protocol::table_index_kind::secondary_u128:
      return {commitment::contract_table_family::secondary_u128, 16U};
   case protocol::table_index_kind::secondary_u256:
      return {commitment::contract_table_family::secondary_u256, 32U};
   case protocol::table_index_kind::secondary_f64:
      return {commitment::contract_table_family::secondary_f64, 8U};
   case protocol::table_index_kind::secondary_f128:
      return {commitment::contract_table_family::secondary_f128, 16U};
   case protocol::table_index_kind::primary:
      break;
   }
   reject("Savanna primary table index has no secondary key family");
}

struct table_index_selection {
   bool primary = true;
   std::uint64_t table = 0;
   commitment::contract_table_family family = commitment::contract_table_family::primary;
   std::size_t key_size = 8U;
};

table_index_selection select_table_index(const protocol::table_rows_request& value) {
   constexpr auto index_mask = std::uint64_t{0x0fU};
   if ((value.table.value & index_mask) != 0U) {
      reject("Savanna table rows request uses a table name with reserved index bits");
   }
   try {
      validate_table_index(value.index);
   } catch (const exceptions::invalid_request&) {
      reject("Savanna table rows request has an invalid typed index");
   }
   if (value.index.kind == protocol::table_index_kind::primary) {
      return {.primary = true, .table = value.table.value};
   }
   const auto secondary = select_secondary_index(value.index.kind);
   return {
       .primary = false,
       .table = value.table.value | value.index.position,
       .family = secondary.family,
       .key_size = secondary.size,
   };
}

commitment::commitment_bytes table_bound(const commitment::commitment_bytes& prefix, protocol::table_index_kind kind,
                                         const protocol::bytes& value) {
   try {
      validate_table_key(kind, value);
   } catch (const exceptions::invalid_request&) {
      reject("Savanna table rows request has an invalid canonical bound");
   }
   auto result = prefix;
   const auto suffix = state_bytes(value);
   result.insert(result.end(), suffix.begin(), suffix.end());
   return result;
}

commitment::commitment_bytes exclusive_successor(commitment::commitment_bytes value) {
   for (auto position = value.size(); position > 0U; --position) {
      const auto index = position - 1U;
      const auto byte = std::to_integer<std::uint8_t>(value[index]);
      if (byte != std::numeric_limits<std::uint8_t>::max()) {
         value[index] = static_cast<std::byte>(byte + 1U);
         value.resize(position);
         return value;
      }
   }
   reject("Savanna contract table key has no exclusive successor");
}

void validate_table_cursor(std::span<const std::byte> cursor, std::span<const std::byte> prefix,
                           const table_index_selection& selected) {
   const auto expected_size = prefix.size() + selected.key_size + (selected.primary ? 0U : 8U);
   if (cursor.size() != expected_size || !has_prefix(cursor, prefix)) {
      reject("Savanna table rows cursor has invalid canonical framing");
   }
}

std::uint64_t decode_primary_key(const protocol::bytes& key, std::span<const std::byte> prefix,
                                 std::size_t ordered_fields) {
   constexpr auto ordered_uint64_size = std::size_t{8U};
   const auto encoded = state_bytes(key);
   const auto expected = prefix.size() + ordered_fields * ordered_uint64_size;
   if (encoded.size() != expected || !has_prefix(encoded, prefix)) {
      reject("Savanna table key has invalid canonical framing");
   }
   return decode_unsigned(std::span<const std::byte>{encoded}.last(ordered_uint64_size));
}

class proof_cursor {
 public:
   proof_cursor(const protocol::state_anchor& anchor, const std::vector<protocol::proof_blob>& proofs,
                audit_verifier& verifier)
       : anchor_{anchor}, proofs_{proofs}, verifier_{verifier} {}

   protocol::bytes read(commitment::commitment_bytes key) {
      if (position_ >= proofs_.size()) {
         reject("Savanna projection response omits an authenticated source proof");
      }
      auto value = verifier_.verify_state_point(anchor_, protocol_bytes(key), proofs_[position_++]);
      if (!value) {
         reject("Savanna projection source does not exist at the audited anchor");
      }
      return std::move(*value);
   }

   authenticated_state_range read_range(commitment::commitment_bytes lower, commitment::commitment_bytes upper,
                                        std::uint32_t limit, bool reverse = false) {
      if (position_ >= proofs_.size()) {
         reject("Savanna projection response omits an authenticated source proof");
      }
      auto result = verifier_.verify_state_range(anchor_,
                                                 authenticated_range_query{
                                                     .range =
                                                         {
                                                             .lower = protocol_bytes(lower),
                                                             .upper = protocol_bytes(upper),
                                                         },
                                                     .limit = limit,
                                                     .reverse = reverse,
                                                 },
                                                 proofs_[position_++]);
      if (result.rows.size() > limit) {
         reject("Savanna projection range proof exceeds its requested limit");
      }
      return result;
   }

   void require_end() const {
      if (position_ != proofs_.size()) {
         reject("Savanna projection response contains unrelated authenticated source proofs");
      }
   }

 private:
   const protocol::state_anchor& anchor_;
   const std::vector<protocol::proof_blob>& proofs_;
   audit_verifier& verifier_;
   std::size_t position_ = 0U;
};

template <typename Value> Value unpack_source(const protocol::bytes& value) {
   try {
      return forge::raw::unpack_exact<Value>(value);
   } catch (const std::exception&) {
      reject("Savanna projection source has invalid canonical wire bytes");
   }
}

commitment::primary_value read_primary(proof_cursor& proofs, commitment::contract_table_location location,
                                       std::uint64_t primary) {
   return unpack_source<commitment::primary_value>(
       proofs.read(commitment::contract_primary_key(location, commitment::contract_table_family::primary, primary)));
}

protocol::table_rows_request reward_source_request(const protocol::producer_rewards_request& request,
                                                   protocol::account_name code, protocol::name scope,
                                                   protocol::table_name table, std::uint64_t primary) {
   const auto key = encode_table_key(primary);
   return {
       .code = code,
       .scope = scope,
       .table = table,
       .index = {.kind = protocol::table_index_kind::primary},
       .lower_bound = key,
       .upper_bound = key,
       .limit = 1U,
       .anchor = request.anchor,
       .finality_from = request.finality_from,
       .audit = request.audit,
   };
}

void verify_reward_source_context(const protocol::audited_response& outer, const protocol::table_rows_response& source,
                                  const char* name) {
   if (source.context != outer.context || !source.audit || source.audit->finality || source.audit->ancestry ||
       !source.audit->content.empty() || source.audit->transaction) {
      FORGE_THROW_EXCEPTION(exceptions::invalid_state_proof,
                            "Savanna producer reward source does not share the finalized anchor",
                            forge::exceptions::ctx("source", name));
   }
}

} // namespace

void contract_table_projection_verifier::verify(const protocol::table_rows_request& request,
                                                const protocol::table_rows_response& response,
                                                const protocol::audit_bundle& audit, audit_verifier& verifier) {
   if (!response.context.anchor) {
      reject("Savanna table rows response omits its audited state anchor");
   }
   if (!audit.content.empty()) {
      reject("Savanna binary table rows response contains unrelated content witnesses");
   }

   auto proofs = proof_cursor{*response.context.anchor, audit.state, verifier};
   const auto selected = select_table_index(request);
   const auto primary_location = commitment::contract_table_location{
       .code = request.code.value,
       .scope = request.scope.value,
       .table = request.table.value,
   };
   const auto index_location = commitment::contract_table_location{
       .code = request.code.value,
       .scope = request.scope.value,
       .table = selected.table,
   };
   const auto prefix = commitment::contract_index_prefix(index_location, selected.family);

   auto lower = request.lower_bound ? table_bound(prefix, request.index.kind, *request.lower_bound) : prefix;
   auto upper = request.upper_bound ? exclusive_successor(table_bound(prefix, request.index.kind, *request.upper_bound))
                                    : prefix_upper_bound(prefix);
   const auto requested_lower = lower;
   const auto requested_upper = upper;
   if (request.cursor) {
      auto cursor = state_bytes(*request.cursor);
      validate_table_cursor(cursor, prefix, selected);
      if (key_less(cursor, requested_lower) || !key_less(cursor, requested_upper)) {
         reject("Savanna table rows cursor is outside the requested canonical range");
      }
      if (request.reverse) {
         upper = std::move(cursor);
      } else {
         lower = std::move(cursor);
      }
   }

   if (!key_less(lower, upper)) {
      proofs.require_end();
      if (!response.rows.empty() || response.next) {
         reject("Savanna table rows response is not empty for an inverted canonical range");
      }
      return;
   }

   const auto page_limit = request.limit == 0U ? 1U : request.limit;
   auto page = proofs.read_range(lower, upper, page_limit, request.reverse);
   auto source_rows = std::move(page.rows);
   auto continuation = request.limit == 0U ? (source_rows.empty() ? std::optional<protocol::bytes>{}
                                                                  : std::optional{source_rows.front().key})
                                           : std::move(page.next_key);

   const auto decode_position = [&](const protocol::bytes& value) {
      const auto key = state_bytes(value);
      if (key_less(key, lower) || !key_less(key, upper)) {
         reject("Savanna authenticated table row key is outside the requested canonical range");
      }
      if (selected.primary) {
         return commitment::secondary_key_position{.primary = decode_primary_key(value, prefix, 1U)};
      }
      const auto position = commitment::decode_contract_secondary_key(key, prefix, selected.key_size);
      if (!position) {
         reject("Savanna authenticated secondary row key has invalid canonical framing");
      }
      return *position;
   };

   auto expected_rows = std::vector<protocol::table_row>{};
   if (request.limit != 0U) {
      expected_rows.reserve(source_rows.size());
      auto previous = std::optional<commitment::commitment_bytes>{};
      for (const auto& item : source_rows) {
         const auto key = state_bytes(item.key);
         if (previous && (request.reverse ? !key_less(key, *previous) : !key_less(*previous, key))) {
            reject("Savanna authenticated table rows are not in canonical request order");
         }
         previous = key;

         const auto position = decode_position(item.key);
         auto stored = commitment::primary_value{};
         auto payer = protocol::account_name{};
         if (selected.primary) {
            stored = unpack_source<commitment::primary_value>(item.value);
            payer = stored.payer;
         } else {
            const auto secondary = unpack_source<commitment::secondary_value>(item.value);
            if (secondary.primary != position.primary) {
               reject("Savanna authenticated secondary value disagrees with its primary tiebreak");
            }
            payer = secondary.payer;
            stored = read_primary(proofs, primary_location, secondary.primary);
         }
         expected_rows.push_back({.value = std::move(stored.row), .payer = payer});
      }
   }

   if (continuation) {
      validate_table_cursor(state_bytes(*continuation), prefix, selected);
   }

   proofs.require_end();
   if (response.rows != expected_rows) {
      reject("Savanna table rows are not the canonical authenticated projection");
   }
   if (response.next != continuation) {
      reject("Savanna table rows continuation is not bound to the authenticated range");
   }
}

void contract_table_projection_verifier::verify(const protocol::producer_rewards_request& request,
                                                const protocol::producer_rewards_response& response,
                                                const protocol::audit_bundle& audit, audit_verifier& verifier) {
   if (!response.context.anchor || !audit.state.empty() || audit.ancestry || !audit.content.empty() ||
       audit.transaction) {
      reject("Savanna producer rewards response has unrelated outer audit material");
   }
   if (protocol::calculate_block_id(response.anchor_header) != response.context.anchor->block ||
       protocol::calculate_block_num(response.anchor_header) != response.context.anchor->block_num) {
      reject("Savanna producer rewards anchor header is not bound to the finalized anchor");
   }

   verify_reward_source_context(response, response.system, "eosio.system");
   verify_reward_source_context(response, response.bpay, "eosio.bpay");

   const auto system = reward_source_request(request, protocol::account_name{"eosio"}, protocol::name{"eosio"},
                                             protocol::table_name{"producers"}, request.producer.value);
   const auto bpay = reward_source_request(request, protocol::account_name{"eosio.bpay"},
                                           protocol::name{"eosio.bpay"}, protocol::table_name{"rewards"},
                                           request.producer.value);
   verify(system, response.system, *response.system.audit, verifier);
   verify(bpay, response.bpay, *response.bpay.audit, verifier);

   if (response.system.rows.size() != 1U || response.system.next || response.bpay.rows.size() > 1U ||
       response.bpay.next) {
      reject("Savanna producer reward source does not contain its canonical table rows");
   }
   const auto producer = unpack_source<protocol::producer_info>(response.system.rows.front().value);
   if (producer.owner != request.producer) {
      reject("Savanna producer reward source does not match its request");
   }
   auto bpay_reward = std::optional<protocol::bpay_reward>{};
   if (!response.bpay.rows.empty()) {
      bpay_reward = unpack_source<protocol::bpay_reward>(response.bpay.rows.front().value);
   }

   const auto expected =
       protocol::project_producer_reward(producer, bpay_reward, response.anchor_header.timestamp.to_time_point());
   if (!expected) {
      reject("Savanna producer reward source has malformed canonical rows");
   }
   if (response.reward != *expected) {
      reject("Savanna producer rewards are not the canonical authenticated projection");
   }
}

void contract_table_projection_verifier::verify(const protocol::table_changes_request& request,
                                                const protocol::table_changes_response& response,
                                                const protocol::audit_bundle& audit, audit_verifier& verifier) {
   if (!response.context.anchor || response.context.anchor->block_num != request.to_block || request.tables.empty()) {
      reject("Savanna table changes response does not match its audited target");
   }
   for (auto position = std::size_t{1U}; position < request.tables.size(); ++position) {
      const auto& previous = request.tables[position - 1U];
      const auto& current = request.tables[position];
      if (std::tie(previous.code.value, previous.scope.value, previous.table.value) >=
          std::tie(current.code.value, current.scope.value, current.table.value)) {
         reject("Savanna table changes request has non-canonical selectors");
      }
   }
   if (request.from_block == request.to_block) {
      if (!response.blocks.empty() || response.next || !audit.state.empty()) {
         reject("Savanna empty table changes interval is not canonical");
      }
      return;
   }
   if (response.blocks.empty() || audit.state.empty()) {
      reject("Savanna table changes page must contain authenticated block batches");
   }

   const auto fingerprint = table_request_fingerprint(request);
   auto position = request.cursor ? decode_cursor(*request.cursor, typed_change_kind::table, fingerprint,
                                                  request.from_block, request.to_block)
                                  : typed_change_position{
                                        .chain = response.context.chain,
                                        .target = response.context.anchor->block,
                                        .request = fingerprint,
                                        .from_block = request.from_block,
                                        .to_block = request.to_block,
                                        .block = request.from_block + 1U,
                                    };
   if (position.chain != response.context.chain || position.target != response.context.anchor->block ||
       position.block <= position.from_block || position.block > position.to_block ||
       position.selector >= request.tables.size() || position.phase != typed_change_phase::table || position.changed) {
      reject("Savanna table changes cursor is outside its audited request");
   }

   auto complete = false;
   auto mutation_count = std::size_t{};
   auto proof_position = std::size_t{};
   for (auto batch_position = std::size_t{}; batch_position < response.blocks.size(); ++batch_position) {
      const auto& batch = response.blocks[batch_position];
      if (batch.anchor.block_num != position.block) {
         reject("Savanna table changes page has an unexpected block anchor");
      }

      auto expected = std::vector<protocol::table_mutation>{};
      while (!complete && position.block == batch.anchor.block_num && mutation_count < request.limit) {
         if (proof_position >= audit.state.size()) {
            reject("Savanna table changes response omits an authenticated range proof");
         }
         const auto& selector = request.tables[position.selector];
         const auto prefix = table_change_prefix(selector);
         const auto upper = prefix_upper_bound(prefix);
         const auto lower = position.key ? state_bytes(*position.key) : prefix;
         if (key_less(lower, prefix) || !key_less(lower, upper)) {
            reject("Savanna table changes cursor is outside its selected table");
         }

         const auto remaining = static_cast<std::uint32_t>(request.limit - mutation_count);
         const auto verified = verifier.verify_state_changes(batch.anchor,
                                                             authenticated_range_query{
                                                                 .range =
                                                                     {
                                                                         .lower = protocol_bytes(lower),
                                                                         .upper = protocol_bytes(upper),
                                                                     },
                                                                 .limit = remaining,
                                                             },
                                                             audit.state[proof_position++]);
         if (verified.changes.size() > remaining) {
            reject("Savanna table changes exceed the audited page limit");
         }
         expected.reserve(expected.size() + verified.changes.size());
         for (const auto& mutation : verified.changes) {
            auto projected = protocol::table_mutation{
                .table = selector,
                .primary = decode_primary_key(mutation.key, prefix, 1U),
            };
            if (mutation.value) {
               const auto value = unpack_source<commitment::primary_value>(*mutation.value);
               projected.row = protocol::table_row{.value = value.row, .payer = value.payer};
            }
            expected.push_back(std::move(projected));
         }
         mutation_count += verified.changes.size();
         if (verified.next_key) {
            position.key = verified.next_key;
            break;
         }
         complete = !advance_position(position, request.tables.size());
      }
      if (batch.mutations != expected) {
         reject("Savanna table changes are not the canonical authenticated projection");
      }
      const auto stopped = complete || mutation_count == request.limit || position.block == batch.anchor.block_num;
      if (stopped && batch_position + 1U != response.blocks.size()) {
         reject("Savanna table changes response contains batches after its canonical continuation");
      }
   }
   const auto expected_next =
       complete ? std::optional<protocol::bytes>{} : std::optional{encode_cursor(typed_change_kind::table, position)};
   if (response.next != expected_next || proof_position != audit.state.size()) {
      reject("Savanna table changes continuation is not bound to its authenticated range");
   }
}

std::shared_ptr<projection_verifier> make_contract_table_projection_verifier() {
   return std::make_shared<contract_table_projection_verifier>();
}

} // namespace forge::chain::api
