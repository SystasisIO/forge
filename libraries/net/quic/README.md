# forge_net_quic

`forge_net_quic` owns the QUIC transport layer over ngtcp2, OpenSSL 3.0+ and Boost.Asio.
It exposes endpoints, security options, listeners, connectors, connections and
framed streams without defining application protocols.

## Transport Alignment Checkpoint

`forge_net_quic` is a native `forge_net_transport` session transport. The
`forge.net.quic.transport` module exposes:

- `quic::as_transport_stream(...)` and `quic::as_transport_session(...)` for
  adapting existing QUIC objects.
- `quic::make_session_connector(...)` and
  `quic::make_session_listener(...)` for direct `transport::session` usage.
- `quic::register_session(...)` for `transport::registry` integration.
- `quic::to_transport_limits(...)` and `quic::from_transport_limits(...)` for
  explicit limit mapping.

QUIC is already a multiplexed session transport. TCP and STCP remain
byte-stream transports; they become sessions only after a muxer such as Yamux.

## Endpoint Address Family

`quic::endpoint::family` constrains DNS resolution only. `any` maps a DNS host
to `transport::endpoint::host_kind::dns`; `ipv4` and `ipv6` map it to `dns4`
and `dns6`, respectively. A numeric IPv4 or IPv6 `endpoint::host` determines
its actual family regardless of this trailing field and round-trips as `ip4` or
`ip6`.

The QUIC transport connector validates `ip4` with `make_address_v4` and `ip6`
with `make_address_v6` before opening a socket. DNS host kinds are valid only
for connecting; listeners reject them.

## When To Use

- Need multiplexed, TLS-backed streams over UDP.
- Need pinned certificate fingerprints or mTLS-style checks at transport level.
- Need bounded frame sizes, connection slots and packet queues.

## When Not To Use

- Do not put peer discovery or relay policy here; that is `forge_net_p2p`.
- Do not put application protocol messages here; use framed streams as substrate.
- Do not disable peer verification outside explicit tests.

## Client Address Tokens

QUIC Retry tokens are valid for 10 seconds. A verified server issues one opaque
Forge regular `NEW_TOKEN` with a 60 minute validity period; connector-owned
client caches retain one unused token per canonical remote for 55 minutes.
The next connection consumes that token once before its Initial is created.

`client_options::client_tokens` is Preview. When absent, `connector` uses its
own bounded cache. An engaged value with both callbacks empty explicitly
disables caching; an engaged value must otherwise provide both `take` and
`store`. Callbacks are synchronous, nonblocking and may be invoked
concurrently, so custom stores must be thread-safe. A received `NEW_TOKEN` is
only stored after ALPN and configured peer verification succeed; callback
errors or a cache refusal never fail the connection.

An invalid, expired or foreign regular token is treated as unvalidated and
receives Retry. A Retry2 token that decrypts but fails address, expiry or
connection-ID verification is rejected with `INVALID_TOKEN`; unreadable Retry2
and failed legacy Retry tokens receive a fresh Retry.

## Public Modules

- `forge.net.quic.endpoint`, `forge.net.quic.options`, `forge.net.quic.security`.
- `forge.net.quic.listener`, `forge.net.quic.connector`, `forge.net.quic.connection`.
- `forge.net.quic.stream`, `forge.net.quic.framed_stream`.
- `forge.net.quic.runtime`, `forge.net.quic.metrics`, `forge.net.quic.exceptions`.

Target: `forge_net_quic`.

Dependencies: `forge_asio`, Boost.Asio, OpenSSL::SSL/Crypto, ngtcp2 and
ngtcp2 crypto OpenSSL backend.

## Examples

### Parse Endpoint

```cpp
import forge.net.quic.endpoint;

auto endpoint = forge::net::quic::parse_endpoint("127.0.0.1:9443");
auto authority = endpoint.authority();
```

### Connect With Expected Peer

```cpp
#include <boost/asio/awaitable.hpp>

import forge.net.quic.connector;
import forge.net.quic.options;
import forge.net.quic.security;

boost::asio::awaitable<void> connect_with_pin(
   forge::net::quic::connector& connector,
   forge::net::quic::endpoint endpoint) {
   auto options = forge::net::quic::client_options{
      .certificate_pem = client_certificate_pem,
      .private_key_pem = client_private_key_pem,
   };
   options.security.expected_sha256_fingerprint = expected_server_fingerprint;

   forge::net::quic::connection connection = co_await connector.async_connect(endpoint, options);
   use_connection(std::move(connection));
}
```

For CA-based verification, trust is explicit and host-bound:

```cpp
boost::asio::awaitable<void> connect_with_ca(
   forge::net::quic::connector& connector,
   forge::net::quic::endpoint endpoint) {
   auto options = forge::net::quic::client_options{};
   options.security = forge::net::quic::security_options{
      .verify_peer = true,
      .trusted_ca_pem = trusted_ca_bundle_pem,
   };

   // The certificate must be valid for endpoint.host through DNS/IP SAN matching.
   forge::net::quic::connection connection = co_await connector.async_connect(endpoint, options);
   use_connection(std::move(connection));
}
```

### Accept Connections

```cpp
#include <boost/asio/awaitable.hpp>

import forge.net.quic.listener;

boost::asio::awaitable<void> accept_one(forge::asio::runtime& runtime) {
   auto server_options = forge::net::quic::server_options{
      .certificate_pem = server_certificate_pem,
      .private_key_pem = server_private_key_pem,
   };

   auto listener = forge::net::quic::listener{
      runtime,
      forge::net::quic::parse_endpoint("127.0.0.1:9443"),
      server_options,
   };

   forge::net::quic::connection inbound = co_await listener.async_accept();
   handle_inbound(std::move(inbound));
}
```

### Open A Framed Stream

```cpp
#include <boost/asio/awaitable.hpp>

import forge.net.quic.framed_stream;

boost::asio::awaitable<void> write_payload(forge::net::quic::connection& connection) {
   forge::net::quic::stream stream = co_await connection.async_open_stream();
   auto framed = forge::net::quic::framed_stream{std::move(stream), {.max_frame_size = 1 << 20}};
   co_await framed.async_write_frame(payload);
}
```

### Bind API Frames To QUIC Streams

`forge.api.quic.binding` is the API-over-QUIC adapter. It keeps QUIC transport policy in
`forge_net_quic`, contract/error semantics in `forge_api_core`, and delegates frame-loop
mechanics to `forge.api.stream`.

```cpp
import forge.api.core.exceptions;
import forge.api.core.types;
import forge.api.core.descriptor;
import forge.api.core.error_projection;
import forge.api.core.handle;
import forge.api.core.connection;
import forge.api.core.registry;
import forge.api.core.binding;
import forge.api.core.dispatcher;
import forge.api.quic.binding;

auto plan = forge::api::core::binding()
   .serve(app.apis())
   .export_api<cache>({.id = {"cache"}, .major = 1, .min_revision = 8})
   .build();

auto binding = forge::api::quic::api()
   .use(plan)
   .codec({"forge.raw"})
   .max_concurrent_calls(256)
   .deadline(std::chrono::seconds{5})
   .build();

boost::asio::awaitable<void> serve_api_stream(forge::net::quic::connection& connection) {
   auto stream = co_await connection.async_accept_stream();
   co_await binding.accept(std::move(stream));
}
```

`forge.api.quic.binding` does not own certificates, ALPN, listener/connector setup or
packet-level limits. Those remain in `forge_net_quic` transport options. It also does
not own the generic API frame state machine; that lives in `forge.api.stream`.

### Decode Frames Without A Connection

```cpp
import forge.net.quic.framed_stream;

auto encoded = forge::net::quic::encode_frame(payload);
auto decoded = forge::net::quic::decode_frame(encoded);
if (decoded.status == forge::net::quic::frame_decode_status::complete) {
   consume(decoded.payload);
}
```

### Verify A Certificate Fingerprint

```cpp
import forge.net.quic.security;

auto fingerprint = forge::net::quic::certificate_sha256_fingerprint_from_pem(certificate_pem);
auto normalized = forge::net::quic::normalize_sha256_fingerprint(fingerprint);
```

## Backpressure And Failure Model

Transport limits cover stream count, queued bytes and inbound packet queue size.
Timeouts are scoped to handshake/connect/read/write phases so callers can return
typed failures instead of vague network errors.

Graceful connection close serializes and sends a standard QUIC application
`CONNECTION_CLOSE` before local transport teardown. Peers therefore observe a
terminal close promptly rather than retaining higher-level session ownership
until the QUIC idle timeout. Local failure paths may still terminate transport
immediately when a close frame cannot be emitted safely. Late packets for a
released connection are isolated according to ngtcp2's silent-drop contract;
they neither consume a listener slot indefinitely nor fail a concurrent
`async_accept()` for another connection.

## UDP Routing And Scope

A client uses one dedicated UDP socket, permanently connected to its selected
remote tuple. Open, bind, connect and local-endpoint discovery happen before
creating the ngtcp2 connection. Its local address is concrete, not a wildcard.
Client sends use the connected socket without a destination argument; every
ngtcp2 output path is copied with its packet and checked against the owned local
and remote endpoints. A changed path fails rather than silently using the old
route. Client migration, remote rebinding and server preferred addresses are not
supported. This is an explicit implementation policy, not a consequence inferred
solely from the advertised `disable_active_migration` transport parameter.

Client readiness waits for ngtcp2 `handshake_confirmed`, when both endpoints
agree that the handshake finished, rather than local cryptographic completion.
The existing connect/handshake deadlines still apply. Server readiness remains
on `handshake_completed`, which already implies confirmation for a server.
Peer verification and token-store publication remain gated after client readiness.

Native UDP send and receive failures retain a typed handshake terminal cause
before cleanup. Peer refusal/unreachability is a connection failure, not caller
cancellation; other native failures retain an internal-error classification.
Cleanup flags do not replace that cause. A previously committed connect timeout
or cancellation still wins, and `operation_aborted` is not relabeled as a peer
failure.

The shared server socket remains unconnected. Transport `datagram_io` supplies
the actual destination, remote endpoint and receive-interface index through
Initial, Retry, INVALID_TOKEN, admission and queued packet processing. Sends use
the path returned by ngtcp2, including connection-close packets. IPv6 zones are
represented separately from the host and preserved as native scope IDs.

Ordinary unicast replies require exact source-address affinity, not interface
pinning. The received interface index remains packet metadata. Scoped IPv6 or
an explicitly scoped listener requires the matching interface; conflicting or
unsupported constraints fail without retrying with a weaker route. On Darwin,
this ordinary IPv4 policy uses source-only packet info and does not promise
interface selection for duplicate local addresses. No temporary socket-option
mutation is performed. A consumer requiring fixed interface ownership must
provide an appropriate persistent binding.

Gate tickets serialize sends across suspension points, including shared-listener
replies. Send waits are bounded to five seconds; cancellation races join their
losing operations. Connection cancellation never cancels the shared listener FD.
Listener stop closes its gate and socket, waking queued and active operations.

## Initial Tokens

The listener validates the remote address with an encrypted QUIC Retry token
before allocating server connection or TLS state. Tokens are bound to the
remote endpoint, Retry connection ID and original destination connection ID,
and expire after the ngtcp2 donor-compatible ten-second validation window.

An empty, unknown, proprietary, or invalid `NEW_TOKEN` is unvalidated and
receives a fresh Retry. A Retry2 (`B7`) token that is readable but fails
address, expiry or connection-ID verification receives a stateless
`CONNECTION_CLOSE` with `INVALID_TOKEN`. Unreadable Retry2 tokens and failed
legacy (`B6`) tokens receive a fresh Retry because the legacy verifier cannot
distinguish foreign ciphertext from a readable invalid token. Valid Retry and
`NEW_TOKEN` tokens are passed to ngtcp2 with their respective token types
without poisoning a concurrent `async_accept()`.

`connection_metrics::new_tokens_submitted` counts frames accepted by ngtcp2
for transmission. It does not claim UDP delivery or client receipt.

`server_options::inbound_admission` is an optional early-admission hook for
higher-level resource managers. QUIC invokes it only after initial-token
validation but before allocating the server connection and TLS state. Returning
an empty token, or throwing, rejects that connection attempt; a non-empty token is held
until the connection closes or a higher-level transport adapter takes
ownership through the private connection-access bridge. Leaving the callback
empty keeps the standalone QUIC listener unrestricted by an external admission
layer.

## Security Notes

OpenSSL 3.0+ is the supported TLS backend. Fingerprint and mTLS failures are
correctness failures, not warnings. CA-based client verification binds the peer
certificate to the requested endpoint host; SNI alone is not treated as identity
verification. Pinned fingerprints and custom verifiers are explicit trust paths;
they do not implicitly opt into CA hostname checks. Test certificates must not
become application defaults.

## Risks And Anti-Patterns

- Do not disable peer verification to work around certificate issues. Fix trust
  material or use an explicit pinned/custom verifier path.
- Do not confuse SNI with identity verification. CA-based verification must bind
  the certificate to the requested endpoint host.
- Do not raise frame/queue limits without backpressure tests. Oversized frames
  are a memory pressure and denial-of-service vector.
- Do not define application API envelopes in QUIC handlers. Use `forge.api.quic.binding` and
  `forge::api::core::frame` for typed API calls over QUIC streams.
- Do not swallow handler exceptions in detached stream tasks; convert expected
  failures into typed `forge_exceptions` values or API error frames.
- Do not treat `.deadline(...)` or `.max_concurrent_calls(...)` as documentation
  only; API frames are checked by the call runtime before application handlers run.
- Do not put ALPN, certificate or listener lifecycle options into
  `forge.api.quic.binding`; those belong to the transport owner.

## Typical Mistakes

- Do not put peer discovery or relay fallback in `forge_net_quic`; use `forge_net_p2p`.
- Do not use insecure test settings as application defaults; identity and ALPN
  checks are part of correctness.
- Do not bypass `transport_limits` for "temporary" large frames without adding a
  backpressure test.

## Tests

`test_forge_quic_p2p` covers endpoint parsing, frame codec, loopback handshakes,
parallel streams, loss/delay/reorder fault proxy, mTLS, pinned fingerprints and
backpressure limits.
