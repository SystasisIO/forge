# Forge API Chain

`forge_api_chain` is the client-side API binding (package component
`api_chain`). It uses the existing typed Chain contracts and transaction
builder. A generated proxy builds an action locally. The peer sees the
canonical signed transaction in the existing Chain submission protocol.

A contract inherits the local Forge contract interface and declares a virtual
`const` action taking a request aggregate by value. Declare movable application
fields in their aggregate order and end the request with its builder reference.
The reference belongs to the action invocation. The implementation uses it to
add the action whose account, authorization and argument order match the deployed
contract.

```cpp
module;

#include <cstdint>
#include <forge/api/chain/macros.hpp>

export module example.counter;

import forge.api.chain.binding;

export namespace example {

struct increment_request {
   std::uint64_t expected;
   forge::chain::transaction::builder& transaction;
};

class counter : public forge::api::core::contract<counter> {
 public:
   virtual ~counter() = default;
   virtual void increment(increment_request request) const = 0;
};

} // namespace example

FORGE_CHAIN_API(example::counter, FORGE_CHAIN_ACTION(increment, expected))
```

Install `counter_impl` as `provider.install<example::counter>(implementation)`.
The registry descriptor has local surface only. The API identifier in this
example is `example.counter`, version `1.0`.

```cpp
auto target = forge::api::chain::http_endpoint{"primary"};
auto counter = co_await forge::api::chain::remote<example::counter>(context.apis(), target);
auto id = co_await counter->increment(7);
auto verified = co_await forge::api::chain::status(context.apis(), target, id);
```

Pass a P2P `peer_id` to `remote` and `status` to use P2P. An HTTP endpoint name
selects a configured URL for the same existing Chain query and submission
services. Both transports use the same generated contract proxy.

An action returns its submitted transaction ID after acceptance. If final
status is pending, query the same ID again. An optional third argument to
`remote` accepts `forge::api::core::interceptor_step` values. Before dispatch,
interceptors can inspect an immutable tuple of the action arguments through
`call_context.request`. Completion exposes the accepted transaction ID. The
request view borrows its value for the duration of the callback. Terminal
interceptor errors are logged while the action still returns its known ID or
propagates its original failure.

The outbox keeps the immutable signed packet, chain and transaction IDs, destination,
delivery policy identity and recovery state in ObjectDB. The API never
re-signs a saved transaction on recovery. Application policy can select replay
of the same packet or reconciliation only after a possible send. Verified
finalized status includes the chain receipt. Applications own any subsequent
contract-specific decision. See [the outbox plugin](../../../plugins/chain/outbox/README.md)
for database and transport composition.

This API and its stored record format are preview. The embedded test is
enabled by `FORGE_ENABLE_CHAIN_API_TESTS`.
