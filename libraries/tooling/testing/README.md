# Contract Testing

`forge_tooling_testing` is the executable oracle for the Contract SDK host
interface. It is an installed Forge library in namespace
`forge::tooling::testing` and is exported as the `tooling_testing`
component.

## Usage

```cmake
find_package(Forge CONFIG REQUIRED COMPONENTS tooling_testing)

add_executable(contract_state_tests contract_state_tests.cpp)
target_link_libraries(
   contract_state_tests
   PRIVATE Forge::forge_tooling_testing
)
```

```cpp
import forge.tooling.testing.host;
import forge.tooling.testing.state;

auto host = forge::tooling::testing::host{};
host.configure(forge::tooling::testing::oracle_state{});
```

## Responsibilities

- Register all 152 public imports, 48 runtime imports and optional capability
  entries from the canonical intrinsic registry without a
  second hand-written registration list.
- Execute a contract invocation with one `forge::db::object::transaction`.
  `host::transact(body)` can group explicitly driven invocations into the same
  database/oracle transaction: every child must succeed before commit. Any
  failed child, including a preflight timeout/cancellation, poisons the group
  even when the callback catches its exception. Nested groups and reentrant
  invocations are rejected.
- Between grouped invocations, `configure` may update deterministic caller
  context; start from `state()` to preserve already recorded effects. Read
  helpers use the active transaction; raw `snapshot()` is rejected until the
  group has committed or rolled back. Contract registration is setup-only.
- Commit successful `apply` and `eosio_exit`; roll back assertions, DB errors
  and VM traps.
- Implement Spring iterator and table behavior for the primary table and all
  five secondary-index families.
- Expose independent read helpers so tests inspect committed ObjectDB state
  without trusting the invocation-local iterator cache.
- Provide deterministic invocation state for authorization, accounts,
  transactions, producers, protocol features, resource limits and finality.
- Execute hashing, key recovery, extended crypto, BLS, synchronous calls,
  console output, inline actions and deferred transactions through real host
  callbacks.

The seven schema models are direct `forge::db::object::object` values. Ranked
ObjectDB indexes own uniqueness, ordering, updates and rollback. The test-local
memory driver implements only ordered `forge::db::core` records; it has no
knowledge of Spring tables, secondary keys or iterators.

## Floating And 256-bit Keys

Double and long-double keys use the SoftFloat implementation already shipped
inside `forge.vm.wasm.interpret`. NaN is rejected before a DB query or mutation, and
signed zero maps to one ObjectDB sort key. `idx256` uses
`forge::chain::protocol::key256`; its legacy two-word pointer is handled by the
VM argument proxy so unaligned donor inputs are copied safely.

## Boundaries

This library is not a blockchain runtime. Its authorization and privileged
state are deterministic fixtures, not a controller or consensus model. It does
not implement RAM billing, fork choice, controller lifecycle, persistent schema
migration or a public memory database. Product host bindings must provide their
own policy, state models and production Forge DB drivers.

## Optional Expression Capability

When Forge is built with `FORGE_ENABLE_EXPRESSION=ON`, the existing host can
borrow a caller-owned `forge::expression::runtime`. The default constructor
keeps expression execution disabled and reports typed
`expression_unavailable` when an expression import is called.

```cpp
import forge.expression.program;
import forge.tooling.testing.host;

forge::expression::runtime expressions({.helper = helper_executable});
forge::tooling::testing::host host(expressions);
```

Keep `expressions` alive until the host and every invocation have finished.
Invoke the synchronous test host on a host thread, outside Forge Asio IO
threads. The host borrows the existing bounded worker. Compile returns a
portable artifact; import rechecks it; evaluation imports it into a temporary
native program object. Destruction uses the runtime's existing bounded
asynchronous retirement. No native program ID is stored in contract state. Owner-local
`forge.tooling.testing.conversion` maps portable activation values to the
existing Forge variant types.

The additive `invoke(..., data, invocation_options)` overload accepts an
absolute steady-clock deadline and a stop token. Its effective deadline is
the earlier of the caller's deadline and the host timeout. The same deadline
and cancellation reach every native CEL operation, including queued work;
the worker's own timeout remains an additional ceiling. Timeout or cancellation
after a request is sent terminates and joins the worker's cleanup. Typed CEL
errors cross the WAMR boundary and roll back the invocation transaction
together with fixture state.

Inline and deferred actions are recorded fixture effects. They do not prove
execution of another contract or a financial transaction cycle.

## Tests

`guest/tests/e2e/contract_tests.cpp` runs real generated wasm32 contracts
through `forge.vm.wasm.interpret`. The database cases cover all 60 DB imports, C++23
`multi_index` and `singleton`, traversal, transaction commit/rollback, iterator
errors, payer and ownership checks, NaN, and unaligned `idx256` input. The full
oracle contract executes every non-database capability family, verifies
observable side effects and proves that assertion failures roll back ObjectDB
and fixture state together. A malformed-pointer case verifies that guest ranges
are rejected before a host callback touches memory.

`tests/tooling_testing/expression_tests.cpp` provides a literal WASM fixture
that calls the three expression imports through the same host. It covers
artifact transfer across worker restart, exact Raw response bytes, malformed
requests, untrusted artifacts, pointer bounds, typed failures and DB rollback.
Stopped real worker cases exercise shared deadlines and cancellation inside a
host import. The independent compute profile retains its five allowed imports.

Grouped invocations do not automatically deliver notifications or inline actions.
A fixture must explicitly drive the known real-WASM action sequence and check
recorded effects. This qualifies transaction rollback, not chain authority
resolution, consensus, or production token custody.
