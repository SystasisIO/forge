# Swarm Runtime: Transfer Ownership And Product Allowances

The [PR6 roadmap](../iterations/forge-content-swarm-v1.md#pr6-library-runtime-and-policy-enforcement)
defines a reusable library engine, not another P2P host or payment system.
Implementation and acceptance evidence are recorded separately; source inspection
alone does not establish runtime readiness.

## Inspected Baselines

- libtorrent `b4e9e2471e7abce2a58585a7d7fbc4e8d1edc47e`:
  `src/peer_connection.cpp` (request cancellation, late disk work),
  `src/disk_buffer_pool.cpp` (buffer pressure), `src/session_impl.cpp`
  (staged shutdown) and `src/torrent.cpp` (hash/write completion ordering).
- Syncthing `1919c89de488d8e1088579f5b11f28aa375c5a8e`:
  `lib/protocol/protocol.go` (response ownership and cancellation),
  `lib/model/folder_sendrecv.go` (bounded pull pipeline),
  `lib/model/model.go` (global/local request limits),
  `lib/model/sharedpullerstate.go` (write/close ownership), and
  `lib/semaphore/semaphore.go` (weighted admission).
- Existing Forge Core picker/choker, file-store writer/lease/checkpoint,
  API live streams, Asio scheduler, compute executor and notifications.

## Accepted And Rejected Patterns

Reserve capacity before starting a block and retain it through verification and
storage. Use both operation and engine limits. Network credit released by an
application read is not a bound on its later disk/CPU pipeline.

Cancellation retires request ownership; a late response or disk completion
cannot revive the request or mark the wrong generation verified. Match peer,
request ID, piece, offset and length before writing. One piece writer has one
active operation, even when multiple peer sessions progress concurrently.

Stop admission first, cancel owned work, join actual users, checkpoint, and
then release storage holds. Canceling a task or completing a typed API call
does not by itself prove its handler has left. Never stop shared execution or
storage to cancel one transfer.

Hash validation cannot override a failed write or sync. Store-verified bytes,
durable checkpoints and atomic publication are different transitions.

Donor buffer pressure is not a product allowance. Libtorrent permits temporary
buffer overshoot; Syncthing clamps oversized semaphore weights. Neither pattern
is accepted as strict byte-quota enforcement. Forge requires an explicit
product grant before output, including metadata and proof paths. Queue
acceptance is not confirmed receipt, and unknown delivery is not an automatic
refund. Payment units, durable accounting and settlement remain product-owned.

The donors' thread pools, polling and protocol implementations are not copied.
Forge reuses caller-owned Asio execution and its existing live API streams.
There is no BitTorrent/IPFS wire compatibility claim.

## Compute Completion Lifetime

Storage fault acceptance exposed an executor-lifetime race below Swarm. A
separate regression reproduces it using only `forge.asio.compute`, fresh
`io_context` instances and fast successful/failing callbacks: pool work can
finish while its caller never receives completion. No filesystem interposition
or crash child is needed for that reproduction.

The inspected Boost 1.90 paths are `asio/detail/impl/scheduler.ipp`
(`post_immediate_completion`, `wake_one_thread_and_unlock`) and
`asio/detail/posix_event.hpp` (`maybe_unlock_and_signal_one`). A queued callback
can execute before the posting worker returns from `post()`. The compute
completion therefore retains its caller's tracked executor on the posting
stack until `post()` returns; transferring the only work guard to that callback
would permit the caller to drain and destroy its context too early. The queued
operation already accounts for its own outstanding work.

The regression requires both the exact result/exception and full executor
quiescence. Its bounded failure diagnostics and the storage helper's watchdog
abort an actual stall; they never stop an executor to manufacture completion.
Native failure, trace and post-fix stress artifacts are retained separately.

## Evidence Boundary

Focused runtime tests, real-file/API integration, installed-consumer validation
and independent exact-head reviews are required before PR6 acceptance. The
optional plugin is PR7. Multi-product deployment, hostile/long-duration trials
and the integrated Swarm acceptance block remain separate gates.
