# Guest Contract

`forge_guest_contract` is the modern C++23 contract API for wasm32. The primary
module `forge.contract` owns `forge::contract::context` and the core contract API.
Focused modules are `forge.contract.intrinsics`, `forge.contract.dispatcher`,
`forge.contract.multi_index` and `forge.contract.singleton`; there is no
aggregate-only module.

```cpp
import forge.contract;

class [[forge::contract("hello")]] hello : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] std::uint32_t add(std::uint32_t a, std::uint32_t b) {
      return a + b;
   }
};
```

Arguments and non-void results use the shared guest lane of `forge.raw`.
Results are returned through `set_action_return_value`. `intrinsics.hpp` is the
only signature registry and generates modern imports, EOSIO C declarations,
the host binding skeleton and the approved-import manifest. Blockchain host
implementations do not live here.

`forge.contract.datastream` exposes the canonical `forge.raw` stream and codec.
The raw codec owns CDT-compatible aggregate and fixed C-array traversal; the
EOSIO compatibility header only aliases the same `forge::datastream`.

The installed C surface, including `<forge/contract/types.h>` and
`<forge/contract/intrinsics.h>`, is generated during SDK assembly. C ABI records
come from `guest/cmake/types.h.in`; generated `.h` files are not source headers
of this module-first library and contain no C++ implementation.

Interface version 1 includes the complete 152-function CDT/Spring union.
Contract C code may include `<forge/contract/intrinsics.h>` or one of the thin
EOSIO compatibility headers such as `<eosio/db.h>`.
`forge.contract.multi_index` and `forge.contract.singleton` are the production
C++23 API over those declarations; they do not link host Forge DB code into a
contract.

## Optional CEL Expressions

`forge.contract.expression` exposes the optional `expression_v1` host
capability. It uses three imports from the same canonical registry:
`expression_compile_v1`, `expression_import_v1` and `expression_evaluate_v1`.
The existing 152 public CDT/Spring imports and 48 compiler/runtime imports
retain their signatures. Generated `forge::contract::host::interface` retains
the required baseline; the separate `host::expression_v1` interface describes
the optional extension. A host must explicitly provide CEL support.

```cpp
#include <cstdint>

import forge.contract.expression;

auto program = forge::contract::expression::compile({
   .source = "x + 1",
   .variables = {{"x", forge::expression::type::int64}}
});
auto checked = forge::contract::expression::load(program);
auto result = forge::contract::expression::evaluate({
   .program = checked,
   .inputs = {{"x", std::int64_t{41}}}
});
```

`program` is a portable Raw artifact suitable for a contract table. It contains
versioned checked CEL bytes and a digest, with no process-local program handle.
Loading rechecks its CEL tree, types and references in the host; its digest is
an identity check, not evidence that a supplied artifact is safe. The SDK does
not include CEL, a parser, an evaluator, or the native worker in guest code.
Requests and responses use the shared `forge.expression.artifact` DTOs and
Forge Raw. The finite standard `std::variant` alternatives preserve signed and
unsigned integers without a second serialization format. The wire limit is
128 KiB; output storage belongs to the caller. Host failures terminate the
invocation through the host's typed error path.

```cpp
#include <forge/contract/serialize.hpp>

import forge.chain.protocol.values;
import forge.contract.multi_index;

using namespace forge::chain::protocol::literals;

struct [[forge::table("accounts")]] account {
   std::uint64_t id = 0;
   forge::chain::protocol::name owner;

   std::uint64_t primary_key() const { return id; }
   std::uint64_t by_owner() const { return owner.value; }

   FORGE_SERIALIZE(account, &account::id, &account::owner)
};

using accounts = forge::contract::multi_index<
    "accounts"_n, account,
    forge::contract::indexed_by<
        "byowner"_n,
        forge::contract::const_mem_fun<account, std::uint64_t, &account::by_owner>>>;
```

Rows are cached in stable owned storage for one action invocation. Primary and
secondary mutations use the generated C ABI and are committed or rolled back by
the host invocation transaction.

## Dependencies And Boundary

The target depends only on `forge_guest_runtime`, the guest lanes of
`forge.raw`, `forge.codec.*`, `forge.chain.protocol`, portable
`forge.expression.artifact` values, and the generated
intrinsic C header. Codec algorithms are prebuilt once into wasm32 archives;
consumer builds compile their module interfaces and link those archives. The
target owns no host binding implementation, filesystem, threads, deployment
logic or blockchain state implementation. Database iterator behavior, payer
rules, authorization, NaN validation and physical storage remain product host
policy.

## Stability And Tests

Intrinsic interface v1 and raw action/result bytes are compatibility contracts;
the C++23 API is experimental until the first SDK release. Modern void and
non-void actions, malformed input, return values, checks, package relocation,
all five secondary-key families, iterator boundaries, payer behavior,
autoincrement, singleton operations and execution in `forge.vm.wasm.interpret` are tested
end to end. The unchanged CDT database C fixture and an independent
Spring-derived signature manifest prove all 60 database imports through the
generated headers and VM parser. The full donor manifest separately verifies
all 152 imports and capability sets.
