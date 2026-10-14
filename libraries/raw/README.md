# forge_raw

`forge_raw` owns binary serialization. Its main contract is byte-to-byte
compatibility with retained old FC raw layouts for supported types, while using
modern FORGE modules and Boost.Describe for structure traversal.

## When To Use

- You need deterministic binary packing for contracts, hashes, signatures or
  persistent wire formats.
- You need FC-compatible byte layout for retained primitives, containers,
  chrono types, variants, described objects and crypto wrappers.
- You need `datastream` helpers for size calculation and buffer packing.

## When Not To Use

- Do not use raw for human-readable config or diagnostics; use JSON/YAML.
- Do not add ad-hoc per-type binary formats if Boost.Describe order is enough.
- Do not serialize secrets into logs or diagnostics just because raw can pack
  them.

## Public Modules

- `forge.raw.datastream` — buffer/vector/size-counting streams.
- `forge.raw.varint` — signed/unsigned variable-width integer wrappers.
- `forge.raw.enum_type` — enum support.
- `forge.raw.enum_wire_type` — explicit shared enum wire metadata.
- `forge.raw.raw` — `pack`, `unpack`, `pack_size`.
- `forge/raw/serialization.hpp` — macro-only explicit-instantiation helpers for
  application/domain DTOs.

Target: `forge_raw`.

Dependencies: `forge_core`, `forge_exceptions`, `forge_reflect`, `forge_variant`,
Boost headers and Boost.Multiprecision.

`forge_raw` owns the retained FC wire conversion for supported `std::chrono`
types and intentionally does not depend on `forge_chrono`.

`unpack_limits` bounds per-container, cumulative-container and byte
allocations. A framed codec that decodes nested Raw payloads through
`unpack_nested_exact(parent, frame)` inherits the parent's configured
per-container and byte limits, capped by the frame size, and charges successful
nested container allocations to the parent's remaining cumulative budget.
Calling ordinary top-level `unpack_exact` independently for each nested frame
would reset that budget and is not suitable for untrusted framed input.

## Shared Enum Wire Types

Native described enums retain their signed 64-bit default; portable
`forge.raw.codec` retains the C++ underlying type by default. Existing
`enum_wire_type` specializations without a `shared` member keep that behavior.
For a type compiled for both native and WASM, explicitly opt in beside the enum
declaration, after ordinary declarations:

```cpp
import forge.raw.enum_wire_type;

enum class result_code : std::uint16_t { ok = 0, rejected = 400 };

namespace forge::raw {
template <> struct enum_wire_type<result_code> {
   using type = std::int64_t;
   static constexpr bool shared = true;
};
}
```

The native codec, portable codec and Abigen then use this one declaration for
scalar enum encoding, including vector/optional/variant elements. Wire types
must use fixed-width `intN_t` or `uintN_t` aliases (8/16/32/64 bits), not
platform-sized `long` or `size_t`. The underlying enum type must also be a
supported signed or unsigned integer; plain `char`, character-code types,
`bool` and extended integers are excluded. Shared scalar
packing rejects values outside the wire range; unpacking rejects values outside
the C++ underlying range instead of truncating them. Unnamed representable
values remain allowed by Raw; protocol membership validation is separate.

Historical array layouts are not normalized: direct `std::array<Enum, N>`
stores the underlying object representation, so its generated ABI uses the
underlying integer element, without scalar symbolic enum metadata. C-style
`Enum[N]` includes a count prefix; Abigen rejects shared-enum C-arrays because
the fixed-array ABI cannot express both that prefix and the exact count.
Use `std::array` or `std::vector` for a shared contract. Non-opted-in legacy
types and their serialization defaults are unchanged.

## Examples

### Pack A Described Struct

```cpp
#include <boost/describe.hpp>

#include <concepts>
#include <cstdint>
#include <vector>

import forge.raw.datastream;
import forge.raw.raw;

struct transfer {
   std::uint64_t id = 0;
   std::uint32_t amount = 0;
};

BOOST_DESCRIBE_STRUCT(transfer, (), (id, amount))

auto bytes = forge::raw::pack(transfer{.id = 7, .amount = 42});
static_assert(std::same_as<decltype(bytes), forge::raw::bytes>);
```

### Use Raw Bytes As The Hash/Signature Contract

When an application signs or hashes a C++ structure, the signed bytes must come from
the same `forge::raw::pack` path that the verifier uses. Do not rebuild bytes with
string concatenation, JSON or hand-written field loops.

```cpp
#include <boost/describe.hpp>

#include <cstdint>
#include <string>

import forge.crypto.asymmetric;
import forge.crypto.core.types;
import forge.crypto.digest.sha256;
import forge.raw.raw;

struct signed_command {
   std::uint64_t account = 0;
   std::uint64_t sequence = 0;
   std::string command;

   [[nodiscard]] forge::crypto::core::bytes signing_bytes() const;
};

BOOST_DESCRIBE_STRUCT(signed_command, (), (account, sequence, command))

inline forge::crypto::core::bytes signed_command::signing_bytes() const {
   auto bytes = forge::crypto::core::bytes{};
   forge::raw::pack(bytes, *this);
   return bytes;
}

auto command = signed_command{
   .account = 42,
   .sequence = 11,
   .command = "rotate-key",
};

auto private_key = forge::crypto::asymmetric::private_key::generate();
auto expected_public_key = private_key.get_public_key();

auto message = command.signing_bytes();
auto signature = private_key.sign(message);

auto verified = forge::crypto::asymmetric::verify(expected_public_key, message, signature);
```

Store golden raw bytes for protocol DTOs in tests. That catches accidental
member reordering before it becomes an interoperability break.

Avoid shortcuts in signing code:

- Do not sign JSON/YAML text, `to_string()` output or manually concatenated
  fields.
- Do not materialize a temporary byte buffer only to hash it when the sink
  accepts `forge::raw::pack` directly.
- Do not treat a recoverable signature as authorized until the recovered public
  key equals the expected signer.

### Calculate Size Before Writing

```cpp
import forge.raw.datastream;
import forge.raw.raw;

auto value = std::string{"hello"};
auto size_stream = forge::datastream<size_t>{};
forge::raw::pack(size_stream, value);
auto size = size_stream.tellp();
```

### Chrono Wire Compatibility

```cpp
import forge.raw.raw;

auto time = std::chrono::sys_seconds{std::chrono::seconds{1}};
forge::raw::pack(stream, time); // old FC time_point_sec: uint32 seconds
```

### Declare Explicit Serialization Instantiations

Use the macro-only header when an application wants one `.cpp` file to own template
instantiations for a frequently used DTO, while other translation units only see
`extern template` declarations.

```cpp
#include <boost/describe.hpp>
#include <forge/raw/serialization.hpp>

#include <cstdint>
#include <string>

import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

struct action_payload {
   std::uint64_t id = 0;
   std::string actor;
};

BOOST_DESCRIBE_STRUCT(action_payload, (), (id, actor))

FORGE_DECLARE_SERIALIZATION(action_payload)
```

Then place the implementation macro in exactly one module implementation unit
or `.cpp` file:

```cpp
#include <forge/raw/serialization.hpp>

import forge.crypto.digest.sha256;
import forge.raw.datastream;
import forge.raw.raw;
import forge.variant.exceptions;
import forge.variant.value;
import forge.variant.conversion;
import forge.variant.containers;
import forge.variant.chrono;
import forge.variant.multiprecision;
import forge.variant.format;
import forge.variant.described;

FORGE_IMPLEMENT_SERIALIZATION(action_payload)
```

`FORGE_DECLARE_SERIALIZATION_PACK` and `FORGE_IMPLEMENT_SERIALIZATION_PACK` cover
`datastream<size_t>`, `datastream<std::uint8_t*>`,
`datastream<const std::uint8_t*>` and
`sha256::encoder`. `FORGE_DECLARE_SERIALIZATION_VARIANT` and
`FORGE_IMPLEMENT_SERIALIZATION_VARIANT` cover `to_variant/from_variant`.

## Compatibility Rules

- Described member order is wire order. Changing `BOOST_DESCRIBE_*` order is a
  breaking binary change.
- Described enums use the retained FC `int64_t` representation by default. If
  an existing enum gains names after its wire format has shipped, its owning
  domain must specialize `forge::raw::enum_wire_type` to the previous fixed
  representation and retain a golden-byte regression.
- `forge::raw::bytes` is the canonical owning wire buffer and uses
  `std::uint8_t`. A donor DTO may still contain `char` or `std::vector<char>`;
  those declared field types retain their existing one-byte raw format and are
  not silently rewritten as unsigned integer fields.
- Forge requires `CHAR_BIT == 8`. Compatibility is defined by emitted bytes,
  not by whether the owning C++ container uses `char` or `std::uint8_t`.
- `sys_time<microseconds>` packs as old FC `time_point` (`uint64` microseconds).
- `sys_seconds` packs as old FC `time_point_sec` (`uint32` seconds).
- `std::chrono::microseconds` packs as old FC microseconds (`uint64` bit layout).

## Runtime Risks And Anti-Patterns

- Do not pack runtime resources such as file handles, sockets, executors or
  pointers. Raw is for value DTOs with deterministic ownership.
- Do not use raw bytes as diagnostics output. Convert to JSON/YAML or render
  explicit safe fields after redaction.
- Do not continue after `std::out_of_range` from unpack as if the stream were
  partially valid. Treat it as a malformed input boundary and fail the operation.
- Do not add raw overloads in unrelated libraries to “make it compile”. The
  owning domain should describe the value type or provide a narrowly reviewed
  compatibility overload.

## Typical Mistakes

- Do not put `forge::raw` overloads in `core`.
- Do not use filesystem path serialization as an application policy boundary.
- Do not catch raw bounds failures by parsing `what()`; errors are standard
  exceptions such as `std::out_of_range`.

## Tests

`tests/raw` contains golden byte tests for strings, described structs/enums,
derived types, chrono values, dynamic bitsets and common containers.
