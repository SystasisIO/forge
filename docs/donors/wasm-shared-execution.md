# Shared WASM Execution: Baseline And Ownership

Phase sections preserve historical checkpoints. Fresh integrated P1-P5 results
are appended below; they do not imply downstream acceptance or a release.

## P5: OpenAPI And Python Qualification

The P5 change repairs the existing `forge.api.http.openapi` generator, not a
new product-specific schema layer. Signed/unsigned 8/16/32/64-bit schemas carry
exact integer bounds. Indexed variants use `oneOf` complete two-item arrays,
each pairing a constant index with its own payload. Numeric format labels,
JSON wire encoding and both standalone native decoder policies remain unchanged.
The approved follow-up selects the existing exact policy at the HTTP boundary;
it does not make standalone JSON reads strict by default.

Primary references inspected are [OpenAPI 3.1](https://spec.openapis.org/oas/v3.1.0),
[JSON Schema 2020-12 numeric semantics](https://json-schema.org/draft/2020-12/json-schema-validation#section-6.1),
[datamodel-code-generator types](https://datamodel-code-generator.koxudaxi.dev/supported-data-types/)
and [Pydantic strict validation](https://docs.pydantic.dev/latest/concepts/strict_mode/).
Qualification pins generator 0.83.0, Pydantic 2.14.1 and jsonschema 4.26.0.

The native [fixture](../../tests/api/openapi_parity_fixture.hpp) promotes schemas
from real typed HTTP operations, rather than substituting handwritten schemas.
It checks literal extrema and adjacent failures, overlapping integer/object
variants, negative/fractional/coerced indexes, missing/unknown fields,
short/trailing arrays, optional/null, nested values, exact uint64 and exclusion
of server-supplied fixed arguments. An explicit attempted client binding of a
server-supplied argument must fail. No recursive credential traversal is added.

The [Python harness](../../tests/api/openapi_python/README.md) generates and imports
untouched models. Its 110 inputs cover 18 component schemas; acceptance verdicts
and decoded values are checked independently. The initial native RED run
reproduced both generator gaps before their fix. It also disproved a test
assumption: the current Glaze parser accepts nonnegative exponent integer forms
such as `7e0`, so that form remains recorded rather than changing the decoder.

This is **qualified coverage, not complete accepted-language equality**:

- HTTP requests, success responses and typed error envelopes select exact
  decoding. Required bodies/fields, integer ranges, extra fields and variant
  shape are checked before dispatch; absent explicit `body<T>` remains absent.
- Independent loopback HTTP tests exercise the real server, registry, binding
  and typed client, including invalid-input non-dispatch, valid controls,
  server-stream fixed arguments and sanitized malformed error responses.
- JSON Schema integers use mathematical equality. Exact Forge validation also
  depends on the parsed numeric kind. Raw Pydantic strict literals accept
  boolean variant indexes. The qualified consumer first validates original
  input with the generated schema, then the untouched model; boolean indexes
  are rejected recursively. Exact Decimal parsing avoids binary64 rounding.
- The original P5 report recorded 22 differing inputs. It is retained as
  characterization evidence, not the current acceptance policy. Remaining numeric spelling
  differences are explicit: generated strict integers reject `7.0` and `7e0`,
  while Schema accepts them mathematically. Exact Forge rejects floating-kind
  integer/index tokens but accepts nonnegative exponent integer tokens.
  Unexpected differences fail; raw Pydantic behavior is a separate observation.
- Forge omits disengaged optional members; Pydantic normally emits null.
  Decoded state equivalence is checked separately from JSON spelling.
- The existing Apple root-int64 typed-writer limitation remains characterized,
  not repaired. Integer variant and composite paths are tested separately.

The emitted `x-forge-json-profile` annotation documents exact HTTP validation and
the bounded canonical client profile, not universal equality for arbitrary
consumer schema rules, aliases, enum naming or custom scalar adapters.
The original 18 schemas do not establish parity for those customizations.
The external `api_http` component consumer checks exact bounds and correlated
variants. Its package-module closure requires an explicit dependency on the
canonical `api_core_values` owner, directly imported by HTTP modules.
Only focused builds/tests are in scope; full installation/CI and downstream
T1/T2 integration are not claimed. Exact pending-source hashes and final evidence
are retained in `.cache/acceptance/wasm-shared-execution/p5/` and the follow-up
`p5-strict/` directory before build cleanup.

## Scope

This note records implementation evidence for the Foundation roadmap
`docs/products/forge/roadmaps/wasm-shared-execution.md`, not a duplicate roadmap.
Earlier phase-local limits remain history; the appended integrated results
cover the current Forge/SDK checks, not downstream migration or live identity
integration acceptance.

## Inspected Baselines

- Forge: `5f6a9ecdd3613bbd953dafd693a5dddfeaeedeae`, fetched `origin/dev`
  on 2026-10-14.
- Turbine design baseline: `2052f99556b00ab57ba7c6b718dd77ed48ec604c`.
- Turbine current checkout: `14d10e1edabfeecb93f3810ba5632f95ba3d5176`.
  Its `libraries/compute/modules`, `libraries/compute/execution`, and guest
  sources have no diff against the design baseline. Product networking tests
  have changed; their previous results are not evidence for this extraction.

| Source | Accepted pattern | Boundary |
|---|---|---|
| Turbine `libraries/compute/modules` | Owned payload bytes, explicit continuation and one external effect per step | Compute artifact commands and product state stay downstream |
| Turbine `libraries/compute/execution/compiled_module.cpp` | Per-invocation context, memory, globals, watchdog and owned return bytes | Cache admission, scheduling and metrics stay downstream |
| Forge [VM backend](../../libraries/vm/wasm/interpret/include/forge/vm/wasm/interpret/backend.cppm) | Existing interpreter, guarded memory and interruption machinery | Shared code allocators do not imply independent cancellation |
| Forge [intrinsic registry](../../guest/libraries/contract/include/forge/contract/intrinsics.hpp) | One canonical source of intrinsic signatures | The production action profile must not install stateful Chain imports |
| Forge [API values](../../libraries/api/core/include/forge/api/core/types.cppm) | Canonical error fields and enum identities | Native error bytes must not change when guest support is added |
| Forge [Raw](../../libraries/raw/include/forge/raw/raw.cppm) and [codec](../../libraries/raw/include/forge/raw/codec.cppm) | Existing serialization mechanics | Explicit enum wire metadata must preserve non-opt-in defaults |

## Baseline Before Production Changes

Baseline fixtures must use literal expected bytes rather than derive their
expectations with the codec being tested. In particular:

- Native described API status uses signed 64-bit wire encoding while its C++
  underlying type is unsigned 16-bit.
- The codec-only enum path uses the C++ underlying type. This is a separate
  baseline, not proof of guest/native parity.
- A direct `std::array<status, N>` copies underlying enum object bytes;
  `std::vector<status>` and C arrays use a count followed by scalar codecs.
  Shared metadata must not silently normalize these existing layouts.
- API error field order, optional codec/details values and malformed input
  handling need fixed fixtures before the value target split.
- HTTP method numeric values and wire width must be measured independently of
  the downstream byte-sized HTTP enum.
- VM input validation and existing execution tests remain regression gates;
  the new invocation scope additionally needs same-package cancellation
  isolation, start-function coverage and resource cleanup tests.

Native/guest/generated-ABI parity, installed consumer closure and downstream
behavior are separate acceptance gates. Source inspection alone closes none
of them. Build logs and exact revisions are retained outside tracked source
in the coordinator-owned acceptance directory.

## P1 And Native Ownership Baseline

The native ownership split introduces public `api_core_values` and
`net_http_values` components. The former owns the existing API types module;
the latter owns the HTTP method module and its idempotence predicate. Runtime
targets consume these owners without duplicate module compilation. Native
imports and serialization remain unchanged.

Validation performed on the development worktree based on the Forge revision
above, with Homebrew LLVM 22.1.8 and Ninja on macOS:

| Evidence | Result and limit |
|---|---|
| Existing Raw, VM prepared-validation, VM unit/internal, structure | Passed before production changes |
| New native and codec-only literal wire fixtures | Passed before and after the native target split; separate executables avoid mixing codec specializations |
| New OpenAPI baseline | Reproduces absent integer bounds, unlinked variant index/payload and overlapping alternatives; this is not a schema-fidelity pass |
| SDK foundation and installed Abigen fixture consumer | Built and passed with the baseline SDK; not evidence that the new value components are guest-ready |
| Native value package consumers | Built and executed from the build-tree package with runtime dependencies disabled; installed-package tests are registered but not yet run |
| SDK host-profile value targets | Configured and built without API/HTTP runtime targets |
| Turbine native baseline | 17 SDK, execution and base-node checks passed against unchanged inspected source paths and its pinned Forge dependency |
| Turbine WASM baseline | Seven ABI/Labs/execution/limits/cancellation/SDK/bindings checks and the text/media base-nodes integration check passed |

The final focused Forge checkpoint passed seven suites: Raw, codec-only values,
OpenAPI baseline, prepared VM validation, VM unit, VM internal and structure.
Independent read-only reviews found no actionable issues in the baseline
fixtures or the native value-owner split. These are scoped source reviews, not
approval of the unimplemented guest wire contract or complete roadmap.

After the product baseline completed, its coordinator-owned generated build was
removed only after checking that no process held it open. Logs and the generated
WASM/ABI/metadata artifacts with SHA-256 hashes were retained separately. The
active Forge build and SDK remain available for incremental P2 work; no other
project's builds were removed.

The OpenAPI baseline also records a macOS root `int64` JSON-write limitation:
the overload set lacks an unambiguous conversion and returns `json.type`.
Exact integer decoding and the composite variant writer are checked separately.
Do not report scalar roundtrip success from successful decoding alone.

Turbine's active checkout subsequently acquired unrelated networking/plugin
changes. The inspected modules, execution, guest and corresponding native test
sources still matched its recorded HEAD when checked. This note does not claim
the entire concurrent product checkout was clean or tested.

## P2: Shared Enum Wire Contract

The existing `forge::raw::enum_wire_type` is now owned by its portable module.
An explicit `shared = true` specialization supplies the same scalar wire type
to native Raw, guest Raw and Abigen. Legacy defaults and specializations
without the marker are unchanged. API enums opt into their existing signed
64-bit native encoding; HTTP method keeps its signed 32-bit encoding and
numeric values. Their C++ underlying types are unchanged.

Abigen resolves imported specializations with Clang semantic lookup, not a
duplicate enum registry or a post-processed ABI JSON file. Its private
`enum_wire_resolver` pair validates markers, fixed-width aliases, integer
ranges and accessibility. Discovery does not require a pack/unpack
instantiation. Invalid contracts must fail without publishing new output
artifacts, with and without enum metadata output.

Historical `std::array<Enum, N>` bytes remain the underlying object
representation; its ABI element is the underlying integer, not the scalar
enum alias. Shared-enum C arrays are explicitly rejected by Abigen because
Raw's count prefix cannot be represented by the fixed-array ABI. ABI integer
aliases express width and signedness, not arbitrary narrower C++ domains;
native and guest additionally check that decoded scalars fit their underlying
type. Unnamed representable Raw values remain legal.

The installed Contract SDK exposes the two canonical value owners and compiles
their same physical sources for WASM. The HTTP values archive is included in
the foundation checksum manifest. Guest Describe/MP11 headers are supplied
with the SDK rather than borrowed from the host include path.

The standalone [parity fixture](../../guest/tests/wire_values/README.md) executes
WASM with the existing test host. It compares literal bytes across native Raw,
guest Raw and the generated ABI codec, including every named canonical API
enum constant and HTTP method, nested `error_payload`, signed/unsigned bounds,
containers and typed negative cases. Numeric ABI JSON and symbolic enum
metadata JSON are tested separately. `unpack_exact` rejection is deliberately
separate from the CDT-compatible dispatcher's permissive trailing-byte policy.

### P2 Validation

The P2 checkpoint uses the same macOS LLVM 22.1.8/Ninja baseline and a developer
Contract SDK. Coordinator builds use `-j4`; no full CI matrix was run. Results
apply to the pending source snapshot, not to a published release or merged PR.
Logs, generated artifacts and source hashes are retained under
`.cache/acceptance/wasm-shared-execution` outside the disposable build tree.

| Evidence | Result and limit |
|---|---|
| Focused native checkpoint | 14/14 passed: Raw, codec-only values, OpenAPI baseline, API Core, three VM suites, six SDK policy/package gates and structure |
| Installed SDK Abigen fixtures | Full fixture target passed, including 60 new invocations for imported shared traits, metadata on/off, invalid contracts, C arrays and effective access through inheritance/using |
| Access-control regression | The previous Abigen binary rejected valid public using of a protected member; the corrected resolver and complete access matrix passed |
| Native/guest/generated-ABI parity | Passed with every named API enum constant and HTTP method, fixed error bytes, signed/unsigned bounds, independent guest construction and typed malformed-input checks |
| Installed native values consumers | Both components configured, built and ran with OpenSSL discovery disabled; actual link commands contain no Asio, VM, HTTP runtime or TLS dependency |
| Relocated Contract SDK | Original install prefix absent; fresh guest build passed without old BMIs; WASM, ABI, enum metadata and contract manifest match byte-for-byte; parity passed again on those artifacts |
| Independent read-only review | No remaining actionable findings in P2 Raw, ABI, SDK/package and fixture boundaries; earlier checksum, typed-error and access-control findings were fixed and rechecked |

The fixture's symbolic scalar roundtrip supplements, but does not independently
pin, every symbolic name. Explicit symbolic nested JSON and named status checks
cover that distinct path. The alternate optional/variant value is additional
roundtrip coverage, not a second literal golden. No broader claim follows from
these checks.

SDK relocation here is a fresh-build check on one host, not a hermetic or
cross-platform reproducibility claim: the developer SDK still uses its declared
external toolchain. Native package evidence covers the installed values
components, not relocation of every Forge component. Linux, full CI, downstream
migration and live identity integration were not run for P2.

Still required by later roadmap stages: exclusive compiled-module invocation
and independent cancellation tests (P3), shared effect protocol (P4), strict
OpenAPI/Python schema fidelity (P5), and downstream golden/negative and live
integration coverage. None of those is implemented or declared accepted by P2.

## P3: Forge Invocation And SDK Executor

The P3 Forge work implements roadmap F1/F2. The maintainer explicitly separated
Turbine T1 from this iteration; no Turbine migration or downstream acceptance
is claimed. The current read-only Turbine reference was
`488fb003ee89203b9bcb1b613b4bc203f8346ea7`, specifically
`libraries/compute/execution/{compiled_module,intrinsics,execution_watchdog}.cpp`.

The existing VM backend now provides exclusive `with_invocation` admission.
One watchdog covers initialization, start and body, with a standard stop token.
Each call owns fresh context, globals and linear memory. Supported MVP tables
are immutable guest state; the module is not shared across concurrent calls.
Borrowed context and allocator references are detached before those values die.
Guest failures permit fresh reuse; host callback and unknown native failures
poison the instance, including errors caught inside a native body. This is not
rollback of host effects and does not forcibly preempt native callbacks.

Rejected donor patterns: sharing a compiled allocator to obtain cancellation
isolation, the product watchdog's two independent callback paths, and retaining
a dangling allocator after external-context teardown. Scoped tests distinguish
host-origin exceptions even when they use a guest exception type. A neighbor's
first guest-to-host tick precedes the interrupted invocation; that neighbor
keeps executing a guest loop until interruption completes and then returns
the expected value. Distinct allocator addresses and rejected sharing are
checked separately, rather than inferring isolation from successful serial calls.

Code-page restoration now checks `mprotect`; failure is a native allocation
error and permanently forbids reuse. Restoration never throws from a cleanup
destructor. A real unmapped-region fixture verifies this failure and timeout
state cleanup. Failure to disable code retains the existing fail-fast process
termination, because an endless guest cannot otherwise be guaranteed to stop;
a bounded subprocess test checks that behavior. This OS-failure boundary is
not process isolation or a recoverable per-call timeout.

`forge_execution_wasm` is a native Preview leaf. It owns only bounded pure SDK
actions, canonical name values, owned return bytes and optional bounded console.
Its five imports consume `FORGE_CONTRACT_INTRINSICS`; canonical C pointers are
lowered to wasm32 offsets and checked against host span signatures. Failed
registration is terminal instead of retrying partial mappings. Forbidden imports,
bad apply exports, guest traps, deadlines, cancellation and native failures are
separate typed errors. The SDK ABI, wire bytes and existing VM operations remain
unchanged. Cache, scheduling, persistence and network policy remain downstream.

On LLVM 22/libc++, instantiating `std::stop_callback` in the named-module
implementation omitted an inline constexpr lock symbol. The private exact
`watchdog_stop_callback.hxx/.cpp` pair owns the standard callback's native
lifetime outside that module; it does not implement another cancellation
primitive, polling loop or timer. The unsuccessful alternate-libc++ link trial
was removed; no machine-specific linker workaround enters production source.

The initial package build also exposed Homebrew's global include directory
shadowing pinned OpenSSL 3 headers with OpenSSL 4. The acceptance configuration
pins the Boost package's own include prefix and OpenSSL 3.5, rather than changing
unrelated PKI code. This is toolchain configuration evidence, not an OpenSSL 4
compatibility claim.

### P3 Validation

Acceptance covers Forge F1/F2 only, on macOS arm64 with LLVM 22.1.8 and Ninja.
All builds were coordinated with `-j4`; no full CI matrix was run. Source is
pending on `forge-wasm-shared-execution-v1`, not merged or released. The reviewed
source manifest and final test logs are retained outside the disposable build
under `.cache/acceptance/wasm-shared-execution/p3`.

| Evidence | Result and limit |
|---|---|
| Final focused CTest | 7/7 passed: structure, VM unit/internal/prepared-validation/invocation, execution unit and real SDK execution |
| Cancellation and ownership repeat | Invocation suite passed 10 consecutive runs, including overlapping guest loops, stop/deadline arbitration, cleanup and real protection-failure regressions |
| Real SDK execution | The P2-built `wirevalues.wasm` executes through the new executor, preserving its literal return bytes and rejecting malformed input; P3 did not rebuild or alter that guest artifact |
| Installed native package | Rebuilt and installed the final host package; separate `execution_wasm` consumer built and executed successfully with Clang/LLVM discovery disabled |
| Runtime dependency boundary | Consumer checks exact direct dependencies; actual executable links no Clang or LLVM runtime. Canonical Chain values retain their existing dependency closure |
| Independent read-only review | Scoped F1/F2 source review is clean after callback-origin, overlap-evidence and code-page restoration findings were fixed and rechecked |
| Whitespace/patch gate | `git diff --check` passed |

The successful final runs supersede earlier failed build attempts, whose logs
are retained rather than discarded. Tests prove the stated local boundaries,
not process-level isolation or preemption of arbitrary native host callbacks.
The OS failure to disable code pages remains an explicit fail-fast boundary.
Linux, x86 JIT execution, sanitizers, full CI and downstream product acceptance
were not run in this P3 iteration. Turbine T1 remains a separate task at the
maintainer's request; P4 and later roadmap stages are not implemented here.

## P4: Portable API Protocol

The subsequent P4 iteration adds `forge_api_wasm` / `api_wasm` with canonical
namespace `forge::api::wasm`. Its shared sources compile independently for native
and SDK guest use. The P3 result above is an earlier checkpoint, not a description
of the P4 tree. T1/T2 product migration and P5+ remain separate.

The implementation follows sections 5, 6 and 12 of the approved foundation
roadmap. Turbine reference at `2052f99556b00ab57ba7c6b718dd77ed48ec604c`
includes `libraries/compute/modules/include/turbine/compute/modules/`:
`payload.cppm`, `continuation.cppm`, `command.cppm`, `step.cppm`, `event.cppm`
and `validation.cppm`, plus their implementation where present.

Accepted patterns: owned typed payloads, explicit success/error alternatives,
versioned continuation, one effect per step, host-selected capabilities and
bounded exact decoding. Rejected patterns: product command alternatives and
Compute ABI versioning in Forge, a second HTTP method/error vocabulary,
duplicated Raw bounds/serializer, or a portable protocol owning network policy.

| Responsibility | P4 owner and boundary |
|---|---|
| Wire values | Payload, continuation, named effect, step/event, context and HTTP intent/response; metadata follows declarations |
| Canonical dependencies | API core error/status values, HTTP method values, existing Raw; no runtime API/HTTP, VM, Asio, auth, DB or compiler dependencies |
| Pure validation | Bounded data, exact type/binding/version, delay and decoded HTTP status/body policy; typed errors without echoing input |
| HTTP authority | Host adapter selects binding and performs URL/path/method/content-type/TLS/credential/redirect checks with existing components; P4 is not an SSRF barrier |
| Context | Serializable host-created facts, not a verified principal or client-granted authority; the host reconstructs current context on resume |
| Guest failure | Existing exception-policy assertion bridge, not a second guest error runtime |
| ABI | New Preview protocol; canonical dependency bytes stay unchanged, no claim of old Turbine Compute ABI compatibility |

The whole effect response type is distinct from its HTTP body type. Status-only
means an absent decoded body after host-side bounded disposal; it is not a claim
that the remote wire contained no body. Typed null remains different from absence.
Name matching does not validate arbitrary product schema bytes; the selected
product decoder and host adapter retain that responsibility.

The existing Raw size helper materializes bytes to preserve concrete codec
dispatch. Consequently `encode` uses one canonical serialization followed by
output-size checks, not a misleading allocating size pass followed by a second
serialization. Native serializers are trusted code, not allocation-sandboxed by
this API; their callers must bound input DTOs. Untrusted protocol decode has a
pre-decode input cap plus Raw allocation limits. Guest execution additionally
requires VM memory limits. A focused regression pins one product serializer call
on both accepted and oversized encoded results.

The independent SDK fixture uses product DTOs under `fixture`, arbitrary action
names, host context as a separate argument, and the production P3 executor.
Hardcoded bytes are compared against native Raw, independently constructed guest
values and generated ABI conversion. Package checks separately enforce the
protocol dependency closure and call the installed validation implementation.

### P4 Validation

P4 acceptance is local to macOS arm64, LLVM 22.1.8 and Ninja. All builds used
`-j4`; no full CI matrix was run. Source remains pending in
`forge-wasm-shared-execution-v1`, not committed, merged or released. Logs and the
reviewed source manifest are retained outside the disposable build under
`.cache/acceptance/wasm-shared-execution/p4`.

| Evidence | Result and limit |
|---|---|
| Final focused CTest | 12/12 passed: protocol, Raw, canonical wire values, VM invocation, execution, structure and six SDK source/package policy checks |
| Independent SDK fixture | Real WASM executed through the P3 executor; literal bytes, native Raw, guest-constructed values and generated ABI agree for every step/event alternative, HTTP/context and the product action's arguments/result |
| Negative guest cases | Exact decoding, malformed/trailing/oversized data, type/version/binding/delay mismatch and decoded HTTP status/body policies are exercised, including the inclusive input limit |
| Installed native package | Separate `api_wasm` consumer built and ran with OpenSSL/Clang/LLVM/ngtcp2 discovery disabled; actual executable links no API/HTTP runtime, VM, Asio, auth or compiler runtime |
| SDK relocation | Fresh guest build after moving the SDK and removing the old path produced byte-identical WASM, ABI, ABI metadata and contract manifest; the final relocated artifacts passed native execution/parity |
| Independent read-only review | Scoped source review is clean after installed-validation, product-action ABI and duplicate-serialization findings were fixed; source hashes identify the reviewed pending changes |
| Whitespace/patch gate | `git diff --check` passed |

The installed native consumer is separate from the execution/parity harness:
that harness uses the build-tree Forge package for its executor and ABI runtime.
The relocated SDK is the developer toolchain lane, not proof of a hermetic
cross-platform SDK. Failed RED/build/fixture attempts remain in the evidence;
they are not counted as successful acceptance runs. The encode-once regression
failed on the earlier double product serialization and passed after correction.

No Linux, sanitizer, full-CI or downstream integration result is claimed for
P4. No HTTP execution, authentication, persistence or network policy is proved
by these pure protocol tests. Turbine T1/T2 and P5+ were not implemented.

## Integrated P1-P5 Validation

Fresh coordinator-owned validation completed on 2026-10-15 against the combined
pending changes based on `5f6a9ecdd3613bbd953dafd693a5dddfeaeedeae`.
Environment: macOS arm64, LLVM 22.1.8, Ninja; builds used `-j4`.
This supersedes the earlier phase-local validation limits only for the checks
below, without rewriting their historical receipts.

Evidence filenames below are relative to
`.cache/acceptance/wasm-shared-execution/final/`. This is coordinator-local
evidence that may not be present in a checkout or shipped package, not a promise
of distributed logs.

| Check | Result | Evidence |
|---|---|---|
| Native configured `forge` exports and focused targets | PASS | `native-build.log` |
| Extended HTTP/WebSocket and trusted-subject tests | 2/2 PASS | `extended-build.log`, `extended-http.log` |
| Invocation interpreter-only option | ON macro present in compile command; build and 1/1 test PASS; OFF restored and rebuilt | `interpreter-command-proof.log`, `interpreter-build.log`, `interpreter-test.log`, `interpreter-restore-configure.log`, `interpreter-restore-build.log` |
| Actual native export installation and five standalone consumers | Configure/build/run and final reruns PASS for `api_http`, `api_core_values`, `net_http_values`, `execution_wasm`, `api_wasm` | `install.log`, `installed-*-configure.log`, `installed-*-build.log`, `installed-*-run.log`, `installed-*-final-run.log` |
| Fresh developer SDK | Build/install PASS | `sdk-build.log`, `sdk-install.log` |
| SDK relocation, original path absent | Both guest fixtures rebuilt; WASM, ABI, metadata and contract manifests byte-identical | `wire_values-relocated-{configure,build,artifacts}.log`, `api_wasm-relocated-{configure,build,artifacts}.log` |
| Full installed Abigen fixture target | PASS | `tooling-abigen.log` |
| Installed native package plus relocated SDK/artifacts | Wire/native parity 1/1 and API/native parity 1/1 PASS | `wire-native-{configure,build,test}.log`, `api-native-{configure,build,test}.log`; corresponding `.command` files pin the installed/relocated inputs |
| Final focused CTest | 27/27 PASS, 33.36 s; includes Python, structure and SDK policy gates | `focused-test.log` |
| Repeated cancellation and strict HTTP | 10/10 and 3/3 consecutive PASS | `repeated-cancellation.log`, `repeated-http.log` |

The one reviewer CMake finding was corrected by passing
`FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=1` to the invocation target when
enabled, matching neighboring VM suites. Its scoped rereview was CLEAN. The
arm64 ON/OFF checks prove definition propagation and interpreter execution, not
x86 JIT execution.

Python qualification used Python 3.12.11, datamodel-code-generator 0.83.0,
Pydantic 2.14.1 and jsonschema 4.26.0. The fresh report retains 110 inputs across
18 schemas and four explicit lexical differences: `7.0`, `7e0`, `[0.0,7]`,
`[1.0,"text"]`. Schema-first consumption of untouched models still matters;
stock Pydantic boolean literal acceptance is not repaired. This remains bounded
qualification, not universal accepted-language equality or proof for arbitrary
schema aliases, enum naming or custom codecs. Existing native writer and
optional-null representation limits remain as described above.

Full CI, Linux, x86 JIT, sanitizers and downstream/live acceptance are **NOT_RUN**.
T1/T2 and Gate remain excluded. Developer-SDK relocation is neither global
native-export relocation nor hermetic cross-platform reproducibility. Execution
ownership, host-callback/OS failure containment and protocol trust boundaries
are unchanged. This note records the tested Forge scope; exact-source review
and delivery are tracked in the PR. Project version is unchanged; no release
or downstream acceptance is claimed. This documentation update runs no builds
or tests.
