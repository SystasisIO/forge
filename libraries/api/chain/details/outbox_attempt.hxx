#pragma once

namespace forge::api::chain::detail {

enum class delivery_state : std::uint8_t {
   staged = 0,
   possible = 1,
   acknowledged = 2,
   finalized = 3,
   rejected = 4,
   expired = 5,
   unresolved = 6,
};

struct outbox_attempt : forge::db::object::object<outbox_attempt, 82, 1> {
   forge::chain::protocol::chain_id chain;
   forge::chain::protocol::transaction_id transaction;
   std::string peer;
   forge::chain::protocol::packed_transaction packed;
   outbox::replay mode = outbox::replay::reconcile_only;
   std::string policy;
   std::uint32_t policy_version = 1;
   std::uint64_t expires = 0;
   std::uint64_t next_check = 0;
   std::uint64_t revision = 0;
   std::uint32_t sends = 0;
   delivery_state state = delivery_state::staged;
   std::optional<forge::chain::protocol::transaction_status_response> final;

   bool pending() const noexcept { return state <= delivery_state::acknowledged; }
};

struct by_id;
struct by_transaction;
struct by_due;

using outbox_attempt_index = forge::db::object::object_index<
   outbox_attempt,
   forge::db::object::indexed_by<
      forge::db::object::primary_unique<by_id>,
      forge::db::object::ordered_unique<by_transaction,
         forge::db::object::composite_key<
            forge::db::object::member<&outbox_attempt::chain>,
            forge::db::object::member<&outbox_attempt::transaction>
         >
      >,
      forge::db::object::ordered_non_unique<by_due,
         forge::db::object::composite_key<
            forge::db::object::const_mem_fun<&outbox_attempt::pending>,
            forge::db::object::member<&outbox_attempt::next_check>
         >
      >
   >
>;

BOOST_DESCRIBE_ENUM(delivery_state, staged, possible, acknowledged, finalized, rejected, expired, unresolved)
BOOST_DESCRIBE_STRUCT(outbox_attempt, (forge::db::object::object<outbox_attempt, 82, 1>),
                      (chain, transaction, peer, packed, mode, policy, policy_version, expires, next_check, revision,
                       sends, state, final))

} // namespace forge::api::chain::detail

FORGE_DB_OBJECT(forge::api::chain::detail::outbox_attempt_index)
