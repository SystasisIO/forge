# Portable CEL and Contract SDK qualification

The implementation follows the existing native expression profile. CEL remains
an upstream, pinned native engine in the bounded worker. Contract code uses the
Forge Raw codec, existing intrinsic registry, `multi_index`, `call_wrapper` and
the existing test host. No alternate evaluator, VM, chain server or serializer
was introduced.

## Ownership and interface

| Owner | Responsibility |
| --- | --- |
| `forge.expression.types` / `artifact` | One physical native/guest schema; fixed-width types and portable checked program |
| `forge.expression.artifact_codec` | Parent-side envelope validation and domain-separated SHA-256 |
| Native worker | Bounded Protobuf decode, AST admission, fresh upstream type/reference checking, planning and execution |
| `forge.contract.expression` | `compile`, `load`, `evaluate` through three optional versioned imports |
| `forge.tooling.testing` | Injected expression runtime, invocation budget, typed failure propagation and rollback |
| Spine `tariffs` | Publisher authorization, services, immutable formula versions, denomination and quotes |

Format/profile/semantics are each version 1. Artifact declarations are sorted and
unique. The enum has an explicit shared `uint8` wire type; budgets and version
fields are `uint32`. Checked expression bytes are capped at 64 KiB, the complete
artifact at 96 KiB, SDK frames at 128 KiB. A literal 81-byte Raw fixture and an
independent SHA-256 vector pin the envelope. Native/guest tariff persistence and
the actual generated ABI separately verify the shared representation.

The worker reconstructs an unchecked AST from the expression, discards claimed
type/reference metadata, rechecks it with CEL, and compares canonical checked
bytes. It neither reparses source on import nor treats a digest as proof of
validity. Unknown versions fail closed. Private worker transport is version 2;
parent and helper must be deployed together.

The baseline 152 public and 48 runtime imports are unchanged. `expression_v1`
adds three imports as an optional extension. The generated baseline host
interface remains unchanged; Compute still permits only its existing five
imports. A host must explicitly supply the native capability.

SDK calls size through the existing counting datastream. The guest uses one
bounded request/response buffer; the host writes Raw directly to the provided
destination. The owning wire-to-native conversion lives in the test host's
`conversion` module. Hashing and Crypto remain in the parent; the worker's
strict link audit continues to exclude these modules and libraries.

## Acceptance

Qualified on macOS ARM64 with LLVM/LLD 22.1.8, Boost 1.90 and OpenSSL 3.6.4.
Implementation revision is `fc21998c`; assertion cleanup is `e815a6f9`.
The SDK was built from this clean revision using the developer profile and an
existing matching base sysroot. Shared foundation sources and guest archives
were rebuilt; native archives/BMIs were not reused as guest implementations.

| Gate | Result |
| --- | --- |
| Normal `forge` aggregate in `contract-sdk-host` profile | PASS |
| Native CTest: profile/artifact/runtime, worker, oracle, SDK bridge, existing host | 5/5 PASS |
| Upstream-backed private arithmetic and artifact mutation tests | 2/2 PASS |
| Exact arithmetic oracle | 1460 cases PASS |
| Genuine installed, relocated expression consumer with producer/Bazel reads denied | PASS, including a different worker loading saved bytes |
| Contract SDK foundation build and installation | PASS |
| Registry, CDT and modern SDK surface fixtures | PASS; base 200 plus optional 3 |
| Real Spine tariff and consumer WASM/ABI/manifests | PASS |
| Standalone Spine testhost/ABI/provenance gates | 4/4 PASS |

The tariff consumer invokes actual guest SDK wrappers and `call_wrapper`.
It checks v1/v2 coexistence, publisher rights, a valid program with a mismatched
schema, forged artifacts, amount bounds, typed errors, exact Raw bytes and
rollback. Hash guards bind the run to both WASM files, helper and SDK foundation.
The tariff remains opt-in and outside Spine's existing bootstrap qualification.

Independent reviews and actual runtime gates found and corrected: initialization
outside the watchdog and nested read-only scope; unsafe exception escape during
cancellation delivery; the old 1 KiB parent response bound; noncanonical service
declarations; unusable reserved names; a schema test stopping at a wrong digest;
configuration-time hashes without runtime guards; and an allocating state copy
after opening a transaction. Tests cover the corresponding observable paths.
Snapshot publication additionally has compile-time no-throw move guarantees.

Mixed Boost discovery selected a legacy stacktrace component independently of
Boost 1.90. Producer and installed package discovery now require the matching
Boost version. Auto-detected OpenSSL 4 exposed an existing, unchanged PKI compile
failure; this qualification selects supported OpenSSL 3 explicitly and does not
claim OpenSSL 4 compatibility.

Receipts and hashes are in the
[validation directory](../validation/expression-contract-sdk-2026-10-23/README.md).
Production Spine/WAMR integration, native resource charging, consensus/finality,
financial contracts, Linux and real providers remain separate gates.
