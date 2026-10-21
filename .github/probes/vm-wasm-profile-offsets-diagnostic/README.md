# CI67 Profile Offset Diagnostic

Separate, manually approved diagnostic transport. Its exact push ref is
`vm-wasm-profile-offsets-diagnostic-v1`; it is not a mandatory CI or PR190 fix.
The runtime checkout is raw050/tree31c53/parent72a9 with WAMRda589, never the
transport's current dev HEAD. Before instrumentation it must be clean; after
the frozen patch exactly one known test postimage is permitted. All other
runtime files and vendors must remain unchanged.

The successful CI67 producer job118921561978 inside failed run39507960049
supplies artifact12310936206/K07a659. Both immutable digests, recipe closure,
LLVM22.1.8/libc++220108 ABI and real header guards are required. No fallback,
producer rebuild, forced HAVE value or loader-path override is allowed. Fresh
Brew/system dependencies are recorded, not claimed identical to CI67.

Only `test_forge_vm_wasm_profile_public` and normal dependencies are built j2.
Exactly one canonical CTest runs its ten Boost cases, j1/120s, within a120min
job. There is no Forge/helper/install/package build or observer rerun. Original
arithmetic, assertions, 1ms/300ms/4M/8192 and opt3/size3 are unchanged. Runtime
RED stays RED; always-run collection does not convert failure into PASS.

The test-only patch emits histograms from existing captured tuples AFTER the
sampling scope stops, plus complete original fixture hex and case/function
context. Control ranges are public-translate queries with `executed=0`, capped
at65,536 lookups around a real captured PC. They are not executed samples or
a complete function/DWARF map. No private ABI/parser/public API is introduced.
The256-range output cap is explicit; truncated or missing records require
diagnostic HOLD. No fixture-only or production-map verdict follows from a
passing diagnostic alone. A deeper private probe requires separate approval.

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
