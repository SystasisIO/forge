module;

#include <boost/asio/awaitable.hpp>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

module test.api_chain.counter_read;

import package.chain_api_component.read_fixture;
import forge.chain.api.block;
import forge.chain.api.state;
import forge.chain.api.table_key;
import forge.chain.protocol.contract_commitment;
import forge.db.authenticated.codec;
import forge.db.authenticated.hash;
import forge.raw.codec;

namespace demo {
namespace protocol = forge::chain::protocol;
namespace api = forge::chain::api;
namespace db = forge::db::authenticated;
namespace {
const protocol::contract_table_location location{protocol::account_name{"counter"}.value,
                                                 protocol::name{"counter"}.value, protocol::table_name{"counts"}.value};

db::proof_leaf counter_leaf() {
   const auto binary = forge::raw::pack(
       protocol::primary_value{.payer = protocol::account_name{"counter"}, .row = forge::raw::pack(std::uint64_t{7})});
   const auto bytes = std::as_bytes(std::span{binary});
   return {.key = protocol::contract_primary_key(location, protocol::contract_table_family::primary, 1),
           .value_hash = db::hash_value(bytes),
           .value = db::bytes{bytes.begin(), bytes.end()}};
}

class blocks final : public package_chain_api_component::block_implementation {
 public:
   blocks(protocol::block_header_response genesis, std::shared_ptr<read_counts> counts)
       : block_implementation({}), _genesis(std::move(genesis)), _counts(std::move(counts)) {}
   boost::asio::awaitable<protocol::block_header_response> get_header(protocol::block_request) override {
      ++_counts->headers;
      co_return _genesis;
   }

 private:
   protocol::block_header_response _genesis;
   std::shared_ptr<read_counts> _counts;
};

class counters final : public package_chain_api_component::state_implementation {
 public:
   counters(protocol::block_header_response genesis, std::shared_ptr<read_counts> counts)
       : state_implementation({}), _genesis(std::move(genesis)), _counts(std::move(counts)) {}
   boost::asio::awaitable<protocol::table_rows_response> get_table_rows(protocol::table_rows_request request) override {
      ++_counts->rows;
      if (request.anchor != _genesis.id || request.audit != protocol::audit_mode::required ||
          request.lower_bound != request.upper_bound || !request.lower_bound || request.limit != 1) {
         throw std::logic_error("Counter requires one key at the audited genesis block");
      }
      // Key 3 deliberately gets the neighbouring proof, so the client can exercise rejection.
      const auto key = request.lower_bound == api::encode_table_key(std::uint64_t{1}) ? 1U : 2U;
      const auto leaf = counter_leaf();
      const auto lower = protocol::contract_primary_key(location, protocol::contract_table_family::primary, key);
      auto upper = lower;
      upper.back() = static_cast<std::byte>(std::to_integer<unsigned>(upper.back()) + 1U);
      const auto& anchor = *_genesis.context.anchor;
      const auto proof = db::range_proof{
          .anchor = {.version = anchor.block_num, .state_root = anchor.state_root, .state_size = anchor.state_size},
          .tree = db::proof_tree::state,
          .request = {.lower = lower, .upper = upper, .limit = 1, .include_values = true},
          .nodes = {leaf}};
      const auto encoded = db::encode(proof);
      const auto wire = std::span{reinterpret_cast<const std::uint8_t*>(encoded.data()), encoded.size()};
      protocol::table_rows_response result;
      result.context = _genesis.context;
      result.audit = _genesis.audit;
      result.audit->state.push_back(
          {.scheme = "forge.db.authenticated.range", .version = 1, .payload = {wire.begin(), wire.end()}});
      if (key == 1) {
         result.rows.push_back(
             {.value = forge::raw::pack(std::uint64_t{7}), .payer = protocol::account_name{"counter"}});
      }
      co_return result;
   }

 private:
   protocol::block_header_response _genesis;
   std::shared_ptr<read_counts> _counts;
};
} // namespace

protocol::block_header_response counter_genesis(protocol::chain_id chain, protocol::time_point time) {
   protocol::block_header_response result;
   result.header.timestamp = time;
   result.id = result.header.calculate_id();
   result.num = result.header.calculate_block_num();
   result.canonical = true;
   const auto leaf = counter_leaf();
   const auto domain = db::canonical_tree_domain("forge.test.counter", db::proof_tree::state);
   protocol::state_anchor anchor{.chain = chain,
                                 .block = result.id,
                                 .block_num = result.num,
                                 .state_root = db::hash_leaf(domain, leaf.key, leaf.value_hash),
                                 .state_size = 1};
   result.context = {chain, result.id, result.id, anchor};
   result.audit = protocol::audit_bundle{.finality = protocol::proof_blob{.scheme = "fixture.counter"}};
   return result;
}

void install_counter_reads(forge::api::core::installer apis, protocol::block_header_response genesis,
                           std::shared_ptr<read_counts> counts) {
   apis.install<api::block>(std::make_shared<blocks>(genesis, counts));
   apis.install<api::state>(std::make_shared<counters>(std::move(genesis), std::move(counts)));
}
} // namespace demo
