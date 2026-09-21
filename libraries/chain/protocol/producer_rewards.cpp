module;

#include <cstdint>
#include <limits>
#include <optional>

module forge.chain.protocol.producer_rewards;

namespace forge::chain::protocol {
namespace {

std::optional<time_point> next_claim_time(time_point last_claim_time) {
   constexpr auto claim_interval = days(1);
   constexpr auto first_claimable_offset = microseconds{1};
   if (last_claim_time.time_since_epoch().count() >
       std::numeric_limits<std::int64_t>::max() - claim_interval.count() - first_claimable_offset.count()) {
      return std::nullopt;
   }
   return last_claim_time + claim_interval + first_claimable_offset;
}

} // namespace

std::optional<producer_reward> project_producer_reward(const producer_info& system,
                                                        const std::optional<bpay_reward>& bpay, time_point anchor_time) {
   if (bpay && (bpay->owner != system.owner || !bpay->quantity.is_valid())) {
      return std::nullopt;
   }

   const auto next = next_claim_time(system.last_claim_time);
   if (!next) {
      return std::nullopt;
   }
   auto result = producer_reward{};
   result.producer = system.owner;
   result.system = {
       .eligible = system.is_active && anchor_time >= *next,
       .active = system.is_active,
       .unpaid_blocks = system.unpaid_blocks,
       .last_claim_time = system.last_claim_time,
       .next_claim_time = *next,
       .contract = account_name{"eosio"},
       .claim_action = claimrewards::get_name(),
   };
   result.bpay = {
       .contract = account_name{"eosio.bpay"},
       .claim_action = claimrewards::get_name(),
   };
   if (bpay) {
      result.bpay.claimable = bpay->quantity;
   }
   return result;
}

} // namespace forge::chain::protocol
