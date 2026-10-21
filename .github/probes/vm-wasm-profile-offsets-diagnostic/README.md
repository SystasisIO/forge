# CI67 Profile: Focused Two-Division Candidate

Separate, manually approved diagnostic transport. Its exact push ref is
`vm-wasm-profile-offsets-diagnostic-v1`; it is not a mandatory CI or PR190 fix.
The runtime checkout is raw050/tree31c53/parent72a9 with WAMRda589, never the
transport's current dev HEAD. Before instrumentation it must be clean; after
the frozen patch exactly one known test postimage is permitted. All other
runtime files and vendors must remain unchanged.

This candidate combines the original frozen diagnostic with a proposed one-file
fixture strengthening: retain `q=((result*A)^B)/count`, then compute
`result=count/(q|1U)` before decrement, with the same independent C++ reference.
The first quotient supplies the second division's nonzero runtime divisor;
the second quotient feeds the next iteration and returned result. This is not
duplicate/dead work, but optimized retention and sampling of both live sites
still require actual qualification. The canonical clean fixture patch is only
an external source proposal; it has not been applied to the runtime baseline.

Raw050 test preimage is `aafa189cb65cca0cb9a5c858d0d2aabc98c06f191fc01026e6def2562db10f47`.
Combined candidate postimage is `e20260d9e09bdec5594130c2cbe067038e0fd979718ace2a2bb5ceb3a378c437`.
The proposed clean-test postimage without diagnostic additions is
`846cb85ff5c3728627c767e8527037249a74502c01d72e37ae2319f757d03a61`.
These identities are distinct from this transport commit and any future fix head.

The successful CI67 producer job118921561978 inside failed run39507960049
supplies artifact12310936206/K07a659. Both immutable digests, recipe closure,
LLVM22.1.8/libc++220108 ABI and real header guards are required. No fallback,
producer rebuild, forced HAVE value or loader-path override is allowed. Fresh
Brew/system dependencies are recorded, not claimed identical to CI67.

Only `test_forge_vm_wasm_profile_public` and normal dependencies are built j2.
Exactly one canonical CTest runs its ten Boost cases, j1/120s, within a120min
job. There is no Forge/helper/install/package build or observer rerun. All62
original assertion sites, the signal handler, sampling, ten-case enrollment,
1ms/300ms/4M/8192 and opt3/size3 are unchanged; only fixture arithmetic and its
independent reference change as described above. Runtime
RED stays RED; always-run collection does not convert failure into PASS.

The test-only patch emits histograms from existing captured tuples AFTER the
sampling scope stops, plus complete original fixture hex and case/function
context. Control ranges are public-translate queries with `executed=0`, capped
at65,536 lookups around a real captured PC. They are not executed samples or
a complete function/DWARF map. No private ABI/parser/public API is introduced.
The256-range output cap is explicit; truncated or missing records require
diagnostic HOLD. No fixture-only or production-map verdict follows from a
passing diagnostic alone. A deeper private probe requires separate approval.

The previous diagnostic39564127400 passed and observed two sampled offsets in
all seven populations, but did not reproduce CI67's single-offset failure.
Its dominant offsets374/410/440 identify WASM `br_if`, not `div_u`; lookup
controls are not native instruction-retirement evidence. CI67 remains FAIL
and its original cause UNKNOWN. This candidate is a test-only strengthening,
not a production-map repair or proof that the original failing map was correct.
Producer67 raw050, all11 recipe inputs, artifact/K and trust guards stay fixed;
no new LLVM build or alternate bundle is introduced. Inspect both new division
origins in focused histogram/control evidence. Missing live-site evidence must
not be compensated by retries, fabricated labels or weaker assertions. A focused
dirty-test PASS cannot accept a future clean fix head: independent actual review
and the full unchanged required CI on that separately identified head remain.

The new workflow and finite driver preserve the existing DIAG token/redirect,
safe archive, source identity and bounded capture mechanics. GitHub tokens and
signed download URLs are not stored in evidence. Owned command groups must
terminate before phase completion, even if their leader exits first.
Authenticated API requests reject redirects; the single signed download accepts
only HTTPS443 GitHub Actions/Azure Blob hosts and no further redirects. Raw050
owns TAR path/type/link validation; this driver adds no TAR-cardinality claim.
SIGTERM/INT enter bounded group cleanup; external SIGKILL cannot run finally and
remains a runner-cancellation/incomplete-evidence boundary, never a scoped PASS.
The final collector repeats source/pin/postimage guards even after a failed phase.

Implementation source, offline checks, independent review and publication/run
authorization are separate gates. No commit/push/dispatch/merge is authorized
by these files themselves.
