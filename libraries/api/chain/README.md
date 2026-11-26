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

A typed read receives a scoped `const forge::api::chain::read_snapshot& state`
as the final field of its request aggregate and returns `awaitable<Result>`.
Declare it with `FORGE_CHAIN_READ(find, key)` in the same contract macro.
The caller selects the block for the entire read.

```cpp
auto block = co_await forge::api::chain::finalized(context.apis(), target);
auto value = co_await counter->find(block, key);
auto id = co_await counter->increment(7, forge::api::chain::action_options{block, expiration});
```

The snapshot proves the canonical header at the chosen block. Its `time()`
comes from that header. Its `get_table_rows` and `get_code` use the configured
projection verifier for all contract data at that anchor. An unsupported
projection or invalid proof fails the call. Passing action options signs
against the same verified block and the caller's absolute expiration.

Pass a borrowed read session to group calls at one block. Each method runs its
interceptors and checks the destination and verification profile of the session.

```cpp
co_await forge::api::chain::read(context.apis(), target, block,
   [&](const forge::api::chain::read_session& session) -> boost::asio::awaitable<void> {
      first = co_await counter->find(session, first_key);
      second = co_await counter->find(session, second_key);
   });
```

The callback owns the lifetime of its verified reads. For repeated checks of the
same contract at that block, the implementation can keep a private
`read_snapshot::check` for one rule. Call `state.confirm(rule)` after verifying
the evidence. Later methods can test `state.verified(rule)` before repeating
that verification. Each snapshot retains one confirmed rule.

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

Set `outbox::options::prepare_first_send` to authorize the first possible send
of a staged packet. The callback receives the retained packet and destination
before the outbox opens a writer, including after restart. It returns a check
that uses the outbox writer for local validation and writes. The check must
leave commit and rollback to the outbox and keep network I/O in preparation.

- `proceed` commits the application's writes with the possible-send marker
  before the transport is called.
- `defer` rolls back tentative application writes and schedules another
  check within the packet's lifetime.
- `deny` commits the application's writes and closes the staged packet with
  no send. A submit call reports `admission_rejected` for that retained packet.

The outbox calls this check before the first possible send. Once a possible
send is recorded, recovery follows the packet's retained replay policy.
Applications reconstruct preparation from the saved packet on each recovery.

This API and its stored record format are preview. The embedded test is
enabled by `FORGE_ENABLE_CHAIN_API_TESTS`.
