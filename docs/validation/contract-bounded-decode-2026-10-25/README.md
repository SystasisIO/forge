# Exact bounded decoding: installed native/Contract SDK qualification

2026-10-25. **PASS_SCOPED_JOB0**, macOS arm64, LLVM 22.1.8, Release.
Accepted implementation: `569cc72ed870ac91fc3334143d646545b7ac8663`.
Machine-readable scope, hashes and preserved outputs: [acceptance.json](acceptance.json).

## Delivered boundary

- Additive `bounded_unpack_limits` and `unpack_bounded_exact` share the existing
  Raw implementation. Legacy four-field `unpack_limits`, its sixteen-byte wire
  representation, bare `{}` calls and two-field `call_entry` remain compatible.
- Opt-in `bounded_decode_policy` rejects oversized action/call input before
  payload allocation/read, and malformed/trailing arguments before constructing
  the contract or invoking the method. Calls route using a fixed stack header.
  Legacy default action decoding retains its previous permissive behavior.
- Strict decoding validates canonical booleans/varints, enum values before
  narrowing and nested aggregate element/byte-content budgets. Enum validity
  metadata is explicit; missing strict enum metadata fails closed.
- Native `forge_add_contract_library` accepts normal native dependency targets;
  the guest path still requires registered guest-compatible owners. Native
  aliases are deduplicated by resolved target identity.

The branch also integrates two earlier accepted deliveries missing from dev:
CEL native/Contract SDK (`7b95c3af`) and `DISPATCH_MODULE` (`2bd4b2a0`). Their
implementations are reused. This qualification builds expression values and
the guest SDK with the CEL evaluation backend disabled; it does not repeat the
earlier CEL worker/evaluator acceptance.

## Executed checks

| Gate | Result |
| --- | --- |
| Fresh native Forge install and Contract SDK install | PASS |
| Raw installed consumer | 2/2 CTest; 6 focused + 51 unchanged legacy Boost cases |
| Genuine contract fixtures | 17 E2E fixture targets built |
| Contract E2E in real WAMR interpreter | 1/1 CTest, all 53 Boost cases; five new bounded/compatibility cases |
| Downstream JOB0 wire fixture | 3/3 CTest; 208 accepted / 47 rejected native/WASM cases |
| ABI and independent hashes | Real Abigen output; Python hashes fixed bytes without another serializer |
| SDK archive relocation | PASS; real aligned multi-index VM and native/guest positive/negative dependency graphs |
| Independent reviews | GO; exact source and installed-file hashes recorded in the three review JSON files |

Inputs use Forge-native chain/K1/digest types. Checks cover a sixteen-kibibyte
envelope, malformed vectors, canonical encodings, zero-argument methods,
constructor/audit preservation, aggregate nested budgets, ID/index/time limits
and legacy wire/source compatibility. Ordinary golden tests cannot rewrite
their expected files.

## Limits and retained failures

The **unfiltered SDK consumer build FAILED** in unchanged `donorcompat`:
the reused upstream freestanding sysroot does not provide its regex/locale/
stream APIs. This is retained separately from the successful seventeen-target
build and full 53-case E2E run; no general SDK ALL acceptance is claimed.
Earlier compiler/configuration failures are also preserved; no test timeout,
assertion or resource threshold was relaxed to obtain PASS.

Raw limits bound admitted byte content/elements, not physical heap usage by an
arbitrary custom codec. Product authorization/state-machine behavior, financial
execution, TLSNotary/FairSwap profile proofs, full TurbineOS regression, Linux,
real Labs and live Spine are outside this acceptance. CI is not run.

Raw logs, configure metadata, CMake caches, SDK archive and WASM/ABI/manifests
are retained outside build directories in the local workspace at
`artifacts/jobs0-20261025`; `acceptance.json` gives their hashes. Ambient include
variables were removed only in child processes; Boost 1.90 and OpenSSL 3.6.4
were selected explicitly. The unrelated upstream sysroot is retained.
