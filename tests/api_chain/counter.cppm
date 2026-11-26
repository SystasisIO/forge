module;

#include <cstdint>
#include <optional>
#include <forge/api/chain/macros.hpp>

export module test.api_chain.counter;

import forge.api.chain.binding;
import forge.api.core.types;
import forge.chain.transaction.builder;

export namespace demo {

struct increment_request {
   std::uint64_t expected;
   forge::chain::transaction::builder& transaction;
};

struct find_request {
   std::uint64_t key;
   const forge::api::chain::read_snapshot& state;
};

class counter : public forge::api::core::contract<counter> {
 public:
   virtual ~counter() = default;
   virtual boost::asio::awaitable<std::optional<std::uint64_t>> find(find_request request) const = 0;
   virtual void increment(increment_request request) const = 0;
};

} // namespace demo

FORGE_CHAIN_API(demo::counter, FORGE_CHAIN_ACTION(increment, expected), FORGE_CHAIN_READ(find, key))
