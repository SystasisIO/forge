# FORGE Provenance

This file records source lineage and donor boundaries for FORGE. It is not a
license by itself; see `LICENSE`, `NOTICE`, and `THIRD_PARTY_LICENSES`.

Audit date: 2026-08-10.

WASM source-lineage/runtime clarification: 2026-10-20. Other audit sections and
historical qualification records are not re-audited by this clarification.

## Experimental WAMR Replacement

The replacement engine source is maintained in
`SystasisIO/wasm-micro-runtime` on `forge-runtime-profile-v1`. This experimental
working tree selects published draft fork candidate
`f6fe6d5ae1c14b2cc4301e2fca0b3364be6a692b` through
`cmake/ForgeWamrSource.cmake` and a clean `vendor/wamr` checkout. A fast-forward
push and remote-ref read confirmed this commit in the existing draft branch/PR
on 2026-10-19. Publication is not a merge or production acceptance.
Its upstream base is `ea5dd88e9ca40a44b6a5d9ffeb87a20833b13b8a`.
The seven existing runtime/compiler patches were transferred as separate
commits; at transfer commit `7459d1e`, all 22 changed engine files matched the
previously qualified prepared source byte for byte. Forge now archives the clean pinned fork without
build-time patch application. See the
[fork provenance](docs/donors/forge-wamr-fork-v1.md).

The subsequent reviewed commit `65f974b` corrects imported start and
post-instantiation execution; its dedicated regression matrix is recorded
in the same provenance note.
Later reviewed commits add structured exception IDs, raw argument allocation
classification and aligned runtime/AOT table layouts. Their bounded qualification
is revision-scoped, not automatic acceptance of subsequent allocation-usage
changes or products.
Further reviewed commits correct CLI allocation callbacks, static native frame
placement and block-type policy, then add budgeted native loading and eager LLVM
JIT preparation. The candidate also includes native instruction profiling
through the existing opcode translator, LLVM debug metadata and ORC object
layer. Earlier execution results, including those for `ed5c12a`, retain their
original revisions in the owning fork/follow-up notes; they are not
automatically transferred to later candidates. A local pin, scoped source review or
successful configure is not full runtime, installed-consumer or product
qualification. Candidate-specific status is recorded separately in the
[integration note](docs/donors/forge-wamr-dev-integration-20261018.md) and its
owning evidence.

Commit `69c107c` adds a sticky, typed FP-guard failure status in the existing
exec-env, after its previous scalar fields and outside the AOT-visible prefix.
Nested guards retain the first failure; only the embedder's outer operation may
clear it. This distinguishes a failed FP transition from ordinary guest control
flow without changing the public C API. The three-file change passed scoped
source review and private interpreter probes at that historical checkpoint;
its then-pending rebuild/delivery gates are not the current qualification state.
Earlier `12d1f906` framework and product results retain their original source
identity. The current fork also includes the reviewed Darwin DWARF unwind
registration change in `f6fe6d5`; current CI and installed-runtime results are
revision-scoped in the integration note, not complete product acceptance.

WAMR remains under its upstream Apache License 2.0 with LLVM Exceptions,
preserved in `vendor/wamr/LICENSE`. This dependency record does not relicense
the existing EOS-derived implementation, converters or test code. Their
notices remain until a source/dependency audit confirms which replacement
targets and delivered artifacts no longer contain them.

## Resource Token Verification

The optional host target `forge_auth_liboauth2` uses the SystasisIO/liboauth2
fork, pinned by `vendor/liboauth2`, on top of OpenIDC/liboauth2 2.4.0 commit
`85da4346435aec77f2ff11b529f2c1e8e5033c44`. The maintained patch is commit
`ec0059e8de6b4a8e7fed4e5d605bad3d46f035f5` on `resource-verification-controls`.
The fork adds public per-call
failure classification and bounded HTTP controls needed by resource servers.
Forge must not reconstruct failures from vendor log messages or implement a
second JOSE/HTTP engine. The original upstream qualification report remains
historical evidence, not an approval of the patched backend.

The original dependency qualification used Jansson 2.14.1 commit
`ed5cae4ed0621ef409510f94270c9f8f263736d0` and CJOSE 0.6.2.8 commit
`e787ace6a203d483038a793fd58f958a2f9b48c1`. That report remains historical;
the external-signing candidate below replaces CJOSE, not OAuth policy.
See `THIRD_PARTY_LICENSES` for their distinct
license boundaries. CJOSE 0.6.2.5 is deliberately excluded after review of
upstream advisory [GHSA-f6wf-pqg3-6wqq](https://github.com/OpenIDC/cjose/security/advisories/GHSA-f6wf-pqg3-6wqq).
Although the resource profile uses signed tokens rather than JWE encryption,
the qualified dependency must carry the upstream fix.

## JOSE External Signing Candidate

`vendor/cjose` pins the SystasisIO fork of OpenIDC/cjose, based on release
0.8.1 commit `6fdb5fe75a86acb9cd34ca2eb13d9e090a6df588`. Its additive C API
prepares immutable ES256 signing input and completes it only after verifying
the external signature with a frozen public key. CJOSE does not acquire private
keys, transports or an asynchronous runtime. Upstream encoding and crypto
remain the implementation, not copied Forge modules.

The optional OAuth backend uses the same vendored static target. Jansson
remains pinned to 2.14.1; CJOSE and Forge share `OpenSSL::Crypto`.
This is a qualification branch, not approval of the backend or a new public
Forge signing API. See the [F0-B report](docs/donors/cjose-external-signing-qualification.md)
for the candidate revision and CI evidence. Upstream issue/PR publication is
not part of this change.

## Linux Native Token Acquisition

`vendor/oidc-agent` pins the maintained SystasisIO fork of KIT's oidc-agent
`5.3.8`, upstream commit `f31513acaacaa7ebc115b3a329105d449b43ac99`.
The fork identifies itself as `5.3.8-systasis.1`; its exact gitlink is
`28d6f89701b64d5150b05d01f85b8a8eed54808e` on `token-response-expiry`.
It corrects per-response token expiry and ownership in the agent daemon
without replacing OAuth flows or changing the public C API/IPC field layout.
Forge consumes that public C API through the optional Linux-only
`forge_auth_oidc_agent` target. The daemon is deployed separately, including
when accessed through SSH forwarding; the client package alone is insufficient.
See the [upstream report](docs/donors/oidc-agent-upstream-report.md) for the
defect, reproduction and acceptance boundaries. MIT notices and bundled
third-party notices remain in the vendor tree.

## Forge Contract SDK

`guest` is Forge's native C++23 smart-contract toolchain assembly. Release
archives build unmodified LLVM `llvmorg-22.1.8` from commit
`ca7933e47d3a3451d81e72ac174dcb5aa28b59d1`, including Clang, lld, libc++,
libc++abi and compiler-rt for the freestanding `wasm32` target. LLVM remains an
upstream toolchain; Forge carries no compiler patches.

`guest/libraries/runtime/details/allocator.hxx` is source-derived from
AntelopeIO/cdt commit `69599db279b7b93d0688502720c15c6962a1401b`, file
`libraries/eosiolib/malloc.cpp`. Forge preserves the donor allocation, reuse,
coalescing, reallocation and WebAssembly memory-growth algorithms while
adapting the error policy, naming and integration to the versioned Forge
intrinsic surface. The derived file remains under the donor MIT license.

The macro-only EOSIO dispatcher veneer derives its parenthesized action-sequence
expansion from the same CDT commit, files
`libraries/eosiolib/contracts/eosio/dispatcher.hpp` and
`libraries/meta_refl/include/bluegrass/meta/preprocessor.hpp`. Action decoding
and invocation are implemented only by `forge::contract::execute_action`.

The attribute plugin, ABI generator, contract checker, manifest generator,
CMake package and guest runtime integration are Forge implementations.
AntelopeIO CDT at the commit above and Spring commit
`e6a99f68b67abc4d89fe716755b2e1394a4991f7` are pinned compatibility oracles
for attributes, ABI schema, dispatch and observable contract behavior. They are
not build dependencies and their patched compiler code is not incorporated.
The active CDT ABI pass/fail fixtures are hash-pinned, mapped one-for-one to
test-local Forge inputs and compared against the donor ABI goldens in CI.

Target-neutral `forge.raw` codec modules and guest-safe
`forge.chain.protocol` value modules are compiled from the same source units
for host and guest targets. They are not duplicated inside the SDK.

## Confirmed Derived Or Adapted Source

### EOS VM Source Lineage And Current Runtime

The original C++23 module port in `libraries/vm/wasm/interpret` was derived from
AntelopeIO/eos-vm commit `e5b1fc79c4b8d78f32749afa94a8d4c4d071f67f`. Retained
host-function/value conversion, neutral allocation/container/signal/watchdog
utilities and mapped test code still have donor source lineage. Replacing the
engine does not make those portions FORGE-original or remove their notices.

In the current experimental candidate, the shared WAMR engine owns WASM parsing,
validation, interpretation, AOT loading and explicitly enabled LLVM JIT
execution. The retired EOS parser, interpreter/visitor/writer and x86_64
machine-code backend are not current execution paths. The public `jit` and
`jit_profile` selectors use the optional upstream WAMR LLVM JIT, not EOS code
or a silent AOT/interpreter fallback. Ordinary interpreter/AOT runtime builds
do not require LLVM codegen libraries; compiler and LLVM JIT are optional
dependency boundaries. These source facts do not imply full migration or
consumer acceptance.

The EOS-derived portions retain their recorded EOS VM License V1.0 provenance;
the complete license remains at `libraries/vm/wasm/interpret/LICENSE.eos-vm`,
with existing notices unchanged. WAMR's separate license is recorded above;
this clarification does not relicense either source lineage. Donor tests are
mapped mechanically to Boost.Test and verified against source and fixture
hashes. Test fixtures come
from EOSIO/eos-vm-test-wasms commit
`ffbbb552e6020623d8ae148d81a152c8ef700325`.

`vendor/softfloat` pins AntelopeIO/berkeley-softfloat-3 commit
`19cbea6c254cdbb5d539584e5696675de997721d`; it remains a separate upstream
dependency and its C types are not part of Forge's public VM API.

The donor Catch2 oracle retains the original test bodies. A hash-checked CI
preparation step adds the nullable allocator overload and fixes forwarding
reference classification in the donor host result conversion; no donor test or
fixture is patched.

The following two paragraphs retain the historical regression record of the
earlier EOS-derived implementation. Statements about its parser, native writer,
interpreter primitives or copied JIT tables describe that historical port,
not current WAMR internals or qualification of the local candidate. The
historical results are preserved below rather than relabeled as WAMR results.

Forge carries regression-tested correctness fixes over the pinned donor:
function-type equality checks non-void result types, `vector_to_string` sizes
its destination before indexed writes, alternate-stack allocation rejects
`MAP_FAILED`, data segments reject unsupported memory indexes, and zero-length
guest spans do not probe outside their empty range. JIT execution also rejects
missing exports before looking up their function type, and `memory.grow`
interprets its operand as an unsigned page count. Default span proxies avoid a
zero alignment divisor, and interpreter argument conversion receives the active
host just like JIT. Hosted `execute_all` calls pass that host through the same
pointer contract as individual calls. These changes harden invalid input and
allocation-failure paths. Empty profile maps return the unknown-address
sentinel, and interpreter-only test lanes do not instantiate JIT regressions.
Zero-capacity managed vectors grow before their first write, and nested signal
scopes restore the outer memory ranges. JIT `execute_all` traverses the copied
JIT export table after parser storage is released. The unmodified donor test
bodies remain the compatibility oracle. The imported `f32` sign operations use
explicit unsigned masks so their bit-level semantics remain defined in C++.
Arena-backed managed vectors retain appended elements when they grow and reject
empty pops before their write index can underflow.
VM tests preserve the donor's disabled POSIX signal handling policy so guarded
guest-memory faults remain owned by the VM. AddressSanitizer instrumentation is
disabled only for the native guest-memory read/write primitives because lowering
a cross-guard-page store may mutate valid bytes before delivering the trap.

The initial standalone import (`02ef01e Initial standalone storlane-fc import`)
contained FC-style code under `include/fc` and `src`. That import carried the
MIT notice for AntelopeIO/spring, EOS Network Foundation, and EOSIO/eos. Later
commits moved and rewrote portions into FORGE modules. The following current
areas retain file-level source lineage or substantial layout/semantic
continuity:

- `libraries/raw`: mixed provenance, not a whole-library FC-derived claim.
  Retained or adapted source includes the byte-compatible pack/unpack
  templates, variant raw encoding, datastream shape,
  varint/signed_int/unsigned_int wrappers, `varint.cpp` conversion helpers,
  and enum serialization behavior that trace to `include/fc/io` and `src/io`.
  FORGE-original additions include the C++ module/target boundary, typed
  `forge.raw.exceptions`, macro-only `serialization.hpp` explicit-instantiation
  helpers, and hardened typed exception plumbing around datastream range
  errors.
- `libraries/variant`: variant value/object/static-variant support and legacy
  serialization behavior.
- `libraries/core`: selected utility code including `uint128`, string helpers,
  UTF-8 helpers, type names, time/version helpers where the file history traces
  to the initial FC import.
- `libraries/codec`: selected Base58 and Base64 implementations retain source
  continuity to the initial import.
- `libraries/crypto`: mixed provenance, not a whole-library FC-derived claim.
  Retained or adapted source includes numeric helpers (`bigint`,
  `modular_arithmetic`), selected BLS wrappers, selected secp256k1/recovery
  wrappers, and digest value/bitwise/HMAC/hex compatibility code where current
  files retain source continuity to the initial import. OpenSSL3 EVP backends
  and newer high-level crypto APIs are FORGE-original rewrites or additions.
- `libraries/log`: selected legacy logger, log message, log context, appender,
  and config code tracing to FC logging. Newer structured record/sink support,
  `std::source_location` APIs, and other hardened additions are FORGE-original.

FORGE modifications and original additions around these portions are distributed
under Apache-2.0. Upstream MIT attribution and license compatibility notices
are preserved in `THIRD_PARTY_LICENSES`.

## Original FORGE Source In This Audit

The following areas were introduced during FORGE foundation hardening or later
network/API work and did not show source continuity to the initial FC import in
this audit:

- `libraries/app`
- `libraries/config/core`
- `libraries/config/program_options`
- `libraries/codec/yaml`
- OpenSSL3-backed crypto mechanics introduced after the initial import,
  including current AES streaming/GCM/CBC mechanics, random bytes, KDF,
  DER/PEM helpers, Ed25519, RSA, X25519, X.509, WebAuthn parsing, asymmetric
  facade modules, and rewritten P-256 mechanics where current code no longer
  retains old FC source structure.
- FORGE raw infrastructure added after the initial import, including
  `forge.raw.exceptions`, `serialization.hpp`, target/module glue, and typed
  datastream range-error plumbing.
- current Glaze-backed `libraries/codec/json`
- current Boost.Describe-based `libraries/reflect`
- `libraries/asio`, `libraries/cli`, `libraries/config/env`, `libraries/exceptions`,
  `libraries/net/http`, `libraries/schema`, `libraries/net/transport`,
  `libraries/net/websocket`
- `libraries/net/quic`, `libraries/net/tcp`, `libraries/net/stcp`,
  `libraries/net/yamux`, `libraries/net/p2p`
- `libraries/api/core`, `libraries/api/transport`, `libraries/plugins`,
  `libraries/tui`

If a future file-level audit proves source continuity for a specific file in
one of these areas, update this section and `THIRD_PARTY_LICENSES`.

## Compatibility Donors Only

Some donor projects are used as behavior, protocol, or acceptance-test
references. They do not create runtime source attribution unless source text,
schemas, generated code, or vendored runtime code are copied into FORGE.

- libp2p specifications, go-libp2p, go-libp2p-kad-dht, go-libp2p-pubsub, and
  rust-libp2p are compatibility donors for `forge_net_p2p`, Yamux, QUIC/TCP
  transport composition, Relay, DCUtR, AutoNAT, DHT, Rendezvous, and GossipSub.
  The 2026-06-07 audit found no copied `.proto` files, protobuf-generated
  files, or copied Go/Rust libp2p source under `libraries/net/p2p` or
  `tests/quic_p2p`. FORGE P2P codecs are hand-written C++ implementations of the
  wire behavior.
- Boost.Asio and Boost.Beast are architecture and mechanics donors for async
  composed operations, buffering, and HTTP/WebSocket behavior. They are normal
  build dependencies, not copied source in FORGE.
- Go and Rust libp2p fixtures under `tests/libp2p_interop` import donor
  dependencies for live compatibility tests. They are development/test fixture
  dependencies and do not make libp2p a runtime source component of FORGE.

If future work copies libp2p `.proto` schemas, generated code, or source text,
that work must either be rewritten as an independent implementation or add the
exact upstream license and attribution to `THIRD_PARTY_LICENSES` before merge.

## Embedded And Vendored Third-Party Source

FORGE contains or vendors permissively licensed third-party source. Confirmed
items include:

- `vendor/secp256k1`: Bitcoin Core secp256k1, MIT.
- `vendor/bn256`: EOS Network Foundation bn256, MIT.
- `vendor/bls12-381`: Matthias Schonebeck BLS12-381, MIT.
- `vendor/utf8cpp`: Nemanja Trifunovic UTF8-CPP, permissive license.
- `libraries/codec/base58/base58.cpp`: Satoshi Nakamoto and The Bitcoin
  Developers, MIT/X11.
- `libraries/codec/base64/base64.cpp`: Rene Nyffenegger base64 implementation
  with Kevin Heifner modification notice.
- `libraries/core/uint128.cpp`: portions adapted from Evan Teran.
- `vendor/pugixml`: Arseny Kapoulkine pugixml, MIT. FORGE compiles
  `vendor/pugixml/src/pugixml.cpp` directly into `forge_codec_xml` as a private XML
  backend; `pugi::*` types are not part of the public API.
- `vendor/libmdbx`: unmodified libmdbx `v0.14.2` amalgamated C source and
  public C header, Apache-2.0. The annotated tag resolves to commit
  `c8780aecc3dea7f4f2cb83e88bb4d33a622774bd`. Archive and per-file hashes,
  the exact imported file set, and the upstream mirror notice are recorded in
  `vendor/libmdbx/MANIFEST.md`. Forge does not export the MDBX C API.
- `vendor/libsodium/upstream`: unmodified official libsodium `1.0.22` release
  tree, ISC. Forge compiles only the portable XSalsa20 source subset through
  the private `forge_crypto_symmetric_xsalsa20_vendor` object target. The
  Forge-owned integration shim is outside `upstream/`; no public Forge module
  exposes libsodium headers, types or functions. The official release archive
  SHA-256 and complete 678-file tree manifest are recorded in
  `vendor/libsodium/MANIFEST.md`.
- `vendor/c-ares`: the 532 unmodified upstream files from official c-ares
  `1.34.8`, MIT, plus Forge's adjacent source manifest and checksum list.
  Forge builds it only as the private static resolver backend for
  `forge_net_dns`; no Forge public module exposes c-ares headers, types or
  functions. The official release archive SHA-256, detached-signature key
  fingerprint and complete 532-file tree manifest are recorded in
  `vendor/c-ares/MANIFEST.md`.
- Botan test fixture: the 139-byte XSalsa20 vector in
  `tests/crypto/test_symmetric.cpp` is copied only as a test fixture from
  Botan commit `9bce628ffbe90d1cb9242ae0efcbf3bc2fb0123d`,
  `src/tests/data/stream/salsa20.vec`, BSD-2-Clause. Botan is not vendored,
  compiled, or a Forge build dependency.
- `vendor/CLI11`: unmodified CLI11 `v2.6.2` header set, BSD-3-Clause. The tag
  resolves to commit `37bb6edc5317e99af72ef48405e65d9ca5218861`; the archive hash and imported
  file boundary are recorded in `vendor/CLI11/MANIFEST.md`. CLI11 is the
  private parser backend for `forge_cli` and its C++ types are not exported.
- `vendor/appauth`: unmodified AppAuth for iOS and macOS `3.0.0`, Apache-2.0.
  The release resolves to commit `a972daac82d449d58ab119e91c68153e29ddac33`.
  Forge compiles the AppAuthCore and macOS source sets as the private native
  backend for `forge_auth_appauth`; Objective-C types are not exported by the
  Forge API.

Submodules may contain their own third-party dependency notices, such as
Catch2 under `vendor/bn256/third-party`. Those notices remain with the vendored
source and are not collapsed into FORGE authorship.
