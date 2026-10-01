# forge_api_stream

`forge_api_stream` is the focused Forge API wire-v2 runtime over an already
established `forge::net::transport::stream`. It sits above `forge_api_core` and
`forge_net_transport` and owns the symmetric session handshake, multiplexed
call lifecycle, incremental stream frames and receiver-driven flow control.

## Public Modules

- `forge.api.stream.options`
- `forge.api.stream.session`
- `forge.api.stream.server`

## Wire V2

Both peers exchange `session_hello` before application traffic. The hello
negotiates the wire major, codec, method capabilities and limits. Call id zero
is reserved for session control. Each call then has independent input and
output directions; `stream_end` half-closes one direction, while one terminal
`response`, `error` or `cancel` completes the call.

`stream_window` carries monotonic absolute item and byte limits. Duplicate or
smaller limits do not add credit. A received item consumes one item and its
encoded payload bytes; capacity is advertised again only after application
code consumes the item. Control frames bypass flow-controlled data, while data
frames remain FIFO within a call and calls share the writer round-robin. Control
priority is bounded, so a sustained window-update stream cannot starve data.

Default limits are 16 items and 1 MiB of byte credit per call, 1 MiB per item,
16 MiB aggregate buffered bytes, 60 seconds idle timeout, no total call
deadline and 5 seconds disconnect grace. Protocol violations, deadlines and
cancellation wake blocked reads, writes and credit waits without terminating
unrelated calls.

Completing hello negotiation does not revive a failed or cancelled call. The
session checks terminal state before accepting handshake completion and again
before queuing an outgoing request, preserving the original typed error without
creating an unreachable write receipt. Terminal server responses remain queued
after their call is marked complete; call completion alone is not a blanket ban
on response writes.

The advertised inflight limit is conservatively capped by aggregate bytes
divided by the initial byte window. Every admitted inbound stream therefore
receives one complete initial window; an idle stream cannot reserve all credit
while another admitted stream remains at zero. The initial byte window also
caps the negotiated maximum item size.

`session` owns all detached reader/writer/handler coroutines through shared
session and call state. Completed tombstones are bounded. An ingress-drain
tombstone remains live until the peer half-closes or cancels that direction and
continues to consume an inflight slot, so credited crossing items cannot turn
into an unknown-call session failure. Destroying an unfinished typed call
cancels only that call; callers still use `async_finish()` to observe the
terminal result.

Server-side `session` and `serve_stream` preserve their trusted-metadata
arguments and accept an optional typed trusted invocation after that metadata.
This keeps existing calls, including calls that pass `{}` for metadata,
unambiguous while carrying immutable typed values separately into contextual
request dispatch.

## Boundaries

- Use this library when a protocol adapter already has a `transport::stream` and
  needs to serve or consume a typed API binding plan.
- Use `forge_api_transport` when you need the generic transport client,
  connection or session helpers.
- Socket, QUIC, P2P and application lifecycle stay in their owning libraries.
- WebSocket maps one binary message to one length-delimited transport frame and
  then uses this same runtime. HTTP/1.1 streaming uses the same API frame codec
  over request/response bodies, but native body backpressure replaces
  `stream_window` frames.
