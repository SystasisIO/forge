# GossipSub Scoring And Routing

## Pinned Sources

- Go pubsub `0ed6f6fdad7eb820486892336bed3081f8fa7f25`:
  `score.go`, `score_params.go`, `gossipsub.go`, `gossipsub_feat.go`,
  `mcache.go`, `gossip_tracer.go`, `pubsub.go` and their focused tests.
- Rust libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`:
  `protocols/gossipsub/src/peer_score.rs`, `peer_score/params.rs`,
  `behaviour.rs`, `config.rs`, `mcache.rs` and `gossip_promises.rs`.
- libp2p specs `6b6203ee`: GossipSub v1.0/v1.1 mesh, score, gossip,
  PRUNE/PX and validation rules. The full revision is locked by the interop
  fixture manifest, not inferred from the current donor checkout.

Donors remain implementation references, never Forge build dependencies.
Interop fixtures consume immutable exports of the locked revisions. Neither
donor router is patched for PR11.

## Accepted Composition

`detail::pubsub_peer_score` owns bounded P1-P7 peer/topic counters, validation
generations, indexed retention and decay. `detail::pubsub_router` owns router
random selection and outstanding gossip promises. The existing node owns
heartbeat-window message history, subscriptions, sessions, validation tasks,
negotiated streams, heartbeat and admitted outbound queues. No plugin network
loop is introduced.

The score engine receives authenticated direct IP facts, not advertised hints
or a relay's IP substituted for the remote peer. Exact-IP P6 and fractional P1
follow Rust; Go's integer P1 quanta are not an exact floating-point oracle.
Application P5 callbacks execute outside both node and score locks. A late
clock sample from a concurrent caller does not roll back logical engine time.

Only committed rejection or invalid native input is malicious. Ignore does
not forward or penalize; retry, cancellation and local queue pressure remain
neutral. Pending delivery records attribute valid/invalid duplicates by peer
generation. Disconnect preserves negative state until the retention deadline.

Mesh repair uses score eligibility, score-selected survivors, random remainder
and physical outbound constraints. A peer subscription is not a mesh edge.
Flood publishing, local fanout and relay of accepted messages are distinct
paths. PRUNE serialization follows the actually selected stream version;
v1.0 excludes v1.1 PX/backoff fields.

History advances per heartbeat, not per inserted message. IHAVE eligibility
and IWANT follow-up penalties follow donor thresholds. A promise belongs to a
successfully sent request, not merely a locally assembled RPC.

Go's `RPC.split` and oversized-gossip handling split IHAVE/IWANT by IDs rather
than discarding a complete advertisement when it exceeds the wire envelope.
Forge's codec owns a lazy cursor and allocation-free size calculation over the
existing Multiformats varint mechanism. One bounded per-peer dispatcher owns
the original ephemeral data; durable controls remain independent of its
admission. Each actually sent IWANT chunk samples from its own normalized IDs.

Remote ephemeral gossip carries its originating authenticated session separately
from the selected outbound stream. Admission, request outcomes and failure
attribution check that original owner under the same mutation lock. Retirement
drops stale ephemeral work without suppressing independent durable controls or
hiding an actual native write failure. Cached replies send one moved MESSAGE
per bounded RPC; a valid cache entry is not combined into an invalid aggregate.

Both donors process every structurally accepted GRAFT entry. Forge therefore
uses `max_graft_per_peer` only as an outbound batch quota, with PRUNE priority
and durable residual GRAFT intents. Receive-side byte/count, topic, score,
backoff and mesh-capacity checks remain in force; no wire field changes.

The receive boundary must enforce the pinned PubSub signature policy before
message-ID, cache, promise fulfillment or application validation. StrictNoSign
forbids the protobuf `signature`, `key`, `from` and `seqno` fields themselves,
including encoded-empty and repeated fields. StrictSign requires an eight-byte
big-endian sequence number. Go's legacy author-metadata allowance without
`WithNoAuthor` is not the normative StrictNoSign contract. Raw codec and
cryptographic helper vectors are separate from native wire-policy validation.
Policy rejection is a per-message P4 event, not a malformed-RPC strike; valid
siblings remain usable. The complete protobuf structure and total message count
still require validation, including rejected messages and malformed suffixes.

Messages signed by the receiving node but replayed by a foreign authenticated
peer are rejected as self-origin before cache lookup and promise fulfillment.
Go `pubsub.go`/`score.go` and Rust `behaviour.rs`/`peer_score.rs` attribute this
rejection to the forwarding peer's P4, not the local author or transport's
malformed budget. The native regression expires the original cache through
actual heartbeat shifts before replay and retains a healthy foreign delivery
check on the same connection.

## Evidence And Limits

The planned live gate uses four actual processes, real authenticated sessions,
forced v1.0/v1.1 negotiation and original framed read/write bytes. It requires
off-mesh IHAVE/IWANT cached recovery, neutral ignore, committed P4 rejection,
threshold crossing, PRUNE/GRAFT and delivery through a replacement mesh peer.
It is not a manual mesh script or a codec-only test.

Signed message IDs are raw author PeerId plus eight-byte big-endian sequence,
the Forge/Go default. Rust uses its supported public `message_id_fn` setting to
join this application policy. Its textual native default is different.

Rust's public `Behaviour` exposes scores and mesh membership, but not P4
counters. The fixture explicitly records unavailable counters as null; it does
not reverse-engineer counters from the score. Native decision/score transitions
and actual traffic remain required.

The pinned Rust QUIC transport also wraps a Quinn connection-close cause in a
private transparent `ConnectionError`. When the public error chain does not
expose the typed cause, the observer records `quic_connection_cause_unavailable`
as a failure. Prepare acknowledgements, local close intent, another stream's
earlier error and a later successful close are not substitutes for that cause.
Normal protocol traffic alone does not close this shutdown evidence gap.
The maintainer approved a scoped observation-contract change on 2026-10-08.
Original canonical donor exports remain unchanged. Each of the four Rust QUIC
version/direction cases supplies two independent executions: original active
wire/scoring proof and an explicitly instrumented full shutdown proof. The
original terminal errors, exit codes and snapshots are retained; original
shutdown remains `NOT_PROVEN`, never explained by another execution's cause.

The isolated TEST COPY adds only `ConnectionError::inner() ->
&quinn::ConnectionError`. No router, polling, error outcome, close, dependency
or wire behavior is modified. A separately enabled fixture feature reads the
current owner's error through that accessor. The fixed Git tree, exact patch,
before/after file membership and hashes, separate binary and Cargo commands are
independently checked. Every other donor source must remain byte-identical.

The original active gate validates all four indexed Prepare snapshots and
their unchanged terminal event prefixes, authenticated owners, selected
protocols, real paired RPC bytes and full causal scoring/repair/delivery proof.
The companion gate repeats the complete scenario with distinct tokens/PIDs and
requires actual native owner disposal, zero resources, joined tasks/processes
and attributable typed shutdown. Nonzero close, reset, timeout, pre-Prepare,
foreign or unavailable causes remain fatal. Text classification, borrowed
cross-run causes and retries until a green run remain forbidden. This is a
combined original-wire/instrumented-shutdown acceptance, not a claim that an
unmodified Rust transport publicly exposed the hidden reason.

The native cache-pressure regression observes completed IWANT I/O without
blocking the transport strand, then sends a verified message on a distinct
stream. The exact staged-fulfillment-before-activation race is deterministic
in the router entity test; it is not claimed as a pinned native scheduling race.

Local raw results establish observations only. Final acceptance additionally
requires clean exact-head source/binary/donor provenance, indexed terminal
snapshots, distinct owned processes and joined shutdown. This note does not
declare the live gate passed. PR12 extensions and Stage 8 production soak are
separate work.

Go whole-owner teardown follows `pubsub.go`'s root-context termination, rather
than live `Subscription.Cancel()`/`Topic.Close()` calls. Subscription cancellation
can still select the live cancellation channel after context cancellation;
topic close requires a live process loop. Neither belongs to disposal of the
entire fixture owner. Host close, stream resets, callback/worker joins and sticky
native failures remain required. The fixture does not claim a public join of
Go's internal router goroutine, which the pinned donor does not expose.

`comm.go` starts sender and peer-dead goroutines independently: sender defers
Close while peer-dead calls Reset. `pubsub.go` can reopen an outgoing stream
while the host remains connected. Fixture command Prepare therefore cannot be
treated as PubSub termination. Before any host Stop, all Go actors separately
cancel PubSub, close open/write and application-callback admission and join their
actual fixture-owned I/O/framing/subscriber/disposal work. Native-error and
network observation remain enabled. The controller and host stay alive until
all Go quiesce ACKs have been checked. This fixture ownership barrier changes no
donor router code and does not claim a join of its private goroutines.

The barrier does not order the donor's concurrent Close/Reset calls. Native
Close errors remain errors; only the existing exact returned-Reset-before-Close
observation rule applies. A raw TCP reset, prior failure or malformed frame
cannot be explained away by preparation, quiesce or later cleanup.

The focused native teardown regression uses the existing Asio gate to exercise
both an already-queued acquire and stop before coroutine execution. It separately
holds the tracking ticket until a sticky notification releases it. The prior
timer-based test signaled readiness before registering its wait and could lose
cancel; replacing that test is not evidence of a native node shutdown defect.
