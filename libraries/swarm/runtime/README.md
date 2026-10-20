# Swarm Runtime

Status: **Preview** C++ source API. Existing Swarm/API wire formats are unchanged.

`forge_swarm_runtime` (`swarm_runtime`) composes Core transfer/choker mechanisms,
the generated `swarm::api::peer` contract and caller-owned Asio execution.
It imports no concrete transport, storage, database, P2P or plugin implementation.

## Composition

Import `forge.swarm.runtime.engine` for the engine and move-only sharing/download
handles. The handle interfaces are `engine:sharing` and `engine:download` module
partitions. Policy, shared quota and product capability interfaces are standalone
`forge.swarm.runtime.admission`, `.quota` and `.capabilities` modules.

Supply a caller-owned `task::scheduler`, a bounded CPU `compute::executor`, an
explicit `admission` policy and (for discovery/advertising) `capabilities`.
Connection acquisition belongs to passive, owned `capabilities::route` objects:
lookup and explicitly supplied download routes use the same contract. A route's
`key()` is a bounded diagnostic key, never an authenticated identity. Its
repeatable `async_connect(stop, deadline) const` must retain backend context in
the returned awaitable even if the route facade is destroyed. The runtime retains
the route list through retry rounds and the completion of all children.
Explicit routes work without a discovery capability. Adapters retain the complete
bounded address set rather than encoding it into a string or a second peer cache.

This is an intentional Preview C++ source break: replace `vector<string>` routes
and `capabilities::async_connect(string, ...)` with owned route factories and
`vector<unique_ptr<capabilities::route>>`. There is no compatibility overload;
API wire and persisted state are unchanged.

Supply `shared_ptr<core::store>` to `async_share`/`async_download`. The store owns
disk/verification execution. Runtime does not own, stop or close that store,
the shared scheduler, compute pools, or product persistence. Handles cancel on
destruction; **await `async_close()` before stopping their shared dependencies**.
Destroying a handle cannot synchronously join a coroutine or filesystem syscall.

`download::async_wait()` waits for verified content AND store atomic publication;
it reports terminal failure. Published success is sticky: a later wait does not
turn success into a timeout after the total deadline. A fully verified resume or
all-empty content publishes without needing providers or a connection capability.
`async_close()` first cancels, then joins tasks,
calls and runtime handlers, closes provider/connection holds, checkpoints and
releases leases. Checkpoint/registration close failures are reported. Closing
does not remove files. A canceled operation never stops another operation's store
or scheduler. One engine rejects two active downloads targeting the same store;
composition must likewise prevent multiple writer engines for one store.

An active download can serve only its store's verified pieces through this same
engine endpoint and explicit policy. Inventories and hash responses never turn
unverified staging data into availability. Product-selected descriptors are the
trust anchor; network inventory/discovery results are hints, not authorization.
Existing sessions watch `store::async_wait_change`; the same bounded writer
coalesces verified additions into small HAVE batches and replacements/invalidation
into an inventory. Explicit `async_share` grants permission to advertise, not to
bypass per-caller admission. An operation-owned availability watcher registers
on the first verified piece, and cancels/joins the registration when the last
verified piece disappears. Partial availability is eligible, not a claim of
complete payload possession. Nonempty content with zero verified pieces is not
advertised. Validated all-empty content may advertise its metadata endpoint.
This watcher is independent of RPC sessions and is joined before lease release;
the capability adapter alone owns provider renewal. Downloads do not implicitly
advertise: compose an explicit sharing handle over that store if desired.

## Permission And Accounting

Every metadata and exchange entry requires a trusted `authenticated_caller`,
including generated local typed calls. Direct callers must explicitly provide
binding-created trusted caller context. Request DTOs never supply identity.
`access_kind::metadata` and `access_kind::content` let products distinguish
permission/budgets. Hash requests use the content permission. Missing/null,
failed, mismatched, revoked or expired grants fail closed as the existing
`api::core::exceptions::invocation_denied`. Free service requires `free_admission`.

Policies return the **same shared quota object** for sessions spending the same
content/requester product budget. Reservation updates are atomic across sessions.
Runtime quota units are descriptor chunk payload bytes, block payload bytes, and
hash/proof material (32 bytes per hash including each sibling). Wire headers and
control messages are not charged; they still consume bounded runtime/API capacity.
This is not a payment, receipt, fraud-detection or settlement service.

Allowance is reserved before payload disk/CPU preparation (the first metadata
chunk before descriptor encoding), attempted immediately before async output,
and marked queued only after successful API queue admission. A reservation that
was never attempted is returned; an attempted write is charged even if its
delivery/queue outcome is unknown. Destruction/move finalizes exactly once.
`requested`, `attempted`, `queued`, `uncertain`, `sent`, `received` and `verified`
are distinct diagnostic facts. **Runtime leaves `sent` at zero**: API queue
admission does not confirm transport transmission or useful remote delivery.

Revocation synchronously interrupts accepted calls. Expiry and call deadlines
interrupt them through owned cancellable notification waits. Already handed-off
API/transport bytes cannot be recalled. Per-call shallow API queues, resource
reservations and one active payload producer bound the exposed in-flight amount.
Callbacks must honor their supplied stop token AND deadline. Runtime requests
cancellation and joins late callback completion; it cannot forcibly terminate
arbitrary product code or an entered OS syscall.

## Bounds And Current Limits

Resource tickets atomically reserve global, operation and peer calls/items/bytes/
unfinished pieces. Inbound peer keys use the full trusted source/fingerprint and
are shared across operations. Before connecting, outbound work uses a temporary
connection key. The scope ticket is then atomically rebound to the adapter's
authenticated `connection::remote_identity()` BEFORE opening exchange. Route
aliases cannot create independent budgets for the same peer, and failed rebinding
retains the original hold until cleanup. Adapters must bound their own queues.
Tickets cover full-piece store read/verification working memory, not just the
requested range, and follow received blocks through writer completion.

Temporarily occupied payload capacity is backpressure, not a peer fault. An
unsent pick is rolled back before waiting for the common budget or operation
state to change. Request plus new-piece capacity is reserved atomically, then
split into ownership tickets without changing counters or notifying waiters.
A failed probe releases nothing and cannot wake another failed probe. If a new
piece is blocked, Core can select only already admitted partial pieces using
its additive allowed-piece filter, without altering inventory or priorities.
Release/rebind wakes waiters across operations sharing the authenticated peer.
The post-rollback predicate checks combined request/piece capacity. Empty piece
owners left by choke/reject are retired only without a writer or any active
request/receive owner; active I/O retains its full piece ticket. `capacity_waits`
counts admission waits, while `capacity_waiters` is the current number waiting.
Waiting is cancellable and bounded by `request_timeout` without useful operation
progress and by the fixed total deadline. This local wait pauses peer-idle;
control input does not extend the admission deadline. Work exceeding configured
capacity fails immediately; allocation/store errors are not retried as capacity
pressure. Product quota exhaustion remains denial, not this local resource wait.

Each peer has independent reader/writer coroutines and at most one outstanding
download block. Core picking/verification generations remain shared per operation;
a per-piece gate serializes writer operations. Upload candidates/slot decisions
use one shared Core choker per operation. Its byte samples are successfully
queued serving work, not transport receipts. Short state locks never cover
network, policy, CPU or disk awaits. Bounded children do not occupy additional
scheduler slots while their parent waits.

Normal input half-close drains accepted serving commands, closes response output
and returns successful API completion. Download completion half-closes requests,
drains response EOF and awaits the API terminal result; explicit cancellation is
separate. Connection and provider-registration capabilities retain their unique
ownership through awaited close, including initialization failures. Orchestration
capacity is explicitly released before worker completion, not by eventual task
callable destruction.

Provider lookup is currently an initial bounded snapshot; reconnect attempts and
request timeouts are bounded. `options.attempts` counts consecutive reconnect
rounds without new owned stored-block progress; progress resets that count.
Integrity failures have a separate per-download bound across all reconnects.
Local writer creation/write/non-integrity commit failures are terminal after
rollback and child drain. Discarded partial writes do not reset retries forever.
`idle_timeout` refreshes only for useful progress, not duplicates/control input;
owned CPU/disk/gate backpressure pauses this idle clock. Grant expiry and the
optional final `async_download(..., routes, total_deadline)` argument remain
fixed and are never extended by progress/backpressure/reconnect. No implicit
absolute 30-second API call deadline is installed. A total deadline also cancels
a pending scheduler worker through an owned, joined notification waiter.
There is no continuous topology maintenance,
paid entitlement negotiation, billable accounting persistence, adaptive throughput
tuning, or transport qualification. Large descriptors/pieces may require explicit
resource-limit increases; defaults do not promise every Core maximum concurrently.
Resume picking trusts only the store's revalidated verified bitmap; unfinished
staging ranges can be safely fetched again rather than advertised as complete.

Selection uses private Crypto Core randomness by default; RNG errors propagate
without a constant-seed fallback. `options.selection_seed` provides reproducible
tests. Operation, picker and choker seeds are derived separately from control IDs;
seeds change ties, not Core priority/rarity ordering.

The unit tests live in `tests/swarm_runtime`. Separate coordinator-owned integration
tests exercise concrete TCP/file-store adapters; those dependencies are not part
of this target. Build/test/publication acceptance must be reported independently.
