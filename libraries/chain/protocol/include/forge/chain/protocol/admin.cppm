module;

#include <boost/describe.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

export module forge.chain.protocol.admin;

export import forge.chain.protocol.account_ram_correction;
export import forge.chain.protocol.audit;
export import forge.chain.protocol.block;
export import forge.chain.protocol.finalizer_vote_record;
export import forge.chain.protocol.protocol_feature;
export import forge.crypto.bls.serialization;

import forge.raw.codec;
import forge.variant.containers;
import forge.variant.described;

export namespace forge::chain::protocol {

enum class list_update : std::uint8_t {
   add = 0,
   remove = 1,
   replace = 2,
};

struct admin_query {
   bool operator==(const admin_query&) const = default;
};

enum class operator_role : std::uint8_t {
   producer,
   finalizer,
};

struct operator_identity {
   account_name producer;
   public_key block_public_key;
   forge::crypto::bls::public_key finalizer_public_key;
   std::vector<operator_role> enabled_roles;

   bool operator==(const operator_identity&) const = default;
};

enum class node_lifecycle_state : std::uint8_t {
   starting,
   running,
   stopping,
   stopped,
   failed,
};

struct node_status {
   time_point process_start = time_point{};
   std::uint64_t uptime_ms = 0;
   node_lifecycle_state lifecycle = node_lifecycle_state::starting;

   bool operator==(const node_status&) const = default;
};

struct producer_runtime_options {
   std::optional<std::int32_t> max_transaction_time_ms;
   std::optional<std::int32_t> max_irreversible_block_age_seconds;
   std::optional<std::int32_t> produce_block_offset_ms;
   std::optional<std::int32_t> subjective_cpu_leeway_us;
   std::optional<std::uint32_t> greylist_limit;

   bool operator==(const producer_runtime_options&) const = default;
};

struct action_restriction {
   account_name account;
   action_name action;

   bool operator==(const action_restriction&) const = default;
};

struct producer_access_policy {
   std::vector<account_name> actor_whitelist;
   std::vector<account_name> actor_blacklist;
   std::vector<account_name> contract_whitelist;
   std::vector<account_name> contract_blacklist;
   std::vector<action_restriction> action_blacklist;
   std::vector<public_key> key_blacklist;

   bool operator==(const producer_access_policy&) const = default;
};

struct producer_status_response {
   bool paused = false;
   producer_runtime_options options;
   std::vector<account_name> greylist;
   producer_access_policy access;
   std::vector<digest> scheduled_protocol_features;

   bool operator==(const producer_status_response&) const = default;
};

struct finalizer_status {
   bool enabled = false;
   bool policy_member = false;
   std::optional<finalizer_vote_record> last_emitted_vote;

   bool operator==(const finalizer_status&) const = default;
};

struct supported_protocol_features_request {
   bool exclude_disabled = false;
   bool exclude_unactivatable = false;

   bool operator==(const supported_protocol_features_request&) const = default;
};

struct protocol_feature_subjective_restrictions {
   bool enabled = false;
   bool preactivation_required = false;
   time_point earliest_allowed_activation_time = time_point{};

   bool operator==(const protocol_feature_subjective_restrictions&) const = default;
};

struct supported_protocol_feature : protocol_feature {
   protocol_feature_subjective_restrictions subjective_restrictions = protocol_feature_subjective_restrictions{};

   bool operator==(const supported_protocol_feature&) const = default;
};

template <typename Stream> void raw_pack(Stream& stream, const supported_protocol_feature& value) {
   forge::raw::pack(stream, value.feature_digest);
   forge::raw::pack(stream, value.subjective_restrictions);
   forge::raw::pack(stream, value.description_digest);
   forge::raw::pack(stream, value.dependencies);
   forge::raw::pack(stream, value.protocol_feature_type);
   forge::raw::pack(stream, value.specification);
}

template <typename Stream> void raw_unpack(Stream& stream, supported_protocol_feature& value) {
   forge::raw::unpack(stream, value.feature_digest);
   forge::raw::unpack(stream, value.subjective_restrictions);
   forge::raw::unpack(stream, value.description_digest);
   forge::raw::unpack(stream, value.dependencies);
   forge::raw::unpack(stream, value.protocol_feature_type);
   forge::raw::unpack(stream, value.specification);
}

struct supported_protocol_features_response {
   std::vector<supported_protocol_feature> features;

   bool operator==(const supported_protocol_features_response&) const = default;
};

struct ram_corrections_request {
   std::optional<account_name> lower_bound;
   std::optional<account_name> upper_bound;
   std::uint32_t limit = 10;
   bool reverse = false;

   bool operator==(const ram_corrections_request&) const = default;
};

struct ram_corrections_response {
   std::vector<forge::chain::protocol::account_ram_correction> rows;
   std::optional<account_name> next;

   bool operator==(const ram_corrections_response&) const = default;
};

struct unapplied_transaction {
   transaction_id id;
   time_point_sec expiration;
   std::string source;
   account_name first_authorizer;
   account_name first_receiver;
   action_name first_action;
   std::uint16_t action_count = 0;
   std::uint32_t billed_cpu_time_us = 0;
   std::uint64_t packed_size = 0;

   bool operator==(const unapplied_transaction&) const = default;
};

struct unapplied_transactions_request {
   std::optional<transaction_id> lower_bound;
   std::uint32_t limit = 100;
   std::optional<std::uint32_t> time_limit_ms;

   bool operator==(const unapplied_transactions_request&) const = default;
};

struct unapplied_transactions_response {
   std::uint64_t size = 0;
   std::uint64_t incoming_size = 0;
   std::vector<unapplied_transaction> transactions;
   std::optional<transaction_id> next;

   bool operator==(const unapplied_transactions_response&) const = default;
};

struct producer_pause_request {
   bool paused = false;
   std::optional<std::uint32_t> at_block;

   bool operator==(const producer_pause_request&) const = default;
};

struct greylist_update_request {
   list_update operation = list_update::replace;
   std::vector<account_name> accounts;

   bool operator==(const greylist_update_request&) const = default;
};

struct snapshot_schedule_request {
   std::uint32_t block_spacing = 0;
   std::uint32_t start_block_num = 0;
   std::uint32_t end_block_num = 0;
   std::string description;

   bool operator==(const snapshot_schedule_request&) const = default;
};

struct snapshot_schedule_id {
   std::uint32_t id = 0;

   bool operator==(const snapshot_schedule_id&) const = default;
};

struct snapshot_schedule {
   std::uint32_t id = 0;
   snapshot_schedule_request request;
   std::vector<std::uint32_t> pending_blocks;

   bool operator==(const snapshot_schedule&) const = default;
};

struct snapshot_requests_response {
   std::vector<snapshot_schedule> schedules;

   bool operator==(const snapshot_requests_response&) const = default;
};

struct integrity_hash_response {
   block_id head;
   digest hash;

   bool operator==(const integrity_hash_response&) const = default;
};

struct push_block_response {
   block_id id;
   std::uint32_t num = 0;
   bool accepted = false;
   bool duplicate = false;

   bool operator==(const push_block_response&) const = default;
};

enum class snapshot_state : std::uint8_t {
   pending,
   completed,
};

struct snapshot_request {
   std::string request_id;
   std::string name;

   bool operator==(const snapshot_request&) const = default;
};

struct snapshot_status_request {
   std::string request_id;

   bool operator==(const snapshot_status_request&) const = default;
};

struct snapshot_lifecycle_response {
   std::string request_id;
   std::string name;
   snapshot_state state = snapshot_state::pending;
   std::optional<block_id> head;
   std::optional<std::uint32_t> head_num;

   bool operator==(const snapshot_lifecycle_response&) const = default;
};

struct snapshot_response {
   std::string name;
   block_id head;
   std::uint32_t head_num = 0;

   bool operator==(const snapshot_response&) const = default;
};

struct prune_request {
   std::uint32_t through_block = 0;
   std::uint32_t max_records = 4'096;

   bool operator==(const prune_request&) const = default;
};

struct prune_response {
   std::uint64_t records = 0;
   bool complete = false;

   bool operator==(const prune_response&) const = default;
};

BOOST_DESCRIBE_ENUM(list_update, add, remove, replace)
BOOST_DESCRIBE_ENUM(operator_role, producer, finalizer)
BOOST_DESCRIBE_ENUM(node_lifecycle_state, starting, running, stopping, stopped, failed)
BOOST_DESCRIBE_STRUCT(admin_query, (), ())
BOOST_DESCRIBE_STRUCT(operator_identity, (), (producer, block_public_key, finalizer_public_key, enabled_roles))
BOOST_DESCRIBE_STRUCT(node_status, (), (process_start, uptime_ms, lifecycle))
BOOST_DESCRIBE_STRUCT(producer_runtime_options, (),
                      (max_transaction_time_ms, max_irreversible_block_age_seconds, produce_block_offset_ms,
                       subjective_cpu_leeway_us, greylist_limit))
BOOST_DESCRIBE_STRUCT(action_restriction, (), (account, action))
BOOST_DESCRIBE_STRUCT(producer_access_policy, (),
                      (actor_whitelist, actor_blacklist, contract_whitelist, contract_blacklist, action_blacklist,
                       key_blacklist))
BOOST_DESCRIBE_STRUCT(producer_status_response, (), (paused, options, greylist, access, scheduled_protocol_features))
BOOST_DESCRIBE_STRUCT(finalizer_status, (), (enabled, policy_member, last_emitted_vote))
BOOST_DESCRIBE_STRUCT(supported_protocol_features_request, (), (exclude_disabled, exclude_unactivatable))
BOOST_DESCRIBE_STRUCT(protocol_feature_subjective_restrictions, (),
                      (enabled, preactivation_required, earliest_allowed_activation_time))
BOOST_DESCRIBE_STRUCT(supported_protocol_feature, (),
                      (feature_digest, subjective_restrictions, description_digest, dependencies, protocol_feature_type,
                       specification))
BOOST_DESCRIBE_STRUCT(supported_protocol_features_response, (), (features))
BOOST_DESCRIBE_STRUCT(ram_corrections_request, (), (lower_bound, upper_bound, limit, reverse))
BOOST_DESCRIBE_STRUCT(ram_corrections_response, (), (rows, next))
BOOST_DESCRIBE_STRUCT(unapplied_transaction, (),
                      (id, expiration, source, first_authorizer, first_receiver, first_action, action_count,
                       billed_cpu_time_us, packed_size))
BOOST_DESCRIBE_STRUCT(unapplied_transactions_request, (), (lower_bound, limit, time_limit_ms))
BOOST_DESCRIBE_STRUCT(unapplied_transactions_response, (), (size, incoming_size, transactions, next))
BOOST_DESCRIBE_STRUCT(producer_pause_request, (), (paused, at_block))
BOOST_DESCRIBE_STRUCT(greylist_update_request, (), (operation, accounts))
BOOST_DESCRIBE_STRUCT(snapshot_schedule_request, (), (block_spacing, start_block_num, end_block_num, description))
BOOST_DESCRIBE_STRUCT(snapshot_schedule_id, (), (id))
BOOST_DESCRIBE_STRUCT(snapshot_schedule, (), (id, request, pending_blocks))
BOOST_DESCRIBE_STRUCT(snapshot_requests_response, (), (schedules))
BOOST_DESCRIBE_STRUCT(integrity_hash_response, (), (head, hash))
BOOST_DESCRIBE_STRUCT(push_block_response, (), (id, num, accepted, duplicate))
BOOST_DESCRIBE_ENUM(snapshot_state, pending, completed)
BOOST_DESCRIBE_STRUCT(snapshot_request, (), (request_id, name))
BOOST_DESCRIBE_STRUCT(snapshot_status_request, (), (request_id))
BOOST_DESCRIBE_STRUCT(snapshot_lifecycle_response, (), (request_id, name, state, head, head_num))
BOOST_DESCRIBE_STRUCT(snapshot_response, (), (name, head, head_num))
BOOST_DESCRIBE_STRUCT(prune_request, (), (through_block, max_records))
BOOST_DESCRIBE_STRUCT(prune_response, (), (records, complete))

} // namespace forge::chain::protocol
