module;

#include <boost/describe.hpp>

#include <optional>

export module forge.chain.savanna.producer_opportunity;

import forge.chain.protocol.types;

export namespace forge::chain::savanna {

struct producer_opportunity {
   forge::chain::protocol::block_timestamp timestamp;
   forge::chain::protocol::account_name expected_producer;
   std::optional<forge::chain::protocol::block_id> produced_block;
   std::optional<forge::chain::protocol::block_num> produced_block_num;

   bool operator==(const producer_opportunity&) const = default;
};

BOOST_DESCRIBE_STRUCT(producer_opportunity, (), (timestamp, expected_producer, produced_block, produced_block_num))

} // namespace forge::chain::savanna
