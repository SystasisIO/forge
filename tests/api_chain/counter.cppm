module;

#include <cstdint>
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

class counter : public forge::api::core::contract<counter> {
 public:
   virtual ~counter() = default;
   virtual void increment(increment_request request) const = 0;
};

} // namespace demo

FORGE_CHAIN_API(demo::counter, FORGE_CHAIN_ACTION(increment, expected))
