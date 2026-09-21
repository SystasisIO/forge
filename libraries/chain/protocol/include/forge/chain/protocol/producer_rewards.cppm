module;

#include <boost/describe.hpp>

#include <cstdint>
#include <optional>

export module forge.chain.protocol.producer_rewards;

export import forge.chain.protocol.audit;
export import forge.chain.protocol.block;
export import forge.chain.protocol.producer_info;
export import forge.chain.protocol.state_query;
export import forge.chain.protocol.system;

export namespace forge::chain::protocol {

struct bpay_reward {
   account_name owner;
   asset quantity;

   bool operator==(const bpay_reward&) const = default;
};

struct system_reward {
   bool eligible = false;
   bool active = false;
   std::uint32_t unpaid_blocks = 0;
   time_point last_claim_time{};
   time_point next_claim_time{};
   account_name contract;
   action_name claim_action;

   bool operator==(const system_reward&) const = default;
};

struct bpay_claim {
   std::optional<asset> claimable;
   account_name contract;
   action_name claim_action;

   bool operator==(const bpay_claim&) const = default;
};

struct producer_reward {
   account_name producer;
   system_reward system;
   bpay_claim bpay;

   bool operator==(const producer_reward&) const = default;
};

[[nodiscard]] std::optional<producer_reward> project_producer_reward(const producer_info& system,
                                                                       const std::optional<bpay_reward>& bpay,
                                                                       time_point anchor_time);

struct producer_rewards_request {
   account_name producer;
   std::optional<block_id> anchor;
   std::optional<block_id> finality_from;
   audit_mode audit = audit_mode::none;

   bool operator==(const producer_rewards_request&) const = default;
};

struct producer_rewards_response : audited_response {
   block_header anchor_header;
   producer_reward reward;
   table_rows_response system;
   table_rows_response bpay;
};

BOOST_DESCRIBE_STRUCT(bpay_reward, (), (owner, quantity))
BOOST_DESCRIBE_STRUCT(system_reward,
                      (),
                      (eligible, active, unpaid_blocks, last_claim_time, next_claim_time, contract, claim_action))
BOOST_DESCRIBE_STRUCT(bpay_claim, (), (claimable, contract, claim_action))
BOOST_DESCRIBE_STRUCT(producer_reward, (), (producer, system, bpay))
BOOST_DESCRIBE_STRUCT(producer_rewards_request, (), (producer, anchor, finality_from, audit))
BOOST_DESCRIBE_STRUCT(producer_rewards_response, (audited_response), (anchor_header, reward, system, bpay))

} // namespace forge::chain::protocol
