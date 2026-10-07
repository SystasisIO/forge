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
The original donor must remain unchanged in canonical acceptance; an auxiliary
instrumented build, diagnostic-string classification or retries until a green
run cannot promote this gate. A different donor pin or public observation
contract requires a separate reviewed decision.

The native cache-pressure regression observes completed IWANT I/O without
blocking the transport strand, then sends a verified message on a distinct
stream. The exact staged-fulfillment-before-activation race is deterministic
in the router entity test; it is not claimed as a pinned native scheduling race.

Local raw results establish observations only. Final acceptance additionally
requires clean exact-head source/binary/donor provenance, indexed terminal
snapshots, distinct owned processes and joined shutdown. This note does not
declare the live gate passed. PR12 extensions and Stage 8 production soak are
separate work.
