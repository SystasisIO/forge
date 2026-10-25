# Retained TLSNotary commitments: local macOS qualification

Date: 2026-10-25. Result: **PASS within the scoped native implementation**.
[Acceptance](acceptance.json), [source hashes](source-manifest.json),
[independent source/runtime review](review-final.json).

## Implementation

The existing [native library](../../../libraries/net/tlsnotary/README.md) and
[session plugin](../../../plugins/net/tlsnotary/session/README.md) now support
explicit SHA-256 transcript commitments, narrow versioned retained evidence and
prover-only openings. `engine::check` and local API 1.1 reuse the same admission,
deadline, cancellation, managed process and shutdown owners. Hashing and random
blinders remain upstream-owned. No upstream fork, parallel runtime, notary service,
contract verifier, signer or financial mechanism was introduced.

Upstream TLSNotary remains exactly `5f67389de3f8962227fddffd71d78a852acc9b01`;
MPZ and all other locked dependency versions are unchanged. Adding a direct serde
edge reused the existing locked package. No insecure Rust cfg/environment was used.
Default sessions preserve FTN1; opt-in sessions/checks use bounded FTN2.
Canonical retained Raw format 1 explicitly describes its field order, fixed
SHA-256 and implementation pin. Its described direction enum follows canonical
Forge Raw int64 encoding; the private worker protocol uses its separate u8 tag.

## Validation

macOS arm64, LLVM 22.1.8, Release, Homebrew Boost 1.90/OpenSSL 3. Conflicting
ambient include variables were removed only from child environments. Exact
commands, elapsed times and exit codes are retained in the JSON records and
[full unmodified logs](logs).

| Gate | Result |
| --- | --- |
| Rust worker unit tests | 18/18 PASS, [original successful log](logs/rust-tests-r2.log.gz) |
| Native session / retained evidence / official plugin | 3/3 PASS |
| Standard full configured dev-component installation | PASS |
| Fresh installed plugin and library-only consumers | 2/2 PASS |
| Final scoped CTest matrix | 6/6 PASS, no skips, [JUnit](final-tests-r2.xml) |
| Build and installed worker identity | Byte-identical, [manifest](deliverables-manifest.json) |

The final [CTest invocation](final-ctest-r2.json) took 146.02 seconds including
consumer builds. Both consumers use installed module sources, archives and the
explicit installed worker. Borrowed fixture headers are test composition only.
No source-tree Forge library or prebuilt BMI substitutes for installed imports.
The producer built all configured library/plugin archives for an ordinary CMake
installation; no hand-built partial package was presented as a full install.

Actual paired MPC against local HTTPS produces authenticated commitments while
API-key/selected response bytes remain hidden from the verifier. The application
fixture signs the complete verifier record and independently supplied context
before permitting an opening. Wrong signatures/context/origin/direction/translated
ranges fail that gate without releasing an opening. Hash checks use upstream
`hash_plaintext`; wrong bytes/blinders/digests require exact `verification_failed`,
not a worker/transport crash. Structural/capacity failures require their precise
typed codes. Canonical Raw field order, strict decoding and unknown/narrowing enum
values are tested. An unknown value cannot narrow into a known direction.

Private openings use separate 0600 files. A new host process authenticates the
saved record before reading openings and checking them through a fresh worker.
This is restart verification, not a power-loss durability claim. New check-path
tests cover duplicate admission, overload, active/pre-cancellation, deadline,
stopped/lazy-after-shutdown operations, rogue worker transport requests and child
reaping. A real negative MPC exchange rejects a mismatched commitment selection
with identical plaintext disclosure masks.

## Reviews and failures

[Independent final review](review-final.json) verified 29 frozen files with
fingerprint `c45b90aa7e4ee1c6e02a6811deb684ea4e41e0f60554e5439612a61bed62ce46`.
[Cross-review of foreign C++/plugin code](review-native-on-host.json) excludes
its author's Rust changes. [Upstream gate](review-upstream-gate.json) and
[static passes](review-static-final.json) retain their own snapshots.
The final reviewer checked five public evidence files and 17 raw logs against
the request credential/hidden-response markers and actual blinders: no matches.

Original failures are retained: new-worktree submodules were not initialized;
the C++ implementation needed a direct SHA module import; early test code used
an ambiguous multiargument Raw overload and raced a newly added compile definition;
the Rust Blinder adapter initially decoded a tuple-newtype with the wrong shape;
the first Raw direction golden incorrectly assumed u8; the installed plugin demo
initially omitted its direct App dependency. The fixes preserve the canonical
codec, upstream crypto, privacy assertions and time/resource limits.

Four concurrent unchanged Chain API compilations caused measured swap/disk
pressure. The owned install build was interrupted, the measured heavy target
completed with `-j1`, then the remaining targets completed with `-j4`.
[Resource record](resource-interruption.json), [scheduling record](install-scheduling.json)
and all interrupted logs remain. This is a build-resource exception, not a relaxed
runtime gate. No thresholds or timeouts were raised.

## Scope and cleanup

Retained evidence is **not an independently verifiable offline TLS proof**.
The caller must authenticate the entire record plus its context through trusted
local storage or an independently trusted application signer. The opening hash
alone does not authenticate origin, direction, ranges, lengths or application
semantics. The signature-before-release demonstration is a local application
fixture, not a deployed buyer transport or FairSwap protocol.

File encryption/delivery, bounded fair-exchange disputes, financial settlement,
live TurbineOS/Spine, Linux, real external providers and the economics of a $0.012
operation are NOT_RUN. Upstream research code has not been declared production
ready by this technical qualification.

[Cleanup](cleanup.json) records process/open-file checks and removal of the owned
build/install trees. Twenty-four local deliverable/restart-fixture files were
retained and rehashed; private openings and binaries are not committed to this
report. Source worktrees, shared Cargo cache and other tasks' builds are preserved.
