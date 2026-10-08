# Forge Contract SDK

Forge Contract SDK is the standalone C++23 toolchain for building smart
contracts executed by `forge.vm.wasm.interpret`. It compiles contracts for freestanding
`wasm32`, generates Spring-compatible ABI data and a dispatcher, validates the
finished WebAssembly module, and records the complete build identity in a
sidecar manifest.

The SDK is separate from the normal Forge host build. Configuring Forge does
not download LLVM, build a guest sysroot, or compile contract tools.

Reusable host services live in optional `libraries/tooling/*` package
components. The command programs in `tools/` are thin adapters over those
libraries; guest code and the SDK assembly remain under `guest/`.

## Profiles

- `release` builds unmodified LLVM `llvmorg-22.1.8`, libc++, libc++abi and
  compiler-rt from commit `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`.
  Release archives are reproducible and relocatable.
- `developer` uses an installed Clang/lld and a compatible upstream wasm32
  sysroot. It is faster for SDK development, but manifests explicitly contain
  `"reproducible": false`, report the selected Clang version line and omit an
  unknown source commit. Developer archives bundle the discovered runtime
  dependencies of all shipped tools; they do not retain links to the build
  machine's LLVM installation.

The host tools use the platform C++ ABI of their LLVM and dependency packages:
`libc++` on macOS and `libstdc++` on Linux. Linux archives bundle the selected
`libstdc++` runtime so C++23 library support does not depend on the destination
machine. Contract code is independent of that host choice and always uses the
pinned wasm32 libc++ sysroot shipped by the SDK.

SDK and Forge versions are identical. The sysroot schema and intrinsic
interface have independent versions because either contract can evolve without
changing C++ source compatibility.

## Stability

The `forge_add_contract_library()` and `forge_add_contract_project()` helpers
are Experimental. They create and launch ordinary CMake targets; they do not
define a second package or build-graph format.
`forge_add_contract()` and the existing guest C/C++ compatibility surface retain
the stability stated by their owning SDK libraries and headers.

## Install A Release Archive

Extract one platform archive and point CMake at its package directory:

```bash
tar -xzf forge-contract-sdk-8.5.0-macos-arm64.tar.gz
export ForgeContract_DIR="$PWD/forge-contract-sdk-8.5.0-macos-arm64/lib/cmake/ForgeContract"
```

The archive includes pinned compiler tools, the wasm32 sysroot with prebuilt
runtime, raw, codec, chain protocol and contract implementation archives,
Forge contract modules, ABI tools, validation tools, and an example under
`share/forge-contract/examples/hello`.

## Build A Contract

```cmake
cmake_minimum_required(VERSION 3.31)
project(hello_contract LANGUAGES CXX)

find_package(ForgeContract CONFIG REQUIRED)

forge_add_contract(
   hello
   SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
   SOURCES hello.cpp
)
```

For multiple translation units, identify the source that declares the contract
class. Other sources remain ordinary separately compiled implementation files:

```cmake
forge_add_contract(
   token
   SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
   SOURCES token_helpers.cpp token.cpp
   DISPATCH_SOURCE token.cpp
)
```

## Shared Host And Guest Protocol

Declare reusable protocol code once and add the same physical directory to the
native and standalone guest projects. Each configuration compiles it for its
own toolchain:

```cmake
find_package(Forge CONFIG REQUIRED COMPONENTS chain_protocol raw)
find_package(ForgeContract CONFIG REQUIRED)

forge_add_contract_library(
   product_protocol
   ID product.chain.protocol
   MODULE_BASE_DIRS include
   MODULE_SOURCES
      include/product/chain/ids.cppm
      include/product/chain/actions.cppm
   SOURCES protocol.cpp
   PUBLIC_HEADERS include/product/chain/macros.hpp
   PUBLIC_LIBRARIES
      Forge::forge_chain_protocol
      Forge::forge_raw
)

forge_add_contract(
   product
   SOURCES contract.cpp
   COMPILE_CHECKS protocol_checks.cpp
   LIBRARIES product_protocol
   COMPILE_DEFINITIONS PRODUCT_LIMIT=42 "PRODUCT_LABEL=\"configured value\""
)
```

`ID` identifies the library in ABI module-owner diagnostics.
`PUBLIC_HEADERS` declares rare macro-only public headers as a standard CMake
header file set; private textual dependencies remain compiler-discovered.
`PUBLIC_LIBRARIES` and `PRIVATE_LIBRARIES` create ordinary CMake dependency
scopes. In a native configuration, the returned target is a normal static
library that products may extend or install with standard CMake commands. In a
guest configuration, the declaration is complete: sources, compile settings
and include paths must not be mutated after `forge_add_contract_library()` or
`forge_add_contract()`. This keeps CMake compilation and Abigen on one semantic
profile instead of creating consumer-specific module variants.
An optional `forge_add_contract(COMPILE_DEFINITIONS ...)` list declares literal,
object-like `NAME` or `NAME=value` macros for that contract only. The same list
reaches Abigen, its generated dispatcher and implementation wrappers, and
`COMPILE_CHECKS`; it does not mutate imported library/module owners or neighboring
contracts. Quote string literal values as in the example. Leading `-D`,
function-like macros, generator expressions, semicolon lists, newlines and `#`
are unsupported. Duplicate names, compiler-reserved names (`__...` / `_[A-Z]...`),
`FORGE_...` SDK definitions and the configuration-owned `NDEBUG` are rejected.
Omitting the list preserves the existing contract build profile.
Directory-wide compile options, definitions and includes are rejected for the
same reason. The guest toolchain rejects external configuration-specific C++
customization and owns the standard Debug, Release, MinSizeRel and
RelWithDebInfo flags. Abigen receives that same selected configuration profile,
so `NDEBUG` and other configuration semantics match the compiled guest modules.
Single-configuration builds publish artifacts directly under `artifacts/`.
Multi-configuration builds isolate WASM, ABI, manifest, dispatcher and wrapper
outputs under configuration-specific directories such as
`artifacts/Release/` and `<contract>.generated/Release/`. The
`FORGE_CONTRACT_WASM_FILE`, `FORGE_CONTRACT_ABI_FILE` and
`FORGE_CONTRACT_MANIFEST_FILE` target properties contain the matching
`$<CONFIG>` path; exact `_<CONFIG>` properties are also available.

CMake and Clang remain responsible for module visibility. ABI tooling reads
compilation metadata only to require that every imported module has exactly one
owner in the active guest configuration; it does not reconstruct dependency
scopes. Textual includes are tracked by the generated depfile.

ABI-visible `multi_index` and `singleton` aliases belong in an imported guest
module interface. Library implementation units are compiled only by CMake and
are never reparsed by Abigen with an approximated command line.

The Contract SDK does not install downstream dual-target source packages.
Products use `add_subdirectory()` for shared source-tree libraries. An ordinary
native `install(EXPORT ...)`, including a destination for the
`forge_contract_modules` file set, remains the product library owner's
responsibility.

## Named Action Payloads

A shared action DTO owns its canonical action name:

```cpp
struct begin_revision {
   workspace_id workspace;
   inode_id inode;

   static constexpr forge::chain::protocol::action_name get_name() {
      return forge::chain::protocol::make_name("beginrev");
   }
};
```

For a handler with exactly one such parameter, ABI generation exposes the DTO
fields directly and dispatch unpacks the same DTO. An explicit action attribute
is allowed only when its value matches `get_name()`. Legacy handlers without a
valid public, static, zero-argument, constant `get_name()` retain the
method-parameter wrapper ABI.

Host code constructs an action without repeating the name:

```cpp
auto action = forge::chain::protocol::action{
    permission, contract_account, begin_revision{...}};
```

Host packing and guest dispatch both use `forge.raw`, so the binary action
payload has one type definition.

Configure a standalone guest project with the SDK toolchain:

```bash
cmake -S guest -B build/guest -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$ForgeContract_DIR/ForgeContractToolchain.cmake"
cmake --build build/guest --target hello_artifacts -j 4
```

A native project may call `forge_add_contract_project()` as a convenience
launcher for that same guest project. Products that add shared directories
outside the guest directory pass their common root once:

```cmake
forge_add_contract_project(
   product_guest
   SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/guest"
   BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/guest"
   CONTRACT product
   SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
)
```

The launcher forwards that root as `FORGE_CONTRACT_SOURCE_ROOT`. A direct guest
configure sets the same cache variable. Every guest target uses it for one
project-wide reproducible path mapping. Checked-in inputs must remain below that
root; generated contract inputs may live below the guest project's
`CMAKE_BINARY_DIR`, which has its own deterministic build-path mapping.

The target always produces:

```text
hello.wasm
hello.abi
hello.contract.json
```

## Modern Contract

```cpp
#include <cstdint>
#include <string>
#include <vector>

import forge.contract;

class [[forge::contract("hello")]] hello : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] void greet(std::string user, std::vector<std::uint32_t> values) {
      forge::contract::check(!user.empty(), "user must not be empty");
      forge::contract::check(!values.empty(), "values must not be empty");
   }
};
```

`forge::contract::context` provides `get_self()`, `get_first_receiver()` and the
action-data datastream. The generated `apply` dispatcher reads action bytes
through the same target-neutral `forge.raw` codec used by host applications.

## Legacy Source Vocabulary

The initial compatibility veneer accepts the same canonical attributes and
constructor style through EOSIO names:

```cpp
#include <eosio/eosio.hpp>

class [[eosio::contract("hello")]] hello : public eosio::contract {
 public:
   using contract::contract;

   [[eosio::action]] void greet(std::string user);
};

EOSIO_DISPATCH(hello, (greet))
```

`forge` and `eosio` spellings lower to the same Clang annotations, ABI model,
dispatcher, raw codec and intrinsic imports. The veneer is not a second
runtime. `EOSIO_DISPATCH` expands directly to the Forge dispatcher template;
when it is present, `abigen` includes the source without generating a second
`apply` entry point. The database C ABI is available through generated
`<eosio/db.h>`, while `<eosio/multi_index.hpp>`, `<eosio/singleton.hpp>` and
`<eosio/fixed_bytes.hpp>` adapt the same C++23 implementation used by modern
contracts. The remaining unchanged EOSIO header corpus grows only through
donor-backed compatibility blocks.

## Persistent Tables

`forge.contract.multi_index` provides primary and up to 16 secondary indexes
without a guest dependency on `forge.db`:

```cpp
#include <forge/contract/serialize.hpp>

import forge.contract.multi_index;
import forge.contract.singleton;

using namespace forge::chain::protocol::literals;

struct [[forge::table("orders")]] order {
   std::uint64_t id = 0;
   std::uint64_t customer = 0;
   std::string memo;

   std::uint64_t primary_key() const { return id; }
   std::uint64_t by_customer() const { return customer; }

   FORGE_SERIALIZE(order, &order::id, &order::customer, &order::memo)
};

using orders = forge::contract::multi_index<
    "orders"_n, order,
    forge::contract::indexed_by<
        "bycustomer"_n,
        forge::contract::const_mem_fun<order, std::uint64_t, &order::by_customer>>>;
using settings = forge::contract::singleton<"settings"_n, std::string>;
```

The API supports primary and secondary lookup, bounds, bidirectional and reverse
iteration, stable loaded-row references, `emplace`, `modify`, `erase`,
`same_payer`, autoincrement and the complete singleton lifecycle. The host owns
the transaction, authorization and RAM policy; a failed action rolls back its
row and index mutations together.

## ABI, Tables And Ricardian Text

`abigen` recognizes action, table and synchronous-call annotations, user
structs, aliases, variants, nested standard containers, action results and
donor-shaped `multi_index`/`singleton` table declarations. ABI 1.2 is emitted
for ordinary contracts; the `calls` extension selects ABI 1.3.

Ricardian source files can be attached without custom commands:

```cmake
forge_add_contract(
   token
   SOURCES token.cpp
   RICARDIAN_CONTRACTS token.contracts.md
   RICARDIAN_CLAUSES token.clauses.md
)
```

Relative Ricardian paths are resolved from the consumer source directory. The
generated ABI tracks contract sources, included headers and Ricardian inputs,
so an incremental build cannot retain stale ABI metadata.

Invalid or ambiguous annotations fail the build. ABI is parsed again by
`contract-check`; it is not accepted merely because JSON syntax is valid.

The tooling suite executes every active ABI pass/fail fixture from pinned CDT
commit `69599db279b7b93d0688502720c15c6962a1401b` as a separate Forge case. When
the donor checkout is present, each generated pass ABI is compared with the CDT
golden after normalizing only its generated comment and historically omitted
empty tail fields. The released SDK does not depend on that checkout.

## Standard Library And Allocation

Contracts use pinned upstream libc++, not Forge-owned replacements for
`std::string`, `std::vector` or algorithms. The shipped profile supports the
core facilities needed by production contracts, including containers,
iterators, algorithms, tuples, optional, variant, span, string views, concepts,
numeric types and utilities.

Localization and `<regex>` use the pinned upstream libc++ implementation.
The freestanding locale substrate supports deterministic ASCII `C`/`POSIX`
only; the empty locale name also selects `C`, never a host environment locale.
Unsupported named locales fail (`newlocale`/`setlocale` return null with
`EINVAL`; with exceptions disabled libc++ terminates a failed named-locale
construction through the contract check). There are no locale files or host
locale imports. The C substrate is compiled from pinned CDT musl sources;
source and lifetime policy are recorded in `libraries/runtime/PROVENANCE.md`.

Dynamic allocation uses the CDT-derived linear-memory allocator behind normal
C++ and C APIs. All non-template Forge guest implementation is compiled once
into the SDK sysroot. Each contract compiles module interfaces only to produce
compiler-local BMI files and links the finished archives instead of rebuilding
Forge sources.

The standard linker profile reserves 32 KiB of guest stack. The prior 8 KiB
reserve is insufficient for the canonical musl quad-precision formatter:
the measured Release `printf_core` frame is 8160 bytes (float formatting is
inlined), with another 288-byte `vfprintf` frame plus caller frames, and a real
default-profile Cstdlib fixture trapped out of bounds. This is a bounded build
resource reserve, not a change to contract source, action/row ABI, long-double
ABI or formatting algorithms. It does not promise unbounded recursion.
The executable default-profile fixture has an initial memory of two 64-KiB
pages and a declared maximum of 256 pages, below the existing chain-compatible
528-page ceiling. Its stack-pointer initializer is 32768. These measured limits
are fixture evidence, not a guarantee that every contract has the same initial
memory requirement.

```cpp
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

auto values = std::vector<std::uint64_t>{};
values.reserve(128);
values.emplace_back(42);

auto text = std::string{"persistent during this action"};
auto* bytes = static_cast<std::byte*>(std::malloc(256));
std::free(bytes);
```

The runtime supplies `malloc`, `free`, `calloc`, `realloc`, aligned allocation,
`new`/`delete`, memory/string primitives and constructor startup. Freed blocks
are reused and coalesced; allocation grows WebAssembly linear memory when
needed. Allocation exhaustion and failed contract checks terminate through the
canonical `env.eosio_assert_message` intrinsic. C++ exceptions and RTTI are
disabled.

Global destructors are registered by the real musl `__cxa_atexit` registry,
including allocation-backed spill after 32 registrations. Explicit C `exit`
drains callbacks in reverse registration order and then invokes `eosio_exit`;
`_Exit`/`eosio_exit` do not drain them. The pinned musl `__cxa_finalize` is empty.
Returning from `apply` does **not** execute registered global destructors.
Before each invocation the VM resets globals and guest linear memory; startup
constructors run again, and all prior invocation allocations and registry state
are discarded, including after a trap or host exit. The runtime does not add an
automatic per-action shutdown or persistent cross-invocation guest state.

The allocator owns only guest linear memory. It provides no host heap, files,
sockets, threads, clocks or random device.

## Intrinsics And Validation

Interface version 1 contains the exact 152-function union of the pinned CDT and
Spring contract interfaces. Every entry records its C header, WASM signature,
capability set and, where applicable, the protocol feature that enables it.
The seven capability sets are:

```text
core
database
privileged
crypto_ext
bls
call
instant_finality
```

The core set includes the lifecycle, action-data and context functions:

```text
env.eosio_assert
env.eosio_assert_message
env.eosio_assert_code
env.eosio_exit
env.action_data_size
env.read_action_data
env.set_action_return_value
env.current_receiver
```

`read_action_data` follows Spring's legacy contract: it normally returns the
number of bytes copied, but when no bytes are copied it returns the full action
payload size. `action_data_size` remains the direct size query.

The same version includes the 60 Spring/CDT database imports: the ten
primary `db_*_i64` operations and ten operations for each of `idx64`, `idx128`,
`idx256`, `idx_double` and `idx_long_double`. `<forge/contract/intrinsics.h>`
is canonical. The pinned CDT family headers under `<eosio/*.h>` are generated
from that registry without a second declaration list. The shipped interface
defines signatures only. The optional installed
`Forge::forge_tooling_testing` target registers all 152 functions directly
from the same registry. It executes the database family against
`forge.db.object`, uses Forge crypto for contract-visible primitives, and
supplies deterministic state for the remaining capability families. This is
an executable SDK oracle, not a product host binding. Authorization policy,
RAM accounting, consensus and the blockchain-owned storage schema remain
responsibilities of the product runtime.

The pinned CDT snapshot contains 14 public EOSIO C headers and 39 public EOSIO
C++ headers. The SDK installs each compatibility header plus the canonical
Forge C headers and 30 modern leaf modules. A committed surface manifest checks
these exact inventories together with attributes, ABI vocabulary and stable
contract-visible errors.

The declarative registry generates guest declarations and the compatibility
manifest consumed by `contract-check`. Validation rejects unknown or
wrongly typed imports, WASI, unsupported WebAssembly features, malformed ABI,
malformed modules and a missing `apply` export. Structural validation uses
`forge.vm.wasm.interpret`, so the build and node execution surfaces share one parser and
validation model.

## Build Manifest

`hello.contract.json` is a sidecar and does not alter WebAssembly bytes. It
records:

- SDK and exact LLVM versions;
- sysroot and intrinsic schema versions and hashes;
- release/developer profile and reproducibility status;
- imported host functions and enabled WebAssembly features;
- SHA-256 of the ABI and WebAssembly artifacts.

Keep the manifest beside deployed artifacts. A producer can verify that a
binary was built with an approved interface without trusting a source path or
local compiler installation.

## Build The SDK

Developer mode requires a Forge host package, Clang 22.1 tools and a compatible
upstream wasm32 sysroot:

```bash
LLVM_PREFIX=/path/to/llvm-22.1
cmake -S guest -B build/contract-sdk -G Ninja \
  -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" \
  -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
  -DCMAKE_PREFIX_PATH="$LLVM_PREFIX" \
  -DFORGE_CONTRACT_PROFILE=developer \
  -DFORGE_CONTRACT_FORGE_DIR="$PWD/build/forge-prefix/lib/cmake/Forge" \
  -DFORGE_CONTRACT_SYSROOT="$PWD/build/wasm32-sysroot"
cmake --build build/contract-sdk -j 4
cmake --build build/contract-sdk --target forge_contract_sdk_archive -j 4
cmake --build build/contract-sdk --target forge_contract_sdk_relocation -j 4
```

`LLVM_PREFIX` identifies the same Clang 22.1 installation used to build the
Forge host package. The guest compiler, dependency scanner, archiver and
ranlib are selected from that LLVM package rather than from an unrelated
program found earlier in `PATH`. `FORGE_CONTRACT_CLANG` remains an explicit
override for controlled developer environments. The matching C driver is
selected beside it, including versioned pairs such as `clang++-22` and
`clang-22`; wrappers can set `FORGE_CONTRACT_CLANG_C` explicitly. `wasm-ld`
may be supplied by a separate lld package, as it is in Homebrew.

Developer mode copies `FORGE_CONTRACT_SYSROOT` into a build-owned staged
sysroot before adding the guest foundation archives. The supplied directory is
never modified. `foundation.json` records the name and SHA-256 of every runtime,
raw, codec, chain protocol, contract and math archive. The complete staged tree
is also covered by `sysroot.sha256`; `ForgeContractConfig.cmake` verifies the
foundation manifest without exposing internal archive paths as consumer API.

Release mode builds the exact pinned LLVM and guest runtimes from source. Use
`FORGE_CONTRACT_JOBS=4` to bound superbuild parallelism.

## Boundaries

- Target is `wasm32` with the MVP feature profile, not WASI.
- Threads, filesystem, sockets, random device and wall-clock facilities are not
  part of the contract environment.
- C++ exceptions, RTTI and thread-safe statics are disabled.
- `forge.raw` codec sources and guest-safe `forge.chain.protocol` values are
  shared with host code; a second datastream or wire codec is forbidden.
  `FORGE_POLICY_THROW_EXCEPTION` selects typed host exceptions or the canonical
  guest check intrinsic without duplicating those implementations.
- `forge.crypto.asymmetric.values` is the single host/guest key and signature
  model. Contract operations use intrinsics, while host verification and
  recovery remain in `forge.crypto.asymmetric`; neither side owns a copy of the
  wire records.
- Spring and CDT are pinned compatibility donors and test oracles, never build
  dependencies of a released SDK.
- Blockchain controller bindings and deployment are outside this vertical
  block. `Forge::forge_tooling_testing` is an installed deterministic VM and
  ObjectDB test host, not a production blockchain controller.

See `docs/iterations/forge-contract-sdk-toolchain-v1.md` for the accepted
design, donor pins and compatibility scope.

<!-- contract-compatibility:start -->
## Compatibility Matrix

This section is generated from `tests/corpus/compatibility.json`. `Verified`
means an automated acceptance gate exists and passes for the pinned donor.

### SDK Surface

| Area | Status | Evidence |
|---|---|---|
| Toolchain and sysroot | Verified | `forge_contract_sdk_relocation` |
| 152 intrinsic functions | Verified | `check_sdk_surface.py` |
| C and C++ compatibility headers | Verified | `check_sdk_surface.py` |
| Modern contract modules | Verified | `surface contract` |
| ABI and dispatcher | Verified | `run_abigen_fixtures.py` |
| Raw, protocol, crypto and time | Verified | `contract SDK E2E` |
| multi_index and singleton | Verified | `run_multi_index_fixtures.py` |
| Executable VM oracle | Verified | `forge_contract_e2e_tests` |
| Relocation and reproducibility | Verified | `forge_contract_sdk_relocation` |

### Unchanged Contracts

| Contract | Source | Build | ABI | WASM | VM | Behavior | Evidence |
|---|---|---|---|---|---|---|---|
| Spring eosio.boot | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| Spring eosio.token | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| Spring eosio.msig | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| Spring eosio.wrap | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| Spring eosio.system | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| Spring test_api | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| Spring test_api_db | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| Spring test_api_multi_index | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| EOSIO eosio.bios | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |
| EOSIO eosio.boot | Verified | Verified | Verified | Verified | Verified | Verified | `forge_contract_corpus_integrity`<br>`forge_contract_corpus_abi`<br>`forge_contract_corpus_artifacts`<br>`forge_contract_corpus_e2e` |

The compatibility denominator covers SDK-owned source, ABI, wire, import
and contract-observable behavior. Controller, consensus, fork choice and
production blockchain host policy are out of scope.
<!-- contract-compatibility:end -->
