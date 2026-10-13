# Contract ABI

Target `forge_tooling_abi`, package component `tooling_abi`, owns Clang AST
analysis, Spring/CDT-compatible ABI generation, Ricardian metadata and generated
dispatchers. Public modules are `forge.tooling.abi.generator` and
`forge.tooling.abi.command` in namespace `forge::tooling::abi`.

```cpp
import forge.tooling.abi.generator;

auto result = forge::tooling::abi::generate({
   .contract = "token",
   .abi = "token.abi",
   .dispatcher = "token.dispatcher.cpp",
   .attribute_plugin = "attr-plugin",
   .sysroot = "wasm32-sysroot",
   .sources = {"token.cpp"},
});
```

The first request source is the dispatch source and must declare the selected
contract. The generator verifies this rule and includes its canonical path, so
quoted includes keep normal C++ lookup semantics. `forge_add_contract` selects
the only source automatically; multi-source contracts must name
`DISPATCH_SOURCE` explicitly. When `request::depfile` is set, the generator
writes compiler-derived dependencies for every source/header opened during
analysis; Contract SDK CMake combines them with direct Ricardian dependencies
to prevent stale incremental ABI output. Integer ABI names are selected from
the Clang target model; in wasm32, `long` is 32 bits. Following CDT
`EOSIO_DISPATCH`, generated local actions execute only when `code == receiver`;
notifications require an explicit notification dispatcher. The library contains
no CLI `main`, compiler patches or guest runtime. Its tests are the pinned CDT
pass/fail fixtures plus local-include and wasm32-width regressions.

`sdk_include_paths` and the matching `abigen --sdk-include` option are reserved
for roots shipped by the selected Contract SDK. They remain ordinary C++
include roots so SDK-owned compatibility aliases retain their ABI meaning.
Compiler depfiles track textual includes; module-owner metadata from the active
guest CMake configuration validates public and private module visibility.

For a multi-source build, `request::source_wrappers` contains one generated
output path for each source after the dispatch source. Each wrapper includes
its original translation unit and emits only the generated record codec
definitions visible there. `forge_add_contract` configures these paths
automatically and compiles the wrappers instead of compiling helper sources a
second time. This preserves separate compilation and source-local type
visibility while making generated codecs available in every contract source.

For generated dispatchers, `abigen` also emits memberwise `forge.raw` codecs
for user-defined ABI records, including records used by action parameters and
results. Namespace-scope record codecs are declared before the contract source,
so action implementations can use `forge::raw::pack` and `unpack` without a
separate serialization macro.
Fields are encoded in declaration order after the single public, non-virtual
base, matching the generated ABI. Unions, inaccessible fields, references,
const fields, bit-fields and multiple or virtual bases are rejected instead of
producing an ABI that the guest dispatcher cannot execute.
Legacy `EOSIO_DISPATCH` sources use the same generated codecs: their macro
dispatch is deferred until after the codec specializations are declared.
Hand-written `apply` functions remain fully author-owned and are included
without generated dispatch code.

Only actual standard-library templates receive CDT container encodings such as
`T[]`. A product type named `vector`, `map`, `optional` or another standard
container name remains a user ABI record. Generated dispatch supports both
const and non-const action member functions.

Modern `[[forge::action]]` and `[[forge::call]]` parameters must be named so
their ABI fields are usable by clients. The EOSIO spelling preserves CDT's
legacy unnamed-parameter output for source and ABI compatibility.

## Additional roots and enum metadata

Opaque raw payloads do not expose their inner C++ types to ABI discovery. Name
those types explicitly with `request::abi_root_types`; Abigen resolves each
qualified name in the dispatch source, including its imported modules, and
uses the ordinary ABI type encoder recursively. A root may be a named record,
enum or type alias, including an alias for a container or variant. Template
expressions, unresolved or ambiguous names, and duplicate roots are rejected.
Types must be visible to the dispatch source. No public action is introduced.

```cpp
import forge.tooling.abi.generator;

auto artifacts = forge::tooling::abi::generate({
   .contract = "example",
   .abi = "example.abi",
   .dispatcher = "example.dispatcher.cpp",
   .attribute_plugin = "attr-plugin",
   .sysroot = "wasm32-sysroot",
   .sources = {"example.cpp"},
   .abi_root_types = {"example::request"},
   .metadata = "example.abi.metadata.json",
});
```

The CLI equivalents are repeatable `--abi-root example::request` and optional
`--metadata example.abi.metadata.json`. Without these options, ABI and numeric
enum behavior remain unchanged. Roots still work without a metadata output.
Metadata never changes `abi_def`: it is a separate
`forge::chain::protocol::abi_metadata` JSON artifact with version
`forge::abi-metadata/1.0`, the C++ root to ABI type mapping, and enum names,
canonical integer types and named values. The ordinary ABI retains any underlying
C++ alias chain. Names and values come from the Clang AST;
there is no handwritten enum list. Decimal strings preserve signed and
unsigned 64-bit values exactly. Duplicate enum numeric values are rejected
only when metadata is requested because their string representation would be
ambiguous; empty enums are also rejected in that mode. Requested roots must map
to distinct ABI types, including roots named through namespace aliases.
Enum metadata supports signed and unsigned 8-, 16-, 32- and 64-bit integers;
other underlying types are rejected only when metadata is requested.
The runtime consumer is the existing
[Chain API ABI codec](../../chain/api/README.md).

## Dependencies

- `forge_chain_protocol` and `forge_codec_json` for canonical ABI values;
- compatible `clang-cpp` and `LLVM` for target-aware AST analysis.

Requesting package component `tooling_abi` is the only case where this API
causes Forge package discovery to require Clang.

## Stability And Tests

The request/artifact API is Experimental in Forge 8.16.0; the generated ABI and
dispatcher behavior are Stable compatibility contracts. Unit, CDT fixture,
quoted-include, wasm32 integer-width, action-result, package and relocation
tests cover the public surface.
