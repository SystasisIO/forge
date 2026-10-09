# Rust QUIC Shutdown Diagnosis

## Status And Scope

Follow-up to Stage 6 PR11 on Forge `7491447cfe6fd19476afcbe5339b55663dcdacbd`.
This note records a diagnostic investigation and the maintainer-approved local
test recipe, not a new acceptance result.
Original Rust QUIC shutdown remains `NOT_PROVEN`. Neither historical raw
errors nor the separate instrumented-companion receipts are reclassified.
No Forge runtime, wire protocol, canonical donor pin or acceptance gate changes.

## Inspected Sources

Rust libp2p pin: `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`.

- `transports/quic/src/lib.rs`: the private `ConnectionError` wrapper.
- `transports/quic/src/connection.rs`: accept/open errors and local close.
- `transports/quic/src/connection/stream.rs`: public native stream read errors.
- `core/src/muxing/boxed.rs`: the original error retained inside `io::Error`.
- `swarm/src/connection/pool/task.rs`: error termination versus muxer close.
- `transports/quic/tests/smoke.rs` and `stream_compliance.rs`: native connection
  setup and stream exchange patterns reused by the diagnostic test.
- Quinn `0.11.11` connection termination: later local close may change the
  connection's observable cause. One operation's error is not authority for
  another operation's error, even on the same authenticated connection.

## Confirmed Cause

The PR11 failures occurred on Rust-to-Rust physical connections. Each peer had
acknowledged Prepare, but one received remote closure before its own close.
The native muxer returned `Error::Connection`; Swarm terminated that connection
through its error path. Streams and muxers were dropped, but successful native
`poll_close` was not observed for the affected owner.

The private wrapper derives a transparent error implementation. Its `source()`
delegates to the inner Quinn error's source, rather than returning the Quinn
error itself. For `ApplicationClosed`, that source is absent. Boxing and the
public `SwarmEvent::ConnectionClosed` do not recover the missing typed cause.
The fixture deliberately rejects an unclassifiable native error.

An owned stream can independently expose `ReadError::ConnectionLost(cause)`.
That observation must not classify an earlier or later opaque muxer error.
The raw regression demonstrates why: after observing remote closure, a local
close can succeed and a subsequent stream read can report `LocallyClosed`.
Neither observation erases the original muxer error.

## Reproduction

`tests/libp2p_interop/rust_fixture/quic_shutdown_tests.rs` uses public native
QUIC transports, authenticates both identities and exchanges
bytes in both directions. Three bounded cases cover each side closing first
and both close futures completing before terminal observations. The third is
not proof that the close operations overlap concurrently.

```sh
cargo test --locked --offline -j4 original_quic_ -- --nocapture --test-threads=1
```

Run this from an exported `rust_fixture` with its pinned `fixture-deps`, as for
the ordinary interop build. Run the same tests in the isolated copy with
`--features quic-cause-observer`: original muxer errors must remain opaque,
whereas the copy must expose its own exact typed source. Stream error causes
are never used to classify a muxer error. Tests join their own scoped futures and dispose of
streams, connections and transports. The public transport does not expose an
endpoint-idle/internal-task join; the diagnostic explicitly does not claim it.

The separate two-host tool reuses already built, hash-recorded fixtures:

```sh
python3 tests/libp2p_interop/reproduce_quic_shutdown.py \
  --forge /absolute/path/to/forge_interop_fixture \
  --rust /absolute/path/to/forge-libp2p-rust-fixture \
  --output /absolute/path/to/new-diagnostic-directory
```

It performs real two-way PubSub delivery and Prepare before stop. It covers
Rust/Rust and both Forge/Rust dial directions with three stop schedules.
`both-published` means publishing both process stop requests without waiting
between them, not simultaneous native close. Errors and process exit codes
remain in per-case `diagnostic.json`; any failed actor makes the tool exit 1.
These files are diagnostics, not canonical acceptance receipts.

The initial nine-case run had successful delivery/Prepare in every case and no
forced termination. Six cases retained opaque Rust closure errors. Rust local
close completed normally when it won the ordering. This isolates a diagnostic
API limitation, not evidence of failed traffic or a Forge shutdown hang. It
does not establish that every unknown native error is benign.

## Local Patch And Deferred Proposal

`tests/libp2p_interop/rust_quic_source_proposal.patch` is an explicit proposal,
not a submitted upstream patch. It makes `ConnectionError::source()`
return the actual Quinn error. `Display` delegates the original formatter,
preserving width, precision and alternate formatting. There are no transport,
polling, close-result, authentication or wire changes and no new accessor API.

The included unit test checks local close, remote application close with zero
and nonzero codes, reset, timeout and transport protocol errors. It covers the
wrapper, outer enum and `io::Error` boxing, typed fields and formatting. The
isolated proposal copy passed all five QUIC library unit tests using its own
locked donor dependencies. That is not live-interoperability acceptance.

Changing the standard error chain is an observable diagnostic API change.
Error reporters may display the same message at two levels. Downcasting also
requires the consumer to use the same Quinn type/version. The actual pinned
donor and the original exported tree remain unmodified.

The maintainer approved local testing and deferred upstream submission.
`rust_quic_observer.py` installs only the implementation hunk, not the proposal's
unit-test module, in a separate fresh copy. Its receipt records the exact
applied diff/hash, original and copied source graphs and the sole changed file.
An accessor-only receipt cannot validate this recipe. The fixture uses the
standard source chain in both builds; the copy feature marks provenance and
selects strict native-boundary test expectations, not error classification.

## Exit Gate

1. Keep the canonical donor untouched. Build the separate source-patched copy
   with exact source/binary/command provenance. Do not send the patch upstream.
2. Test both variants, retaining failure for unknown causes and strict separate
   expectations for opaque original versus exposed copied native muxer errors.
3. Check nonzero closes, resets, timeouts, missing owners and delayed failures;
   an accepted zero-code close still requires Prepare, exact owner and actual
   disposal/join evidence. Do not hide a native `Err` or call it `poll_close=Ok`.
4. Run the canonical 24 original traffic cases plus four independent copied
   shutdown cases on the final clean Forge head. Preserve original errors and
   `NOT_PROVEN`; only the expressly scoped combined result can pass.
5. After clean reviews and scoped acceptance, continue PR12. Future upstream
   submission/pinning is separate work. Only a successful unmodified-donor
   shutdown gate can remove the original `NOT_PROVEN` classification.

Stage 8 remains responsible for long-duration and hostile-network proof.
This follow-up does not implement PR12 GossipSub extensions or claim global
libp2p production readiness.

## First Source-Patch Canonical Run

The clean `12a7c41cf8eaa39b9e534d58f237c32ca77f4ec2` run passed 199 Rust
tests in each build and all four independent source-patched QUIC scenarios.
The full promotion failed in `gossipsub_v1_1.go_to_forge`: the validator routed
a zero-byte outbound stream into its inbound remote-reset diagnostic branch.
The failed artifact is retained with SHA-256
`6ea05ffe52a409c1daa654615657ef124e9db7524f32f2546e01087e37e3b590`;
it is not converted into a successful receipt.

The Go replacement actor recorded Prepare 187, outbound stream 194, successful
full Reset 196, zero-byte local StreamError0 Read 211, Reset 218, zero-byte
local StreamError0 Write 219 and full Reset 220 before native join 239.
Native call counters, not publication order, place successful Reset RETURN
before each I/O BEGIN. The stream never selected a protocol or transmitted
negotiation/application bytes. This calls for a separate exact empty-outbound
disposal check, not a change to Rust or Go transport behavior or an exception
for unknown errors.

The subsequent `ce71a68f` run was interrupted during fixture compilation,
before live actors started, when review found that active snapshots could
publish Read before the already-sealed Reset receipt. Incomplete active
native-counter ranges must remain pending; terminal evidence still requires
every actual Reset and return. No acceptance result is claimed for that run.
