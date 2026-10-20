# Private native worker

This is the private, native Rust backend of `forge_net_tlsnotary`. It composes
the pinned upstream MPC prover/verifier; it is not an HTTP client, a network
listener, an application service, or a public executable API. The owning C++
session launches exactly one worker, supplies connected Forge streams, and
reaps the child on success, failure, cancellation, and timeout.

Build with Rust 1.95 and `cargo build --release --locked`. `Cargo.lock` pins the
complete dependency graph. The build must reject `cfg(tlsn_insecure)`; there is
no ideal, proxy, or plaintext fallback. Upstream dependency provenance and
licenses are recorded in [THIRD_PARTY.md](THIRD_PARTY.md).

## Ownership and runtime

The worker accepts no arguments or configuration paths. File descriptor 0 is
a private connected Unix stream supplied by the host. It is the duplex IPC
channel; an ordinary stdin pipe is not supported. No DNS lookup, TCP socket,
HTTP codec, P2P implementation, secret store, or child process is created here.
The host owns peer authentication, transport security, network policies, and
stream lifetime. The prover's server stream must be raw transport bytes, since
TLS is executed by upstream MPC.

The worker owns a Tokio current-thread runtime and preinstalls both upstream MPZ
and Rayon global pools with the explicitly bounded `mpc_threads` count. The
per-process execution budget is `2 * mpc_threads + 1` threads: one MPZ pool,
one Rayon pool, and the current-thread Tokio runtime. A pool already initialized
or failing initialization fails the worker; there is no CPU-count fallback.
The pinned upstream
`Session` otherwise initializes a process-global pool from CPU count and does
not expose an owned-pool teardown hook. Process isolation makes cancellation
and the final wait/reap a reliable resource boundary without modifying upstream
cryptography. No detached host worker or generic process manager is required.

The worker enforces the configured total peer wire-byte budget, byte and record
bounds for the TLS transcript, bounds on all IPC allocations, and a deadline.
The received-record bound is upstream's **online** record bound; deferred
decryption does not claim a bound on the total number of received records.
Encrypted server bytes in both directions together have the additional bound
`524288 + 18432 * (max_sent_records + max_received_records_online)`; this
includes handshake allowance and bounds the private server bridge, independently
of the application transcript byte limits.
The host separately bounds active workers. These are **not a hard RSS limit**:
the pinned upstream multiplexer's receive window remains 1 GiB and MPC can
allocate substantial intermediate state. Process isolation contains crashes;
measured resident memory must not be described as a memory quota.

## Private IPC version 1

Every integer is unsigned big-endian. A frame is `u8 kind, u32 length, bytes`.
The configuration frame (kind 0, first host frame only) and terminal frame
(kind 128, worker only) allow at most 2 MiB. All other frames allow 65536 bytes;
actual stream chunks allow at most 16384 bytes. Length checks happen before
allocation. Unknown kinds, extra configuration bytes, unexpected directions,
duplicate EOF/acknowledgements, or data without read credit fail closed.

Configuration fields, in order:

```
u32 magic = 0x46544e31
u8 role (1 prover, 2 verifier), u8 flags (bit 0 deferred), u16 reserved = 0
u64 deadline_ms (1..600000)
u32 max_sent, u32 max_received (1..65536 each)
u32 max_sent_records, u32 max_received_records_online (1..256 each)
u32 mpc_threads (1..8), u64 max_peer_bytes (1..1073741824, both directions combined)
blob server_name (DNS name, at most 253 UTF-8 bytes)
u32 roots_count (1..64), blob DER certificate per root
blob request_bytes (prover only; bounded by max_sent)
range_vector sent, range_vector received
```

A blob is `u32 length, bytes`. Each root is at most 65536 bytes and all root
bytes together are at most 1 MiB. A range vector is `u32 count` followed by
`u32 begin, u32 end` pairs, at most 1024 pairs, ordered and nonoverlapping.
`end=0xffffffff` means the final actual transcript length. All other bounds are
checked against both configured limits and the final transcript. The verifier
requires exactly these authenticated disclosure masks and the exact configured
authenticated server name. Products own interpretation of revealed contents.
Empty masks disclose no plaintext: the verifier still authenticates the server
identity and reports the actual transcript lengths through an explicit empty
partial transcript. An absent upstream transcript proof is never accepted.

| Kind | Direction | Meaning |
| --- | --- | --- |
| 1 / 2 | both | peer / server bytes |
| 3 / 4 | both | peer / server EOF, no payload |
| 5 | host to worker | cancel, no payload |
| 6 / 7 | host to worker | peer / server write acknowledgement, no payload |
| 17 / 18 | worker to host | peer / server read request, `u32 maximum_bytes` |
| 128 | worker to host | one terminal result |

The host sends exactly one data or EOF frame for each read request. The worker
has at most one read request outstanding per channel. The host acknowledges
worker data only after its Forge stream write completes; it acknowledges EOF
only after write-side closure completes. The worker has at most one output
frame outstanding per channel. These credits and acknowledgements prevent
unbounded queues and cross-channel head-of-line deadlocks. Both directions
remain independently driven while awaiting acknowledgements.

The terminal payload starts with `u32 status`. Failure contains this field only:
1 configuration, 2 I/O, 3 rejected, 4 upstream internal, 5 cancelled,
6 timeout, 7 limit, 8 verification, 9 IPC protocol. Mapping uses upstream typed
category methods and call-site phase, never error text. No upstream error string
or request bytes are sent to stderr, diagnostic fields, or failure results.

On success (`status=0`) the remaining fields are:

```
u8 authenticated (verifier=1, prover=0)
blob server_name
u32 total_sent, u32 total_received
sparse_data sent, sparse_data received
```

Sparse data is `u32 count` followed by `u32 begin, blob authenticated_bytes`.
Prover success returns zero sparse entries. Verifier success contains only
cryptographically authenticated revealed ranges; unrevealed bytes are never
returned. It is available only after identity, trust roots, source name,
disclosure masks, cryptographic verification, and clean session close pass.

The bootstrap has a 10-second deadline. Session EOF, malformed IPC, explicit
cancel, peer failure, or protocol deadline terminates the operation. The worker
attempts a bounded terminal write and exits; host shutdown must close/cancel
streams and always reap. The host may terminate a stuck worker without relying
on cooperative Rust task cancellation. A valid prover completion is not an
independent verifier acceptance.
