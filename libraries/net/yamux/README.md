# forge_net_yamux

`forge_net_yamux` implements Yamux multiplexed sessions over a single
`forge_net_transport::stream`. It gives higher layers a `transport::session` with
independent logical streams while keeping flow control, reset, close and
resource accounting inside the Yamux owner.

## When To Use

- A secure TCP or libp2p-compatible path needs multiplexed streams.
- P2P direct TCP profiles need a Yamux session over an already authenticated
  stream.
- Tests need a transport-neutral session handle backed by Yamux.

## When Not To Use

- Do not use Yamux as a security layer. TLS/Noise/STCP owns authentication.
- Do not put peer identity, protocol negotiation, DHT, relay or application
  retries here.
- Do not bypass `forge_net_transport` stream/session handles with Yamux-specific
  application APIs.

## Public Modules

- `forge.net.yamux.options`
- `forge.net.yamux.session`
- `forge.net.yamux.exceptions`
- `forge.net.yamux`

Target: `forge_net_yamux`.

Dependencies: `forge_exceptions`, `forge_net_transport`, Boost.Asio.

## Examples

```cpp
import forge.net.yamux.exceptions;
import forge.net.yamux.options;
import forge.net.yamux.session;

boost::asio::awaitable<void> run_client_mux(forge::net::transport::stream base) {
   auto mux = forge::net::yamux::session{
      std::move(base),
      forge::net::yamux::side::initiator,
      forge::net::yamux::options{.max_streams = 1024},
   };

   auto stream = co_await mux.async_open_stream();
   co_await stream.async_write_frame(std::span<const std::uint8_t>{payload});
   co_await mux.async_close();
}
```

```cpp
boost::asio::awaitable<void> serve_mux(forge::net::transport::stream base) {
   auto session = forge::net::yamux::make_session(
      std::move(base),
      forge::net::yamux::side::responder);

   auto stream = co_await session.async_accept_stream();
   auto frame = co_await stream.async_read_frame_chunk();
   co_await stream.async_write_frame(std::move(frame));
}
```

## Boundaries

- `cancel()` is abortive and propagates reset semantics where possible.
- `async_close()` is graceful session shutdown. It waits up to
  `options::close_timeout` for the peer's half-close, then cancels the lower
  transport so shutdown cannot wait forever for a peer that never sends FIN.
- Reset streams are not valid for further read/write operations.
- Once buffered bytes are drained, normal remote FIN raises
  `exceptions::end_of_stream`, a subtype of `exceptions::closed` with the same
  numeric code. Reset, cancellation and failed sessions do not grant this
  normal-completion signal, including after an earlier FIN.
- Lower transport failures are translated to typed Yamux boundary errors.
- Every physical Yamux frame write is bounded by `options::write_timeout`.
  A stalled lower transport therefore fails the whole session closed instead
  of leaving stream open/reset or session shutdown blocked indefinitely.
- Canceling one stream does not interrupt a frame already accepted by the
  lower transport. That frame completes first, then a stream-local `RST` is
  serialized through the same write gate; sibling streams remain usable.
- Yamux's wire-level initial stream credit is fixed at 256 KiB. A larger
  `options::initial_window` is advertised as a SYN/ACK delta; values below the
  baseline are rejected as invalid options. Non-positive `write_timeout` and
  `close_timeout` values are also rejected before session startup.

## Tests

`test_forge_yamux` covers stream open/accept, flow control, resource limits,
reset validity, cancel propagation, lower transport write failures, clean close
and integration through `forge_net_transport::session`.
