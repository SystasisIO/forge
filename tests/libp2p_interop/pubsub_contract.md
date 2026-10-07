# PR11 Native PubSub Actor Contract

This is a fixture interface, not an acceptance result. Donor source remains
immutable. Actors use the real router with scoring and application validation.
The coordinator controls connectivity and publications, never mesh membership
or score values. Native observations are captured without changing decisions.

## Command And Ownership

`pubsub-live` accepts unique flag/value pairs:

- `--version`: `1.0` or `1.1`; exactly one standard protocol is offered.
- `--transport`: `quic`, `tcp`, or `tcp-pnet-noise`.
- `--actor`: `victim`, `offender`, `replacement`, or `sink`.
- `--case-token`: 32 lowercase hexadecimal digits.
- `--ready-file`, `--control-file`, `--result-file`, `--stop-file`, `--store-dir`.
- Private actors additionally accept `--pnet-key-file` and `--pnet-fingerprint`.

Each process owns its host, cancellation context, subscribers and observer.
Readiness follows listener creation and local subscription. Results are
atomically replaced during operation; only the post-shutdown result is final.
The final result must report callback/worker joins, trace overflow and failures.

All three implementations publish the same readiness fields: integer `schema_version=1`,
`implementation`, `actor`, `case_token`, equal nonempty `local_peer_id`/`peer_id`
from the actual native host, one actual `listen_addrs` entry, canonical
`topic=forge-pr11:<case-token>`, boolean `ready=true` and
`subscription_created=true`. These booleans are emitted only after native
listener creation and successful local subscription. The runner requires the
same identity/schema/implementation/actor/token in an active result snapshot
before issuing commands. `status="ready"`, `subscribed=true`, truthy strings
or numeric booleans are not readiness substitutes. Readiness does not prove
stream authentication, protocol negotiation or causal repair; those still need
the indexed native receipts below.

Control file is an append-only JSONL list with contiguous `sequence` and `kind`.
Commands are `connect` with `peer_id`/`address`, `publish` with `payload`,
`sample` with `label`, and `prepare_shutdown` with `actor`, `case_token` and
`local_peer_id` matching the actual ready host. Go additionally accepts one
`quiesce_shutdown` lifecycle command immediately after Prepare, with the same
identity/token and the exact `prepare_ack_sequence`. Completion includes that sequence in a `command_done`
event. Invalid, duplicate or oversized commands fail the actor. Maximum 64
commands, 16 KiB per line, 1 KiB payload and 2,048 observation events.

## Causal Shutdown Barrier

After repaired delivery, the runner appends a real `prepare_shutdown` command to
each of the four actors. Each actor checks sticky errors/overflow and pending
fixture commands before closing command admission and emitting exactly one
`shutdown_prepared` acknowledgement. Its exact source is
`<implementation>.fixture.prepare_shutdown`; fields bind `command_sequence`,
`actor`, `case_token`, actual `local_peer_id`, boolean `admission_closed=true`
and integer `pending_commands=0`. The matching `command_done` follows it with
`command_kind=prepare_shutdown` and explicit `status=ok`. No later fixture
application command, duplicate prepare or partial appended command is admitted.
Only Go's exact, one-time quiesce lifecycle command is allowed. Native
router/stream observation continues unchanged while prepared.

The runner observes all four exact acknowledgements, then rechecks every active
result before requesting ANY normal stop. It publishes every Go quiesce request
and waits for every matching native `shutdown_quiesced` acknowledgement before
any host Stop. The Go PubSub context is independently cancelled while the host
and controller stay alive. Native open/write admission and PubSub callback
admission close; admitted I/O, framing, pending terminal operations, subscriber
work and callbacks must actually drain before ACK. Rejected native streams
remain accounted through their actual Reset return and disposal receipt. Reset
errors and prior sticky failures prohibit ACK. Network/native-error observation
stays enabled; closed application callback admission does not hide errors.

The quiesce ACK binds the actual PID, identity/token, command sequence and
preceding Prepare ACK. It proves only the named fixture-owned drain, not a join
of Go's private router goroutines, Host.Close or process completion. Python
independently rejects missing/foreign ACKs, remaining owners and actual native
PubSub writes or newly selected owners after ACK. Logical RawTracer submission
alone is not a native write. A timeout cannot manufacture a successful ACK.

`shutdown_barrier.operations` records four identity/token/command/ack-event-bound
`prepare_ack` observations, all Go `quiesce_requested` rows, all Go `quiesce_ack`
rows, then all
Go/Rust `stop_requested` publications before waiting for any donor. Forge remains
running until every donor actually exits zero gracefully with clean final raw,
unchanged Prepare prefix, native host close and zero joined I/O/worker owners.
Each verified donor adds one `donor_joined` row with exactly `sequence`, `kind`,
`actor`, `case_token`, `local_peer_id`, actual `pid` and `shutdown_event_sequence`.
That reference binds the same donor's unique post-Prepare native `shutdown`
(Go) or `shutdown_requested` (Rust) event; it is not itself proof of process join.
Indexed terminal raw, native owner counters and the actual process result must
independently agree. Only after all donor joins are observed may the remaining
Forge `stop_requested` rows be published. Contiguous coordinator sequences prove
this order, never comparisons between different processes' monotonic clocks.
Missing, duplicate, foreign or reordered stop/join receipts, delayed/failed
donor joins and nonzero/forced exits cannot release normal Forge stop. The
existing native stop budgets, error classification and donor router behavior remain
unchanged; donor-first scheduling does not guarantee a successful exchange.
Missing, foreign, ambiguous or timed-out acknowledgement is `HARNESS_ERROR`.
Failure cleanup may still stop processes, but cannot manufacture a successful
barrier or acceptance. Final actor events must retain the exact ACK; preparation
is NOT native close, resource release, worker join or successful process exit.
Those actual terminal obligations remain independently required. Quiesce also
does not serialize the donor's concurrent Close/Reset paths: an earlier or
concurrent Reset does not qualify as a returned Reset before Close BEGIN.

Every `prepare_ack` also references its distinct canonical
`<actor>.prepare-result.json` through `evidence_file`. Before any stop, the
runner exclusively creates this bounded snapshot with the actual owner PID,
actor/token and exact command/ACK sequences, preserving the active native
result without rewriting its events. Acceptance loads all four snapshots via
the verified evidence index, independently validates active ACK/completion and
requires their exact event prefixes in the indexed terminal results. A manifest
reference or final ACK alone cannot replace a missing preparation snapshot.

Rust native I/O wrappers return the original native result unchanged. Muxer
wrappers preserve Pending/Ok/Err and successful values; the PubSub-only Yamux
public error conversion below is the explicit error-wrapper exception.
Every observed error retains bounded owned operation, outer `io_kind`,
`raw_os_error`, diagnostic message, `typed_cause` and boolean `prepared`.
Unexpected errors emit `native_io_error` and remain sticky/fatal before AND after
prepare. Only an owned QUIC error with a public typed
`quinn::ConnectionError::ApplicationClosed` code zero or `LocallyClosed` may emit
`expected_native_close` after preparation without introducing a new failure.
Public `ReadError/WriteError::ConnectionLost`, direct `io::Error::get_ref()` and
`StdError::source()` chains are inspected with an eight-node bound; diagnostic
text and `io::ErrorKind` are never used to infer normal closure. Nonzero close,
reset, timeout, transport errors, arbitrary errno 38 and hidden/unclassified
causes remain fatal. The pinned libp2p QUIC connection wrapper has a private
field and transparent `source()`; it does not expose the public Quinn variant
for normal connection closes. That evidence gap remains fail-closed, not a
permission to classify the wrapper or its text as normal. Locked Quinn
`ApplicationClose` is not itself `StdError`. The fixture's direct Quinn dependency
is exactly the already locked 0.11.11 with default features disabled.

The maintainer-approved 2026-10-08 observation contract separates the four
Rust QUIC cases into original-wire and instrumented-shutdown gates. An isolated
copy of pinned Rust adds only a read-only `ConnectionError::inner()` accessor;
the fixture enables `quic-cause-observer` only for its separate binary. Complete
original/observed Git trees, the exact patch and all source/binary hashes must
validate. No native result, polling, close or router behavior changes.

Original traffic is independently checked using four active indexed Prepare
snapshots and their unchanged prefixes in the original terminal snapshots. It
requires the same authenticated RPC, score, repair and delivery checks, not
merely a Prepare ACK. Original errors and exit1 remain diagnostic artifacts;
the verdict never claims original shutdown success. The companion runs the
entire scenario with independent tokens, identities and PIDs, verifies its own
current typed errors and meets all existing successful shutdown obligations.
Canonical promotion requires both gates; neither can borrow another run's
cause, owner or receipt. No observer build is allowed for Go or TCP/private
cases. Original donor repositories and canonical exports remain unmodified.

Only PubSub TCP/PNET opts into `libp2p_yamux_public_into_io` before core's generic
StreamMuxerBox boxing. A clone-local opt-in reuses the existing transport factory;
all other fixtures and QUIC keep their original boxing. The passive upgrade
observer records the original native error before the public consuming
`From<libp2p::yamux::Error> for io::Error` conversion. Native Io retains its
io::Error; other variants expose the exact locked yamux 0.12.1/0.13.10 public
ConnectionError. Subsequent generic boxing can change the outer kind/errno,
so original-wrapper identity is not claimed. No poll is retried or changed
from Err to Ok, and no substream I/O error is normalized.

The converter pins the native error's public source-presence before consuming
it: pinned native Io and Decode expose a source, while Closed does not. Only
source-less native errors whose public conversion yields the exact Closed enum
receive a private ConversionState mark retaining that converted io::Error.
Ordinary Io/Decode is forwarded as converted, including native Io whose
underlying error happens to contain a bare public Closed enum. A native Io
containing an earlier private mark instead receives a denying ConversionState
boundary retaining its converted error, so no prior terminal cause is inherited.
Observers accept only this adapter's current positive mark, never a bare or
inherited Closed type. The private mark changes error-wrapper shape, not the
native outcome; it preserves the converted inner error and bounded diagnostics.

An actual typed Closed muxer error after the owner's preparation may emit
`native_terminal_state`, with `typed_cause=yamux012_closed` or `yamux013_closed`,
`error_boundary=libp2p_yamux_public_into_io`, `closure_reason=unknown` and the
exact prior `prepare_ack_sequence`. It requires the successful authenticated
Noise/Yamux output, its actual Swarm binding, no prior sticky error/overflow,
and a muxer operation without a fabricated stream owner. Only at most eight
nested io::Error wrappers are inspected; opaque wrappers and non-Closed enums
are never skipped through source(). Io, Decode, pre-Prepared, wrong/unbound
owner and substream errors remain fatal. Closed carries no graceful cause:
both pinned versions collapse every GoAway code to Closed. It does not prove
GoAway0, peer cleanup, successful native close, resource release or task join.
Python independently matches the exact receipt fields, source, opt-in native
owner/stack/endpoint, prior actor ACK, all-four-actor barrier and actual final
owner disposal/zero-resource/task-join evidence. Active preparation snapshots
cannot already contain this terminal state. PNET also retains the exact native
group fingerprint requirement. Diagnostic messages never classify a cause.

Wire protocol mismatch, malformed RPC, incomplete receipt and all parser/buffer
bounds are ALWAYS fatal, including after preparation or expected native close.
Prepare never clears earlier errors. Exact native muxer close/terminal records,
zero live resources, actual fixture task joins and process exit zero are still
required; ACK alone proves none of them.

Go stream wrappers own read/write decoder finalization across EOF/error,
CloseRead/CloseWrite, Close, Reset, ResetWithError and host drain. Native calls
hold no observer/drain/wrapper mutex; close/reset never waits for direction
tokens. Drain resets every retained wrapper before joining admitted I/O and
completed decoder finalization. Each decoder is finalized once, after its
in-flight native prefixes have been captured. Nonzero residue emits bounded
owned `incomplete_rpc_frame` from `go.pubsub.native_stream.finalize`, with
operation/direction, pending bytes, typed cause and preparation flag, and sets
an unconditional sticky failure. Empty EOF at a frame boundary is neutral.
All delegated native results remain unchanged; reset errors are retained and
cannot establish disposal/join. Half-close does not release the opposite
direction or the native owner. Python independently rejects nonzero, absent
or non-integer terminal residue and every typed incomplete record.

Go native operation receipts use `go.pubsub.native_stream.native_operation` and
retain wrapper-local `started_order`/`returned_order`, exact owner/protocol,
native outcome and bounded diagnostic. `prepared` and `prepare_ack_sequence`
describe the operation BEGIN, not its return. Read/Write begun before prepare
retain false/zero even when an owned Prepared Reset later interrupts them.
Full Reset's native RETURN phase, counter and original error are recorded
atomically under the wrapper mutex before receipt publication. Publication only
adds that attempt's event sequence; it cannot overwrite the latest START identity.
The closing/capture count stays owned until publication finishes, so deferred
capture cannot produce an early finalizer/join. Native calls remain outside locks.

Only an authenticated native TCP/Noise/Yamux stream's direct local
`network.StreamError{0,false}` with a direct local
`TransportError=*yamux.StreamError{0,false}` can become `owned_reset_pending`.
The actual full `Reset` on that same wrapper must BEGIN in Prepared before the
I/O RETURN observation. Its nil RETURN may follow I/O RETURN because native
Yamux notifies waiting I/O before Reset returns. The candidate pins that Reset
attempt immediately; a later/foreign Reset cannot reclassify an earlier error.
The immutable `stream_io_terminal` records outer and direct transport type,
code/remote fields, original successful prefix/residue and `reset_started_order`.
No diagnostic text, `errors.Is/As`, wrapped/joined cause or prior cause is used
to classify normal termination. Bare reset, nonzero, opaque, mismatched inner
cause, pre-Prepared remote reset0 and TCP ECONNRESET remain fatal.

Separate `native_stream_io_finalized` receipts link the unchanged raw operation
to the exact successful Reset event/return counter via
`operation_receipt_sequence`, `causal_reset_receipt_sequence` and
`causal_reset_returned_order`. Acceptance requires completed native I/O, both
decoders finalized/clean with zero residue and actual native owner disposal.
Every accepted finalizer, including peer cancellation, independently references
`owner_disposal_receipt_sequence`: a real successful full Close/Reset receipt
on the same authenticated connection, stream and protocol, published before
the finalizer. A boolean `native_owner_disposed`, half-close, ACK or receipt
from another owner cannot establish disposal. Disposal does not establish the
physical cause of the earlier I/O error.
Pending/failed Reset, unfinished I/O, malformed/partial RPC or capture overflow
cannot yield an accepted finalizer. Receipts and pending state share the existing
2,048-event bound. Native results are always forwarded unchanged, and no first
failure is cleared even if a separate orderly operation is observed later.

`owned_read_terminal_pending` is the distinct read-only observation of the
pinned Yamux parser's exact direct two-sentinel pair: `network.ErrReset` and
`yamux.ErrStreamReset`. It contains neither remote provenance nor a reset code;
`terminal_state_cause` stays `unknown`. Only an actual authenticated
TCP/Noise/Yamux wrapper whose latest successful Close/CloseRead/full Reset
began in Prepared and returned before the native Read RETURN can emit it.
An earlier Read BEGIN keeps its original false/zero preparation fields.
`terminal_started_order`/`terminal_returned_order` identify the native operation,
and `terminal_receipt_sequence` may be zero while publication is still pending.
The finalizer must bind its real successful receipt through
`observed_terminal_receipt_sequence`, plus the separate full disposal receipt.
A superseding or failed terminal operation, unrelated wrapper, earlier sticky
failure, overflow or incomplete frame is fatal. No recursive error matching,
diagnostic-text matching or attribution of the physical cause is performed.

`peer_zero_reset_pending` is a separate observation of peer cancellation with
UNKNOWN reason, not evidence of remote cleanup or a causal remote Reset/ACK.
Only the same authenticated TCP/Noise/Yamux wrapper's direct
`network.StreamError{0,true}` with direct `TransportError=*yamux.StreamError{0,true}`
is eligible after actual local preparation at RETURN and without an existing
failure/overflow. `prepared`/`prepare_ack_sequence` still describe BEGIN and may
remain false/zero for a long read; separate `terminal_prepare_ack_sequence`
records the actual published local ACK at RETURN, and `peer_reset_reason` is
exactly `unknown`. A future ACK never reclassifies an earlier error. Native n/error
are unchanged. The final `native_stream_io_finalized` copies these fields and
requires both decoders clean/finalized, zero residue, actual I/O join and native
owner disposal. All Reset references remain zero; no `causal_reset_*` fields are
allowed for this observation. Missing/duplicate finalizers remain fatal.
Python verifies the exact direct typed fields and authenticated stream owner,
the ACK preceding the terminal receipt and the actual four-actor preparation
barrier (including the observed peer's actor). Indexed active prepare snapshots
cannot already contain peer-cancellation candidates. This is an observation
policy after preparation, not a claim that cancellation was caused by shutdown.
This TCP observation does not authorize QUIC; other native errors, framing
errors and prior sticky failures are unchanged.

`repeat_close_pending` is distinct: a Prepared Close may preserve a direct raw
Yamux StreamError0 (including cached Remote0) only after this wrapper's successful
full Reset has RETURNED before Close BEGIN. Its separate
`native_stream_close_finalized` still requires clean joined framing and disposal.
At Close BEGIN the Reset must also be the latest START identity: a newer full
Reset or ResetWithError begun earlier prohibits fallback to an old success.
A newer START after Close BEGIN cannot invalidate its pinned attempt. An
unpublished Reset permits only a pending Close with immutable known Reset
start/return counters and `reset_receipt_sequence=0`. Its finalizer must link the
exact subsequently published nil-Reset receipt on that wrapper. Python checks
Reset RETURN < Close BEGIN and both receipts before finalization, not Reset
publication before Close publication; a nonzero raw Reset reference must still
match exactly. Missing/duplicate finalizers and scalar-type substitutions fail.
This Close rule does not itself authorize remote Read/Write errors or prove a
remote causal Reset by comparing process-local Stream IDs. Python independently verifies exact source,
authenticated carrier/substream owner, typed fields, local counters/phase,
immutable candidate/finalizer links and the indexed native prepare ACK. Every
candidate requires exactly one matching accepted finalizer; bool/int and
float/int substitutions are rejected. The four-actor barrier, real host/task
join and successful native process exits remain separate obligations.

## Go Lower QUIC Evidence

QUIC RPC authority is the actual returned CapableConn/MuxedStream, not a Swarm
stream-ID mapping. `go.quic.transport.capable_output` binds the exact native
connection to its InterceptSecured callback, peer identities, addresses and
remote public-key digest. The upper authenticated connection independently
references that lower receipt. `go.quic.CapableConn.stream_return` retains the
native bidirectional StreamID (including zero). Actual multistream frame
references prove both headers, role-correct proposals, bounded `na` fallback
and the matching ACK before `go.quic.native_stream.selected` and RPC bytes.
The live subscription predicate and final checker share this same owner
validator. Exact lower sources use `remote_peer_id`; conflicting identity
fields, fabricated aliases, duplicate owners and bool/float references fail.
The existing TCP/PNET sources and authentication rules remain separate.

Native Read/Write receipts preserve the original successful prefix and error.
BEGIN preparation fields are immutable; `terminal_prepare_ack_sequence` records
the actual RETURN phase, allowing a long Read begun before Prepare. Indexed
Prepare snapshots bind the same native owners and live connection baseline.
Direct local zero StreamError candidates require the same owner's Prepared
terminal operation START before I/O RETURN, eventual nil native RETURN and
exact final receipt. Peer-zero candidates require the direct outer/transport
zero StreamError pair with matching native StreamID/Remote fields and live
send/connection Prepare baseline. Their cancellation reason is unknown, never
remote cleanup or causal ACK. Write requires its current identical send cause;
Read cannot borrow a send context. Direct zero connection-application errors
require the same connection's actual typed terminal context, not a prior Read.
Every actual peer remains bound to the four-actor Prepare barrier.

An opaque native Close result can only be annotated by its own current direct
typed-zero send context on the same StreamID at actual Close RETURN. Public
typed, sentinel, wrapped, joined and errno errors remain fatal. The annotation
is `native_send_reset_close_pending`, never a graceful cause or replacement
error. Eligibility requires a live peer-cancellation Prepare baseline or the
same owner's successful Prepared full Reset native RETURN before Close BEGIN.
The pinned latest START cannot be replaced by a newer attempt; native counters,
not receipt publication order, establish this relation. Diagnostic strings do
not classify any outcome.

For `owned_read_terminal_pending` only, one intervening failed RepeatClose does
not supersede a successful full Reset when both independent terminal finalizers
reference that exact Reset on the same authenticated owner, protocol and Prepare
ACK. Native counters must prove Reset RETURN < Close BEGIN < Close RETURN < Read
RETURN, even if Reset/Close receipts publish after the Read receipt. The Close's
complete native-send-context and finalization checks must pass before this link
is accepted; `accepted=true` alone is not proof. Its send context annotates only
the original Close error, never the Read cause or graceful shutdown. Any other
superseding Reset/ResetWithError/terminal operation remains fatal.

Every pending receipt requires exactly one immutable
`native_quic_terminal_finalized` link. `native_quic_framing_finalized` separately
requires complete negotiation, both decoders finalized with zero residue,
actual native I/O join and an indexed successful full Close/Reset disposal on
the same owner/protocol. Half-close or a disposal boolean is insufficient.
`native_quic_join` checks actual observed owner counts and zero active native
calls; it does not claim every private donor goroutine joined. Host close,
fixture-worker join and process exit zero are still independently required.
Prior sticky failures, pre-Prepared/nonzero/foreign/opaque I/O, malformed RPC,
overflow, missing/duplicate finalizers and unfinished disposal/join remain
fatal. Active indexed Prepare captures cannot contain these terminal receipts.
No donor error, native return or Rust QUIC classification is changed.

### Unselected Lower QUIC Diagnostics

`native_quic_negotiation_io_return` has source
`go.quic.native_stream.io_return`. Unselected `native_stream_operation` retains
source `go.quic.native_stream.operation_return`; its Close context retains
`native_quic_send_context` / `go.quic.Stream.Context.at_Close_return`.
Only these exact kind/source pairs can carry
`observation_phase=unselected_at_native_return` and
`protocol_at_native_return=""`. The common `protocol` is the capture-time
selection and may become nonempty when successful-prefix observation selects
the protocol after the sealed native RETURN. That value still requires the
actual indexed proposal/ACK/selection receipts; it is never inferred from a
configured protocol or a lazy application tail.

All diagnostic receipts retain the common native owner fields and immutable
`started_order`, `returned_order`, BEGIN `prepare_ack_sequence`, and
`send_context`. I/O additionally retains `operation`, `direction`,
`requested_bytes`, `successful_prefix_bytes`, `successful_prefix_valid` and
the original `error`, `error_type`, `outcome`, `typed_cause`. Close/Reset uses
the same error fields, `operation` and `requested_reset_code`; Close also has
`native_close_error_classification` and its actual indexed
`send_context_receipt_sequence`. A diagnostic Close context contains
`observation_basis=same_lower_delegate_Context_at_actual_Close_RETURN` and
`connection_context`, not a fabricated negotiation snapshot.

I/O and operation diagnostics additionally contain RETURN
`terminal_prepare_ack_sequence`, capture-time `prepare_snapshot_ack_sequence`,
`prepare_baseline_present`, `stream_prepare_baseline_present`,
`connection_prepare_baseline_present`, `send_context_at_prepare`,
`connection_context_at_prepare`, `connection_context`,
`context_observation_basis=send_context_after_seal_and_parent_context_sealed_at_native_return`
and `negotiation_snapshot`. Each nonzero ACK resolves to the actual owned
Prepare receipt and completion; no later ACK retrofits BEGIN or RETURN. A
late-born diagnostic stream requires its real preceding ACK and the indexed
parent connection's actual Prepare baseline. Its absent stream baseline stays
absent; the zero-valued context is not evidence of a live stream at Prepare.
An unindexed parent baseline is an evidence gap, not permission to invent one.

`negotiation_snapshot` has exactly `capture_complete`, `selected_protocol`,
`proposal`, `reply`, `touched_pubsub`, `parser_failed`, `proposals`,
`frame_sequences`, `snapshot_basis=parser_state_after_successful_prefix_observation`,
`read`, `write`, plus `successful_prefix` for I/O. Each direction has
`header_seen`, `paused`, `token`, `partial_frame`, `lazy_tail`. Each byte capture
has exactly `bytes`, `sha256`, `hex`, `capture_complete`; length and SHA-256
must match complete captured bytes. Partial negotiation fragments are bounded
by 266 bytes, lazy tails by 16 KiB + 10 bytes, and accumulated snapshot/RPC
capture by 4 MiB. Indexed frames independently reproduce the parser phase,
proposal/NA/ACK, selected token and bounded proposal count.

These diagnostics are not accepted RPC, scoring, delivery, selected-terminal
normalization or full selected PubSub disposal authority. Diagnostic native
errors remain fatal unless the independently verified empty-owner chain or
cleanup-only chain below exists. Sticky errors, failed/truncated captures, wrong/future ACKs and parser
failures remain fatal. Historical lazy bytes never excuse missing ACK or final
RPC residue on a selected stream.

A late-born, wholly empty diagnostic owner has a separate terminal gate, not
negotiation-aborted or selected PubSub authority. Its real native-call BEGIN ACK
must be nonzero and equal its registration/operation/RETURN ACK, with an indexed
live parent Prepare baseline and no invented stream baseline. Read/Write may
retain `outcome=error` and `successful_prefix_bytes=0` only for direct
`*network.ConnError` / `*qerr.ApplicationError` zero codes, matching boolean
Remote fields and `same_native_connection_context_cause=true`. The original
RETURN-sealed parent context must independently match the same authenticated
connection's indexed terminal context; neither a prior stream cause nor send
context can explain this I/O error. No diagnostic error is rewritten.
The entire physical owner must have no frames, proposals, selection, partial
bytes, lazy tail, successful prefix, RPC or framing-authority receipts. Each
error RETURN must precede an actual same-owner Prepared full Reset/Close BEGIN
and successful native RETURN with the same sealed parent. Native counters, not
publication order, establish that disposal; all receipts must precede the
actual zero-active-call native join and clean final host/worker join. Half,
early, missing or failed disposal cannot qualify. This gate exports no RPC,
score, delivery, selected protocol or framing receipt authority and does not
infer why the parent closed. Opaque/wrapped/errno/nonzero errors and prior
sticky failures remain fatal.

### Negotiation-Aborted Cleanup Only

Every `native_quic_stream` additionally has the immutable
`native_call_begin_prepare_ack_sequence` and
`native_call_begin_observation_basis=published_Prepare_ACK_before_native_CapableConn_call`.
The ACK is sampled before the native Open/Accept delegate, not at registration.
Ordinary pre-Prepare streams retain BEGIN=0. Cleanup requires a nonzero actual
BEGIN ACK equal to the stream registration `prepare_ack_sequence` and every
cleanup ACK. A call begun before Prepare cannot borrow a future ACK even if
registration or getters completed afterward. The referenced authenticated
connection must precede Prepare, have an indexed live parent baseline, and
the outbound stream must be born after that ACK with a live initial send
context and no stream-at-Prepare baseline.

The original successful `native_quic_negotiation_io_return` Write must capture
one fully consumed canonical prefix: outbound multistream header, one meshsub
proposal, and exactly one canonical length-delimited SUBSCRIBE-only RPC for
the actual fixture topic. Both actual frame receipts and all captured byte
lengths/hashes are checked. Duplicate/unknown protobuf fields, reordered or
noncanonical encoding, false subscription, enabled partial capabilities,
foreign topic, publish/control payload, second RPC, partial/trailing bytes or
unacknowledged other application bytes are not cleanup candidates. Optional
partial-capability fields are allowed only as canonical explicit false fields.
These candidate bytes are never emitted or consumed as selected RPC evidence.

The immutable candidate Write snapshot contains exactly the two outbound
frames and an empty read direction. Before the failed Read, at most one further
frame is allowed: the exact third canonical peer multistream HEADER, read on
the same authenticated native stream after that Write receipt. Its actual
indexed sequence is `peer_header_frame_sequence`, or exactly integer zero
when absent. The sealed Read, disposal and framing snapshots retain this
negotiation progress. No repeated/foreign header, reply, NA, ACK, selection,
read partial frame or read tail is accepted; the peer header never becomes
protocol or RPC authority and is never added to the successful Write prefix.

The sealed Read RETURN must have n=0 and preserve the original error, outcome
`error`, direct outer `*network.ConnError` code zero and direct inner
`*qerr.ApplicationError` code zero with matching Remote. Its sealed parent
`connection_context` must exactly match the independently indexed current
same-authenticated-connection terminal context and live Prepare baseline.
The new context observation basis samples that parent at native RETURN; the
send context is sampled after the seal. No earlier stream's cause or later
context snapshot can explain this Read. Write RETURN must precede Read RETURN
in the same native stream's counters. The terminal reason remains unknown;
this is not proof of a graceful peer shutdown, protocol ACK or delivery.

`native_quic_negotiation_cleanup_pending` and
`native_quic_negotiation_cleanup_finalized` use only source
`go.quic.native_stream.negotiation_cleanup` and common native owner fields.
Both have exactly these additional fields: `operation_receipt_sequence`,
`candidate_write_receipt_sequence`, `connection_context_receipt_sequence`,
`prepare_ack_sequence`, `candidate_protocol`,
`candidate_protocol_frame_sequence`, `peer_header_frame_sequence`, `candidate_bytes_complete=true`,
`selected_rpc_authority=false`, `negotiation_complete=false`,
`framing_clean=false`, `terminal_outcome`,
`same_native_connection_context_cause=true`.
Pending outcome is `negotiation_aborted_cleanup_pending`. Only this exact,
independently verified pending chain defers the recorded first qualifying
Read failure; it cannot clear any prior sticky/parser/I/O/disposal failure.

Finalized outcome is `negotiation_aborted_cleanup`. The final receipt
additionally has exactly `pending_receipt_sequence`,
`framing_receipt_sequence`, `owner_disposal_receipt_sequence`,
`first_owner_disposal_receipt_sequence`, `native_join_receipt_sequence`,
`accepted=true`. The successful full native
Close/Reset must begin after the sealed Read RETURN, return nil, and be
indexed before framing, join and finalization. Its receipt may precede the
Read receipt and pending because native RETURN sealing precedes publication.
Receipt sequence does not establish native call causality. Half-close, ResetWithError,
pending/error disposal or invented booleans are insufficient. The separate
disposal `connection_context` must exactly match the parent context sealed at
the original Read RETURN; Close also retains its independently indexed native
context receipt link. A substituted nonzero, opaque or foreign parent cause
is fatal even when disposal returned nil. The separate
`native_quic_framing_finalized` receipt retains the normal owner/join/residue
fields, but explicitly has `framing_clean=false`, `negotiation_complete=false`,
the unchanged `negotiation_snapshot`, `candidate_bytes_complete=true`,
`selected_rpc_authority=false`, `peer_header_frame_sequence`,
`cleanup_owner_disposal_receipt_sequence`, and
`negotiation_cleanup_pending_receipt_sequence`. Its snapshot matches the
sealed failed Read, not a retrofitted Write snapshot. Both framing decoders have
zero residue and actual joined directions; the captured negotiation tail is
retained as diagnostic candidate bytes, not reclassified as a decoded RPC.

Framing `owner_disposal_receipt_sequence` retains the original first successful
full native disposal, also referenced by final
`first_owner_disposal_receipt_sequence`. It can precede the failed Read but
cannot explain its cause or satisfy post-Read cleanup. Framing
`cleanup_owner_disposal_receipt_sequence` and final
`owner_disposal_receipt_sequence` instead identify the first actual qualifying
successful full Close/Reset, ordered by immutable native RETURN counter,
whose BEGIN counter strictly follows that Read RETURN. Both references independently resolve to the
same real native stream; neither pin can be rewritten to an earlier, foreign
or later operation. A first disposal does not remove the requirement for the
separate post-Read pin. The pin is independent of cleanup publication and
cannot be replaced by a later native operation that publishes first. Exact
owner/Prepare ACK, strictly increasing BEGIN/RETURN counters and unchanged
sealed parent contexts remain mandatory. No causal reset claim is made about
the original error.

Exactly one finalizer per pending owner must follow that owner's framing and
the actual zero-active-calls native join. Join counts every real physical
owner. Only cleanup finalizers, not I/O/operations/context/framing, may follow
the native join receipt. Missing, duplicate, orphan, foreign, unknown-field,
nonzero, wrapped, partial or unfinished chains remain fatal. Active snapshots
may retain raw diagnostic/pending records, never claim completed cleanup or
hide the original error. Selected-stream protocol, RPC, scoring, delivery and
terminal acceptance requirements are unchanged; host/process join and exit
zero remain independently mandatory.

The case owner pass distinguishes cleanup framing only by the exact indexed
receipt identity exported after complete independent chain validation, not by
kind, source prefix or cleanup flags. Such framing retains `protocol=''` and
`selected_rpc_authority=false`; it never enters the selected RPC owner path.
Unvalidated or incomplete chains receive no cleanup dispatch authority.

## Router Configuration

Use topic `forge-pr11:<case-token>`, strict signed messages and real validators.
Only the victim rejects `reject:<case-token>:*` and ignores
`ignore:<case-token>:*`; other actors accept valid messages. Never inject score
through application-score APIs to prove invalid-message scoring.

The test mesh uses `D=2`, `Dlo=1`, `Dhi=4`, `Dscore=1`, `Dout=0`, 250 ms
heartbeat, one-second prune backoff, no flood publishing and no PX.
Scoring is enabled explicitly:
P4 weight -100, decay 0.99 per second; other factors have zero weight.
Thresholds are gossip -10, publish -50, graylist -80, PX 10, opportunistic 20.
Loopback IP penalties are disabled. Retain score for 60 seconds. Independent
native regressions cover the other factors, flood, promises and retention.

First connect victim-offender and replacement-sink and wait for both native
one-member meshes. Then connect victim-replacement. Both already meet Dlo, so
this additional connected/subscribed candidate remains outside their mesh.
Prove that fact before rejection. No other edges exist. Native reject -> PRUNE
leaves the victim below Dlo; its heartbeat must GRAFT the replacement. Publish a
distinct accepted message at the victim and prove victim -> replacement -> sink
without a victim-sink connection or flood shortcut. This graph avoids forcing
donor selection or relying on peer-ID ordering. Wait for native state, not sleeps.

## Evidence

Top level contains `schema_version=1`, `implementation`, `actor`, `case_token`,
`local_peer_id`, `finalized`, `joined`, `overflow`, `error` and `events`.
Each event has contiguous `sequence`, monotonic `mono_ns`, `kind` and a native
`source`; command completion also has `command_sequence`.

Capture authenticated connections with actual transport/security/muxer and
native connection ID, not configured labels. Stream evidence includes selected
protocol, remote peer and actual connection/stream owner. A default configured
version or advertised protocol list is not negotiation evidence.

Capture validation with propagation peer, author, topic, payload SHA-256,
message ID and result. Capture native score observations and mesh snapshots,
GRAFT/PRUNE send and receive, deliveries and publish outcomes. Snapshot events
include `label`, mesh members and peer scores. Native wire receipts must expose
the actual PRUNE fields; v1.0 must omit PX and backoff, not merely encode zero.

Go uses passive RawTracer/score inspection. Rust uses public Behaviour events,
score/mesh reports and passive upgraded-stream observation. Forge uses copied
node score/mesh state and real admitted stream observations. Asynchronous
inspection timestamps are observation times, not mutation timestamps. In Go,
mesh membership reconstructed from native GRAFT/PRUNE is a derived ledger, not
a direct private-state snapshot. Every Rust validation commit reports successful
`report_message_validation_result`; false is not a committed result.

`joined` covers fixture-owned subscribers/control workers/observer callbacks and
native host/session close. It does not claim all private donor goroutines were
individually joined. The runner independently records actual process exit.
Read only complete JSONL lines. Limit each event to 64 KiB, the entire trace to
16 MiB, connected peers to 16 and observed streams to 64. Errors/overflow are
sticky. Duplicate proof uses identical signed author/seqno bytes, not repeat
publish with a new seqno; it is a focused native regression, not this live claim.

Canonical event keys used by the shared checker:

- `connection`: `peer_id`, `connection_id`, `authenticated`, `transport`,
  `security`, `muxer`, `remote_address` and native `authentication_basis`.
- `protocol`: `peer_id`, `connection_id`, `stream_id`, `protocol`.
- `rpc`: those stream-owner fields, `direction=read|write`, and `receipt`:
  actual `framed_hex`, plus a direction record with `framed_bytes`,
  `framed_sha256`, `frames=1`, `complete_frames=true`,
  `invalid_or_over_limit=false`. The receiver receipt is authoritative; a
  sender's RawTracer submission alone is insufficient actual I/O proof.
- `validation`: `propagation_peer`, `author_peer`, `topic`, `message_id`,
  `seqno_hex`, `payload_sha256`, `outcome=accept|reject|ignore`.
- `delivery`: `propagation_peer`, `author_peer`, `topic`, `message_id`,
  `seqno_hex`, `payload_sha256`.
- `snapshot`: `label`, `mesh_peer_ids` and `peer_scores`, whose entries carry
  `peer_id`, finite `value` and the topic's `invalid_deliveries` counter.

All keys come from observed native state. Missing a required native hook is an
implementation/evidence gap to report, not permission to synthesize the event.

The runner proves offender membership before rejection, negative P4 after a
real validation, PRUNE/removal, GRAFT of the previous non-mesh replacement and
delivery over that repaired path. Ignore is checked before reject/graylisting;
it cannot change malicious penalties or reach the sink. A valid duplicate must
not deliver twice; an invalid duplicate is allowed to retain donor penalties.
Missing receipts, timeout, wrong owner or unjoined process are harness failures,
never successful negative controls. Every attempt binds immutable source,
donor/fixture/binary hashes, PID and command via the canonical runner index.
