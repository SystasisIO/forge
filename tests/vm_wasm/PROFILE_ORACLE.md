# Approved-Profile Donor Test Port

The complete Forge unit/spec port uses the approved bounded WAMR profile, not
the historical EOS VM acceptance policy. This is an expectation reconciliation,
not historical engine parity. Original donor source, corpus, manifest and raw
failure evidence are unchanged. It changes no engine or production API.

## Exact Scope

[port_donor_tests.py](port_donor_tests.py) applies 17 case-scoped rules to seven
generated translation units after the original syntax-only port. These expand
to 34 cases when both interpreter and JIT are enabled. The
existing backend matrix, complete source lists and test registrations remain
unchanged. No skip, expected failure, runner filter or alternate engine is used.

| Cases | Approved expectation |
| --- | --- |
| Static/dynamic `allow_code_after_function_end` | Typed `exceptions::parse`, including with the obsolete permission enabled; retain both `fail_wasms` loops. |
| Static/dynamic `allow_invalid_empty_local_set` | Typed `exceptions::parse` for the same malformed input. |
| Static/dynamic `allow_u32_limits_flags` | Typed `exceptions::parse` for every original memory/table construction, not just the formerly fatal first one. |
| Dynamic call depth | Preserve all earlier direct/indirect/host recursion checks. Require typed `exceptions::profile` for depth 1,048,576, then reinitialize the same backend with depth 51 and execute depth 50 successfully. |
| `e_memory.1.wasm` | Typed `exceptions::profile` for initial 65,536 pages, before instance allocation; the current cap is 65,535. |
| `binary.38`, `.39`, `.40`, `.41`, `.43`, `.44`, `.45`, `.46` | Require successful construction of the unchanged corpus bytes: valid redundant u32 zero memory indices. |
| `unreached-invalid.87` | Require successful admission of the unchanged unreachable/bottom-typed `br_table`. |

Approval for the three retired permissions is in the
[release note](../../docs/releases/unreleased-wamr-bounded-profile.md#retired-parser-permissions).
`allow_zero_blocktype` is not retired or transformed. `binary.37` and `.42`
remain rejection checks for index 1; fixture association uses exact names and
hashes, not lexical position or an inferred `_line_N` selector.

The modern cases prove admission, not execution: those original functions have
no usable exports (and the branch fixture deliberately contains `unreachable`).
[prepared_validation_tests.cpp](prepared_validation_tests.cpp) separately
checks execution for zero memory-index encodings of lengths 1 through 5,
unchanged source bytes, index 1/overflow/six-byte/no-memory rejection, and
concrete i32/f32/f64 negative controls for branch typing. These controls remain
mandatory and are not replaced by the adapted donor cases.

## Provenance And Failure Behavior

The separate host-function harness adaptation supplies real test-owned host
storage to the custom converter for `host_functions_tests_1.wasm.hpp`. That
fixture declares no WASM memory; constructing pointers and references from
`nullptr + offset` is undefined C++, not an engine-compatibility requirement.
The converter and all four pointer/reference helper paths use the same real
buffer. The original fixture bytes, cases, line numbers and assertions remain
unchanged. This does not fabricate production WASM memory or relax default
pointer validation: the absent-memory rejection controls still run normally.
Exact donor/port hashes and all six replacement sites are checked separately
from the 17 profile-expectation rules and recorded in the manifest. No
sanitizer option, suppression or test selection changes for this repair.

The source hashes and exact replacements are owned by the porter. Each adapted
file must match both its original donor SHA-256 and historical syntax-only port
SHA-256. Each rule must match exactly one title/tags pair and each statement
exactly once inside that case. Missing, duplicate, shifted or changed inputs
fail generation; they cannot silently fall back to a different oracle.

Replacements preserve newline counts and the complete case inventory, including
`__LINE__`-derived Boost IDs. They retain all existing assertion sites: nine
throw checks become mandatory no-throw checks, the final unsupported-depth call
becomes a supported recovery call, and 14 additional assertion sites cover
previously unchecked constructors/initialization. This is a source-site count,
not an estimate of assertions executed inside loops or backend expansions.

[donor_manifest.json](donor_manifest.json) remains byte-identical to its pinned
historical input inventory. Both `--write` and `--check` compare all 105 original
sources, all 2,568 inventoried case declarations and all 2,473 corpus files with
that manifest; neither operation can refresh that baseline. `--write` does not
delete the output tree or remove unexpected files. `--check` rejects missing,
unexpected or modified generated files, including unadapted sources/support.

[profile_oracle_manifest.json](profile_oracle_manifest.json) is separate. It
records all 17 rules, exact replacements, source/fixture hashes, compiled case
lines, complete source output hashes, registration digests, assertion counts,
policy and tool/selftest/document hashes. Its backend-expanded count is
conditional on actual JIT enablement; the historical manifest's old architecture
matrix is preserved as historical data, not reused to select WAMR backends.

Profile adaptation is deliberately outside generic `transform_text()`, which
is also used by `regenerate_spec_test.py`. The specification generator and its
golden-output mapping remain unchanged.

## Verification

Run the lightweight selftests from the repository root:

```sh
python3 -B -m unittest discover -s tests/vm_wasm -p test_port_donor_tests.py -v
```

With `DONOR` pointing to the unchanged EOS VM checkout at
`e5b1fc79c4b8d78f32749afa94a8d4c4d071f67f`:

```sh
python3 -B tests/vm_wasm/port_donor_tests.py --donor "$DONOR" --fixtures vendor/eos-vm-test-wasms --write
python3 -B tests/vm_wasm/port_donor_tests.py --donor "$DONOR" --fixtures vendor/eos-vm-test-wasms --check
```

When the parent releases the build slot, rebuild the existing complete targets:

```sh
cmake --build "$B" --target test_forge_vm_wasm_interpret_unit test_forge_vm_wasm_interpret_spec -j2
ctest --test-dir "$B" -j1 --output-on-failure -R '^test_forge_vm_wasm_interpret_(unit|spec)$'
```

Keep prepared-validation and generator-mapping gates in the normal full matrix.
The existing `test_forge_vm_wasm_interpret_donor_mapping` is registered when
`FORGE_VM_WASM_INTERPRET_DONOR_ROOT` is configured; otherwise run the explicit
Python mapping command above. The selftests require no new CMake target.

This slice's C++ compilation/runtime status is **NOT_RUN** until that fresh
build and complete suite execution finish. Preserve all historical raw failures
unchanged and record new results under the approved-profile oracle identity.
