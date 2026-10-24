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

Contracts may opt in to exact bounded invocation decoding:

```cpp
import forge.contract.dispatcher;

class [[forge::contract("bounded")]] bounded : public forge::contract::context {
 public:
   using context::context;
   static constexpr auto decode_policy = forge::contract::bounded_decode_policy{
       .max_action_bytes = 16U * 1024U,
       .max_call_bytes = 16U * 1024U};
   // Actions and synchronous calls use this one policy.
};
```

The generated dispatcher checks the complete payload size before allocating or
reading its payload, decodes arguments with Raw budgets, and rejects trailing
bytes before constructing the contract or invoking a method. A call's byte cap
includes its 12-byte version/function header. Call routing first reads only that
fixed-size header into stack storage; the selected entry checks its complete
envelope before allocating or reading the argument payload. This also supports
manually composed tables with different contract policies without changing
the existing two-field `call_entry` source contract.

Defaults are 16 KiB per action/call, 1,024 entries per element-counted container,
4,096 cumulative entries, and 16 KiB both per string/byte vector and cumulatively
for their contents. `decode_policy.unpack` holds the ordinary
`forge::raw::bounded_unpack_limits`; its `allocations` member reuses the ordinary
four-field `unpack_limits` and can select smaller budgets. Defaults also reject
nonminimal varints and enum values outside their declared domain. Enum metadata
comes from `enum_wire_type<Enum>::is_valid(wire_value)` or Boost.Describe; strict
decoding rejects enums without either. Boolean decoding already requires 0/1
in all profiles.

The bounded policy controls SDK/Raw decoding. Custom codecs must recurse through
Raw using their supplied stream, and framed codecs must use
`unpack_nested_exact(parent, frame)` to preserve budgets and strict flags.
Independent streams and arbitrary C++ allocations inside custom codecs are not
sandboxed. Element counts and byte-content budgets do not bound all physical
heap overhead or `sizeof(T)`; execution memory limits and suitable contract DTOs
remain necessary. A truncated value may allocate within those admitted limits
before its range check fails.

Contracts without `decode_policy` retain CDT-compatible action decoding,
including permitted trailing action bytes. Existing calls continue to require
exact input. No existing contract silently acquires the new limits, enum-domain
checks or minimal-varint requirement.

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

The installed `recordtest` consumer opts in with smaller limits to exercise real
WASM action and call dispatch: valid Raw arguments, limits before payload reads,
constructor effects, malformed lengths, cumulative budgets, unknown enums,
nonminimal varints, invalid bools, truncation, trailing bytes and no-argument
entry points. The unchanged legacy consumer retains its CDT trailing-byte test.
