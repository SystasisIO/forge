# forge_net_transport

`forge_net_transport` is the reusable byte stream/session substrate used by TCP, STCP,
Yamux, QUIC, P2P and API-over-transport bindings. It owns transport-neutral
concept wrappers, endpoint values, frame helpers and pooled byte chunks. It does
not own sockets, peer identity, HTTP, WebSocket, P2P routing or application API
semantics.

## When To Use

- A concrete transport needs to expose a common `stream` or `session`.
- A higher layer needs length-prefixed frame helpers over an existing byte
  stream.
- Hot paths need reusable `chunk` storage without forcing vector roundtrips.
- Tests need fake connectors/listeners/sessions with the same public contract as
  real transports.
- A caller-owned UDP socket needs per-datagram destination/interface metadata
  and explicit reply source selection on macOS or Linux.

## When Not To Use

- Do not add peer IDs, relay policy, protocol negotiation or discovery here.
- Do not put API contracts or RPC method names in this layer. Use
  `forge_api_transport` above it.
- Do not use `buffer_pool` as an unbounded queue or application cache.

## Public Modules

- `forge.net.transport.buffer`
- `forge.net.transport.endpoint`
- `forge.net.transport.datagram_io`
- `forge.net.transport.frame`
- `forge.net.transport.stream`
- `forge.net.transport.session`
- `forge.net.transport.connector`
- `forge.net.transport.listener`
- `forge.net.transport.registry`
- `forge.net.transport.limits`
- `forge.net.transport.exceptions`
- `forge.net.transport`

Target: `forge_net_transport`.

Dependencies: `forge_exceptions`, Boost.Asio.

## Examples

```cpp
import forge.net.transport.buffer;
import forge.net.transport.frame;

auto pool = forge::net::transport::buffer_pool{
   forge::net::transport::buffer_pool_options{
      .default_capacity = 64 * 1024,
      .max_cached_buffers = 32,
      .max_cached_bytes = 8 * 1024 * 1024,
   }};

auto builder = pool.acquire(4096);
auto writable = builder.writable();
std::copy(payload.begin(), payload.end(), writable.begin());
auto chunk = builder.commit(payload.size());

std::vector<std::uint8_t> encoded;
forge::net::transport::encode_frame_to(encoded, chunk.bytes());
auto view = forge::net::transport::decode_frame_view(encoded);
```

```cpp
import forge.net.transport.stream;

boost::asio::awaitable<void> echo_frame(forge::net::transport::stream stream) {
   auto frame = co_await stream.async_read_frame_chunk();
   co_await stream.async_write_frame(std::move(frame));
}
```

## Boundaries

- `stream` and `session` are move-only handles over private concepts.
- Vector APIs remain convenience wrappers; chunk APIs are the fast path.
- Frame decoding supports consumed-offset parsing. Hot paths must not rely on
  repeated front erases of buffered bytes.
- Thread-safety contracts are documented in `docs/runtime/thread-safety.md`.

## Datagram Packet I/O

`datagram_io` borrows a Boost.Asio UDP socket and buffers; it owns no socket,
thread, queue, resolver or protocol policy. Configure the open socket before I/O.
Keep the socket and buffer storage alive through completion, and run all its
operations and close on one owning executor/strand. Allow one receive and one
send concurrently, but serialize sends and never mix receive implementations.

```cpp
import forge.net.transport.datagram_io;

namespace io = forge::net::transport::datagram_io;

boost::asio::awaitable<void> reply(boost::asio::ip::udp::socket& socket,
                                  boost::asio::mutable_buffer buffer) {
   const auto incoming = co_await io::async_receive(socket, buffer);
   // Unicast reply only. For multicast input, choose a local unicast source
   // separately; incoming.local is the multicast destination, not that source.
   const auto sent = co_await io::async_send(
       socket, boost::asio::const_buffer{buffer.data(), incoming.size}, incoming.remote,
       io::source{.address = incoming.local.address()});
   (void)sent;
}
// Before starting the coroutine: io::configure(socket);
```

Omitting the optional source (`std::nullopt`) deliberately lets the kernel
select the route/source, for example for an initial outbound datagram. It does
not connect/disconnect the socket or report the selected source. Replies needing
the original local destination must supply it explicitly. Explicit unspecified
and multicast sources are rejected. IPv6 link-local scope and interface index
must agree; numeric scope is preserved, and interface names are not resolved here.

IPv4 uses `IP_PKTINFO`; IPv6 uses `IPV6_RECVPKTINFO`/`IPV6_PKTINFO`.
Darwin ignores `ipi_spec_dst` when ancillary `ipi_ifindex` is nonzero. Explicit
IPv4 source+interface is therefore accepted only when the socket is bound to that
exact source, or its existing persistent `IP_BOUND_IF` matches the requested
index. In the former case, native source selection retains the bound address;
in the latter, index-zero packet info selects the source and the existing sticky
binding supplies the interface. Otherwise `operation_not_supported` is thrown
before sendmsg. No send changes socket options or silently ignores an index.
Owners requiring interface-specific routing must establish persistent binding
before I/O and must not reconfigure while an operation is pending.

The example uses source-only selection (index zero): it guarantees the source
address, not the egress interface. In particular, duplicate IPv4 addresses on
different interfaces cannot be disambiguated by source-only selection. Platform
multicast-interface options and policy remain owner responsibilities; this helper
does not override them or promise unrestricted routing across all interfaces.

Payload/control truncation, missing or malformed packet info, and source/scope
conflicts throw `boost::system::system_error`. A rejected receive has consumed
that datagram; callers decide whether to drop it and continue. Zero-size UDP
datagrams succeed. Send is whole-datagram or error. Both operations check existing
coroutine cancellation before native I/O and propagate cancellation through
Asio readiness waits. Socket-wide cancellation affects all waiters; use individual
cancellation slots for shared-socket operations. Cancellation never retracts an
already completed syscall. Unsupported platforms report `operation_not_supported`.

## Tests

`test_forge_transport` covers chunk lifetime, bounded buffer reuse, frame
encode/decode, decode views without payload allocation, stream/session wrapper
delegation, registry routing and typed error paths.
The `datagram_io_tests` suite adds wildcard IPv4/IPv6 destination and reply-source
checks, zero-length packets, truncation, source/scope validation and cancellation,
with two-second operation bounds. Darwin additionally checks rejection before
send without persistent binding, exact-source-bound sends, matching/mismatching
sticky interface binding and unchanged socket options. Zero-capacity receives use
private one-byte scratch storage to ensure Darwin consumes the actual datagram;
nonempty packets still fail with `message_size`.
`test_forge_package_net_transport_component`
imports the leaf directly and exercises a UDP exchange through the installed target.
