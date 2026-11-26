module;

#include <boost/describe.hpp>

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

export module forge.chain.protocol.state_query;

export import forge.chain.protocol.account_authority;
export import forge.chain.protocol.code;
export import forge.chain.protocol.currency_stats;
export import forge.chain.protocol.entity_selector;
export import forge.chain.protocol.full_account;
export import forge.chain.protocol.generated_transaction;
export import forge.chain.protocol.permission_link;
export import forge.chain.protocol.table;

export import forge.chain.protocol.audit;

export namespace forge::chain::protocol {

// Invoke the deployed contract's synchronous read-only ABI on a finalized block.
struct contract_call_request {
   block_id block;
   account_name contract;
   bytes data;
};

struct contract_call_response {
   state_anchor anchor;
   std::optional<bytes> value;
};

struct account_request : account_selector {
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const account_request&) const = default;
};

struct account_response : audited_response {
   full_account account;

   bool operator==(const account_response&) const = default;
};

struct code_request : account_selector {
   bool include_wasm = true;
   bool include_abi = true;
   std::optional<digest> known_abi_hash;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const code_request&) const = default;
};

struct code_response : audited_response {
   forge::chain::protocol::code code;
   std::optional<bytes> wasm;
   std::optional<bytes> abi;

   bool operator==(const code_response&) const = default;
};

struct permission_links_request : account_selector {
   std::optional<account_name> code;
   std::optional<action_name> message_type;
   std::uint32_t limit = 10;
   std::optional<bytes> cursor;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const permission_links_request&) const = default;
};

struct permission_links_response : audited_response {
   std::vector<permission_link> links;
   std::optional<bytes> next;

   bool operator==(const permission_links_response&) const = default;
};

enum class table_index_kind : std::uint8_t {
   primary = 0,
   secondary_u64 = 1,
   secondary_u128 = 2,
   secondary_u256 = 3,
   secondary_f64 = 4,
   secondary_f128 = 5,
};

struct table_index {
   table_index_kind kind = table_index_kind::primary;
   std::uint8_t position = 0;

   [[nodiscard]] static table_index from_string(std::string_view value);
   [[nodiscard]] std::string to_string() const;

   bool operator==(const table_index&) const = default;
};

struct table_row {
   bytes value;
   std::optional<forge::chain::protocol::account_name> payer;

   bool operator==(const table_row&) const = default;
};

struct table_rows_request {
   forge::chain::protocol::account_name code;
   forge::chain::protocol::name scope;
   forge::chain::protocol::table_name table;
   table_index index;
   std::optional<bytes> lower_bound;
   std::optional<bytes> upper_bound;
   std::optional<bytes> cursor;
   std::uint32_t limit = 10;
   bool reverse = false;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const table_rows_request&) const = default;
};

struct table_rows_response : audited_response {
   std::vector<table_row> rows;
   std::optional<bytes> next;

   bool operator==(const table_rows_response&) const = default;
};

struct table_change_selector {
   forge::chain::protocol::account_name code;
   forge::chain::protocol::name scope;
   forge::chain::protocol::table_name table;

   constexpr auto operator<=>(const table_change_selector&) const = default;
};

struct table_mutation {
   table_change_selector table;
   std::uint64_t primary = 0;
   std::optional<table_row> row;

   bool operator==(const table_mutation&) const = default;
};

struct table_change_batch {
   state_anchor anchor;
   std::vector<table_mutation> mutations;

   bool operator==(const table_change_batch&) const = default;
};

struct table_changes_request {
   std::uint32_t from_block = 0;
   std::uint32_t to_block = 0;
   std::vector<table_change_selector> tables;
   std::uint32_t limit = 256;
   std::optional<bytes> cursor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const table_changes_request&) const = default;
};

struct table_changes_response : audited_response {
   std::vector<table_change_batch> blocks;
   std::optional<bytes> next;

   bool operator==(const table_changes_response&) const = default;
};

struct account_mutation {
   forge::chain::protocol::account_name account;
   std::optional<account_authority> authority;

   bool operator==(const account_mutation&) const = default;
};

struct account_change_batch {
   state_anchor anchor;
   std::vector<account_mutation> mutations;

   bool operator==(const account_change_batch&) const = default;
};

struct account_changes_request {
   std::uint32_t from_block = 0;
   std::uint32_t to_block = 0;
   std::vector<forge::chain::protocol::account_name> accounts;
   std::uint32_t limit = 256;
   std::optional<bytes> cursor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const account_changes_request&) const = default;
};

struct account_changes_response : audited_response {
   std::vector<account_change_batch> blocks;
   std::optional<bytes> next;

   bool operator==(const account_changes_response&) const = default;
};

struct table_scope_request {
   forge::chain::protocol::account_name code;
   forge::chain::protocol::table_name table;
   std::string lower_bound;
   std::string upper_bound;
   std::uint32_t limit = 10;
   bool reverse = false;
   std::optional<bytes> cursor;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const table_scope_request&) const = default;
};

struct table_scope_response : audited_response {
   std::vector<table> tables;
   std::optional<bytes> next;

   bool operator==(const table_scope_response&) const = default;
};

struct currency_balance_request {
   forge::chain::protocol::account_name code;
   forge::chain::protocol::account_name account;
   std::optional<forge::chain::protocol::symbol_code> symbol;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const currency_balance_request&) const = default;
};

struct currency_stats_request {
   forge::chain::protocol::account_name code;
   forge::chain::protocol::symbol_code symbol{};
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const currency_stats_request&) const = default;
};

struct currency_balance_response : audited_response {
   std::vector<asset> balances;

   bool operator==(const currency_balance_response&) const = default;
};

struct currency_stats_response : audited_response {
   forge::chain::protocol::currency_stats stats;

   bool operator==(const currency_stats_response&) const = default;
};

struct scheduled_request {
   std::optional<time_point> lower_bound;
   std::optional<time_point> upper_bound;
   std::uint32_t limit = 50;
   std::optional<bytes> cursor;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const scheduled_request&) const = default;
};

struct scheduled_response : audited_response {
   std::vector<generated_transaction> transactions;
   std::optional<bytes> next;

   bool operator==(const scheduled_response&) const = default;
};

struct authorizers_request {
   std::vector<forge::chain::protocol::permission_level> accounts;
   std::vector<forge::chain::protocol::public_key> keys;
   std::uint32_t limit = 256;
   std::optional<bytes> cursor;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const authorizers_request&) const = default;
};

struct authorizer_match {
   forge::chain::protocol::account_name account_name;
   forge::chain::protocol::permission_name permission_name;
   std::optional<forge::chain::protocol::permission_level> authorizing_account;
   std::optional<forge::chain::protocol::public_key> authorizing_key;
   forge::chain::protocol::weight weight = 0;
   std::uint32_t threshold = 0;

   bool operator==(const authorizer_match&) const = default;
};

struct authorizers_response : audited_response {
   std::vector<authorizer_match> accounts;
   std::optional<bytes> next;

   bool operator==(const authorizers_response&) const = default;
};

BOOST_DESCRIBE_STRUCT(account_request, (account_selector), (anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(account_response, (audited_response), (account))
BOOST_DESCRIBE_STRUCT(code_request, (account_selector),
                      (include_wasm, include_abi, known_abi_hash, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(code_response, (audited_response), (code, wasm, abi))
BOOST_DESCRIBE_STRUCT(contract_call_request, (), (block, contract, data))
BOOST_DESCRIBE_STRUCT(contract_call_response, (), (anchor, value))
BOOST_DESCRIBE_STRUCT(permission_links_request, (account_selector),
                      (code, message_type, limit, cursor, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(permission_links_response, (audited_response), (links, next))
BOOST_DESCRIBE_ENUM(table_index_kind, primary, secondary_u64, secondary_u128, secondary_u256, secondary_f64,
                    secondary_f128)
BOOST_DESCRIBE_STRUCT(table_index, (), (kind, position))
BOOST_DESCRIBE_STRUCT(table_row, (), (value, payer))
BOOST_DESCRIBE_STRUCT(table_rows_request, (),
                      (code, scope, table, index, lower_bound, upper_bound, cursor, limit, reverse, anchor,
                       finality_from, audit))
BOOST_DESCRIBE_STRUCT(table_rows_response, (audited_response), (rows, next))
BOOST_DESCRIBE_STRUCT(table_change_selector, (), (code, scope, table))
BOOST_DESCRIBE_STRUCT(table_mutation, (), (table, primary, row))
BOOST_DESCRIBE_STRUCT(table_change_batch, (), (anchor, mutations))
BOOST_DESCRIBE_STRUCT(table_changes_request, (), (from_block, to_block, tables, limit, cursor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(table_changes_response, (audited_response), (blocks, next))
BOOST_DESCRIBE_STRUCT(account_mutation, (), (account, authority))
BOOST_DESCRIBE_STRUCT(account_change_batch, (), (anchor, mutations))
BOOST_DESCRIBE_STRUCT(account_changes_request, (),
                      (from_block, to_block, accounts, limit, cursor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(account_changes_response, (audited_response), (blocks, next))
BOOST_DESCRIBE_STRUCT(table_scope_request, (),
                      (code, table, lower_bound, upper_bound, limit, reverse, cursor, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(table_scope_response, (audited_response), (tables, next))
BOOST_DESCRIBE_STRUCT(currency_balance_request, (), (code, account, symbol, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(currency_stats_request, (), (code, symbol, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(currency_balance_response, (audited_response), (balances))
BOOST_DESCRIBE_STRUCT(currency_stats_response, (audited_response), (stats))
BOOST_DESCRIBE_STRUCT(scheduled_request, (), (lower_bound, upper_bound, limit, cursor, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(scheduled_response, (audited_response), (transactions, next))
BOOST_DESCRIBE_STRUCT(authorizers_request, (), (accounts, keys, limit, cursor, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(authorizer_match, (),
                      (account_name, permission_name, authorizing_account, authorizing_key, weight, threshold))
BOOST_DESCRIBE_STRUCT(authorizers_response, (audited_response), (accounts, next))

} // namespace forge::chain::protocol
