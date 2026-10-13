# App Lifecycle Acceptance

Date: 2026-10-14.

## Scope And Revisions

- Base: `9eac8072575da0ccc1f1337aa456a1f0caecf25c`.
- Final code/test candidate: `9906e5e50eeafcad6a199428e68836ff2b7c2f4c`.
- The final delivery documentation does not change this tested code.
- Approved clean C++ source break in Forge.App and official plugins only. No
  downstream source edits, release version, wire protocol or persisted-layout
  changes.

The [implementation contract](forge-app-lifecycle-v1.md) and
[mechanical migration](../releases/unreleased-app-lifecycle.md) define the new
ownership and consumer obligations. Donor provenance and logging boundaries are
recorded in [the donor note](../donors/app-lifecycle-observability.md).

## Environment

Native macOS arm64, Homebrew LLVM 22.1.8, Boost 1.90 and OpenSSL 3.6.4.
Both the vendored MDBX driver and local RocksDB 11.1.1 were enabled.
Native builds used Debug `-O0 -gline-tables-only`; sanitizer builds used
`-O1 -gline-tables-only` and frame pointers.

One coordinator build tree was used. Parallelism was reduced to `-j2` after
`-j4` exhausted the available 24 GiB RAM: approximately 16.5 GiB used swap and
1-3% compiler CPU progress. Neighboring builds and the shared VM were not stopped.
External consumers were configured, built and executed sequentially; successful
child build directories were removed after preserving their logs.

## Results

| Gate | Result | Evidence scope |
| --- | --- | --- |
| Native focused suites | PASS, 10/10 | App, HTTP stop/retry, TLS, HTTP plugin, plugin descriptors, Signer, Wallet, generic plugins, OTLP plugin, DB Store |
| Structure | PASS, 3/3 | General, Net plugin and App lifecycle structure |
| Repeated lifecycle execution | PASS, 20 executions | App and HTTP stop/retry, ten consecutive executions each |
| Installed package consumers | PASS, 11/11 | App, Chain API and the nine registered plugin/package surfaces listed below |
| AddressSanitizer | PASS, 3/3 | App, HTTP stop/retry, OTLP plugin; leak detection enabled, no suppressions |
| ThreadSanitizer | PASS, 3/3 | App, HTTP stop/retry, OTLP plugin; no suppressions |
| Independent source review | CLEAN | App/async ownership and plugin/configuration/layout/test evidence |
| Whitespace gate | PASS | `git diff --check` |

The final native and structure run on `9906e5e5` passed all 13 tests in
124.62 seconds. The repeated run used `d95dcdb5`; production and regression-test
sources are identical between that revision and `9906e5e5`. The latter adds only
the external consumer dependency-selection fix.

Installed consumers: `app`, `chain_api`, `plugins_descriptor_surfaces`,
`plugins_net_p2p_node`, `plugins_net_p2p_resolver`, `plugins_chain_signer`,
`plugins_crypto_secrets`, `plugins_crypto_wallet`, `plugins_net_http_server`,
`plugins_log_otlp`, and `plugins_db_store`.

Package acceptance used the projects, configure options and runtime commands
registered by CTest, with configure/build/run separated to enforce a real Ninja
`-j2` bound. It was not a CTest `11/11` invocation: the final frozen harness
reported ten successes and the separate full Chain API rerun reported one.

## Findings Closed

- Caller cancellation no longer changes the cleanup owner's cancellation policy
  or suppresses an already-published terminal result. Cleanup is a joined child
  operation, not a detached task.
- Protocol admission and publication use the same canonical lifecycle boundary.
  Stale API calls and stopped Signer calls retain their precise typed errors.
- OTLP tests distinguish successful export before stop from collector receipt
  after stop and prove that shutdown actually entered its drain phase.
- Regression fixture teardown releases cancellation handlers before destroying
  their executor context. Timeout teardown has a deterministic regression.
- One-shot configuration tests use fresh owners for each invalid document and
  still prove the original configuration error, rather than accepting a generic
  lifecycle rejection.
- The installed HTTP API component explicitly exports its existing Exceptions
  dependency; external Chain test units import their lifecycle type directly.
- A real Chain API consumer crash exposed mixed OpenSSL ABIs: the producer and
  ngtcp2 used OpenSSL 3, while external CMake discovery selected OpenSSL 4.
  Both generated test-consumer caches now inherit the exact producer dependency.
  No protocol switch, fixture weakening or production workaround was introduced;
  all eleven consumers were rerun consistently.

Earlier failed or interrupted runs are retained, not counted as PASS. These
include the native fixture crash, initial package import failure, mixed-ABI
crash, an unbounded nested build that the coordinator stopped, and a harness
summary parse error caused by editing its script during execution. The final
frozen harness runs supersede that harness result.

## Review And Limits

Galileo reviewed App lifecycle, cancellation and observability; Kuhn independently
reviewed official plugin ownership, configuration, package boundaries and tests.
Both closed their scoped findings on `d95dcdb5`; Kuhn additionally reviewed the
test-only OpenSSL cache delta on exact head `9906e5e5`. Reviewers did not run
builds; runtime evidence was produced by the coordinator.

This is focused native acceptance, not full CI, Linux validation, a new Go/Rust
interop campaign or a production soak result. External Homebrew libraries are
not claimed to be fully sanitizer-instrumented. Local sink tests prove logging
content and lifecycle isolation; App does not guarantee remote OTLP delivery
before exporter startup or after exporter shutdown.

An already-canceled caller may be rejected by Asio before it enters a lazy
awaitable. Cleanup ownership guarantees apply once the operation is entered;
the implementation does not start cleanup eagerly to hide that boundary.

Logs and reproducible local harnesses are retained under
`.cache/acceptance/app-lifecycle-20261014/`: `native-9906e5e5-tests.log`,
`lifecycle-repeat-d95dcdb5.log`, `package-openssl3-summary.log`,
`package-chain-openssl3-summary.log`, per-consumer logs, and
`address-tests.log` / `thread-tests.log`. Only the inactive task-owned build
tree is eligible for post-merge cleanup; acceptance logs and neighboring work
remain intact.
