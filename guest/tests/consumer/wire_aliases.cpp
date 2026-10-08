#include <concepts>
#include <eosio/crypto.hpp>
#include <string_view>

import forge.chain.protocol.blockchain_parameters;
import forge.chain.protocol.call_access_mode;
import forge.chain.protocol.call_data_header;
import forge.chain.protocol.code_hash_result;
import forge.chain.protocol.finalizer_policy;
import forge.chain.protocol.hash_id;
import forge.chain.protocol.kv_parameters;
import forge.chain.protocol.system;
import forge.chain.protocol.types;
import forge.contract.action;
import forge.contract.call;
import forge.contract.crypto;
import forge.contract.deferred_transaction;
import forge.contract.hash_id;
import forge.contract.instant_finality;
import forge.contract.privileged;

static_assert(CONSUMER_DECLARATION_VALUE == 42);
static_assert(std::string_view{CONSUMER_DECLARATION_TEXT} == "quoted \"value\" with space");

static_assert(std::same_as<forge::contract::code_hash_result, forge::chain::protocol::code_hash_result>);
static_assert(std::same_as<forge::contract::blockchain_parameters, forge::chain::protocol::blockchain_parameters>);
static_assert(std::same_as<forge::contract::kv_parameters, forge::chain::protocol::kv_parameters>);
static_assert(std::same_as<forge::contract::finalizer_authority, forge::chain::protocol::finalizer_authority>);
static_assert(std::same_as<forge::contract::finalizer_policy, forge::chain::protocol::finalizer_policy>);
static_assert(std::same_as<forge::contract::hash_id, forge::chain::protocol::hash_id>);
static_assert(std::same_as<forge::contract::access_mode, forge::chain::protocol::call_access_mode>);
static_assert(std::same_as<forge::contract::call_data_header, forge::chain::protocol::call_data_header>);
static_assert(std::derived_from<forge::contract::onerror, forge::chain::protocol::onerror>);
static_assert(std::same_as<forge::contract::public_key, forge::chain::protocol::public_key>);
static_assert(std::same_as<forge::contract::signature, forge::chain::protocol::signature>);
static_assert(std::same_as<eosio::public_key, forge::chain::protocol::public_key>);
static_assert(std::same_as<eosio::signature, forge::chain::protocol::signature>);
