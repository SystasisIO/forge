# WASM Invocation And Optimizing Candidate Reconciliation

This candidate combines the canonical exclusive invocation API with the
optimizing port's existing native trap/cleanup and absolute interruption
boundary. It adds no second execution backend, watchdog, scheduler or parser.
The shared watchdog keeps the published `interruption::deadline` and
`interruption::canceled` values and appends neutral `none` and `interrupted`.
Legacy duration/stop calls preserve cancellation; the absolute API preserves
the supplied deadline and its pre-entry gate without rebasing it.
The published duration/stop constructor still delivers only from its timer
thread, including pre-stopped registration. Only the additive absolute API may
deliver an explicit stop synchronously on its requester. Both use the same
guard and single timer; duration stop callbacks only claim/wake and never run
user code inline, including when the requester holds an application mutex.

The canonical exception codes remain `canceled=4010003`, `busy=4010004`,
`shared_instance=4010005`, `unusable=4010006` and
`invocation_state=4010007`. Only the previously unpublished optimizing
candidate changes its experimental numeric records: `interrupted` is now
`4010008`, and its old candidate-only cancellation number follows canonical
`4010003`. No compatibility aliases or old-artifact migration mode are added.
`execution_unavailable=4000011` and the numeric-operation codes
`4040000/4040001` are unchanged. Pre-integration unshipped diagnostic records
are not a persisted wire-format compatibility promise.

One implementation owns interruption arbitration, code protection, callback
join, checked restoration and terminal TLS-count retirement. Scoped invocation
requires pre-entry admission but retains legacy nested timed-call semantics;
the additive absolute API still rejects overlapping typed ownership of the
same mapping. Private interruption transport is interpreted only by its exact
code owner, never converted to a nested host failure.

Invocation admission, context, memory and host FP/error cleanup use the existing
suspended-recovery adapter. Actual host exceptions retain their original type
and poison reuse; ancestor private transport does not. Restoration failure is
sticky, forbids reuse and takes precedence after callbacks have joined. The
existing failure-to-disable-code fail-fast boundary is unchanged.

Adoption requires a coherent rebuild, normal package install and consumer
rebuild. Earlier candidate binaries, helper receipts and installed BMIs do not
validate this combined source. Runtime, package, Linux and Docker results must
be recorded separately; this note itself is not acceptance evidence.
