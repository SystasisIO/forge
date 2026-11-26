module;
#include <atomic>
#include <memory>

export module test.api_chain.counter_read;

import forge.api.core.registry;
import forge.chain.protocol.block_query;

export namespace demo {
forge::chain::protocol::block_header_response counter_genesis(forge::chain::protocol::chain_id chain,
                                                              forge::chain::protocol::time_point time);
struct read_counts {
   std::atomic_uint headers{};
   std::atomic_uint rows{};
};
void install_counter_reads(forge::api::core::installer apis, forge::chain::protocol::block_header_response genesis,
                           std::shared_ptr<read_counts> counts);
} // namespace demo
