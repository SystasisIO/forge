# forge_net_tcp

`forge_net_tcp` is the Boost.Asio TCP implementation for the reusable `forge_net_transport`
stream contract. Use it when code needs a raw bidirectional TCP byte stream.

Do not use `forge_net_tcp` for TLS, Yamux, P2P identity, API frame dispatch or
multiaddr parsing. Those layers sit above raw TCP.

## When To Use

- Open or accept raw TCP streams and adapt them into `forge_net_transport`.
- Build a higher transport that needs access to the native socket before
  wrapping it.
- Test transport behavior without TLS or multiplexing.

## When Not To Use

- Do not use raw TCP when the caller requires TLS, ALPN or certificate
  verification. Use `forge_net_stcp`.
- Do not implement API frames directly on top of TCP. Use `forge_api_transport`
  after a stream is established.
- Do not put DNS policy, peer identity, relay logic or product admission policy
  in this library.

## Public Modules

- `forge.net.tcp.connector`
- `forge.net.tcp.listener`
- `forge.net.tcp.connection`
- `forge.net.tcp.options`
- `forge.net.tcp.exceptions`
- `forge.net.tcp.transport`
- `forge.net.tcp`

## Dependencies

- `forge_net_transport`
- `forge_exceptions`
- Boost.Asio

## Examples

### Direct Stream

```cpp
import forge.net.tcp.connector;
import forge.net.tcp.listener;
import forge.net.transport.endpoint;

auto local = forge::net::transport::endpoint{
   .host_type = forge::net::transport::endpoint::host_kind::ip4,
   .protocol = forge::net::transport::endpoint::protocol_kind::tcp,
   .host = "127.0.0.1",
   .port = 0,
};

auto listener = forge::net::tcp::listener{executor, local};
auto connector = forge::net::tcp::connector{executor};
auto connection = co_await connector.async_connect(listener.local_endpoint());
co_await connection.stream.async_write(std::span<const std::uint8_t>{bytes});
```

TCP is a byte-stream transport. Use `connection.stream.async_write(...)` and
`connection.stream.async_read()` for raw bytes. Use
`connection.stream.async_write_frame(...)` and
`connection.stream.async_read_frame()` when the caller needs FORGE length-prefixed
message boundaries over the TCP stream.

### Upgrade Surface

Use `tcp::connection` when another layer needs the native socket before TCP is
converted into a generic `transport::stream`. This is the path used by
`forge_net_stcp` for TLS upgrade.

```cpp
import forge.net.tcp.connection;
import forge.net.tcp.connector;

auto connector = forge::net::tcp::connector{executor};
auto tcp = co_await connector.async_connect_connection(remote);

// Either keep using raw TCP bytes:
co_await tcp.async_write(std::span<const std::uint8_t>{bytes});

// Or hand the socket to a higher layer when this connection has no owner lifetime:
auto socket = std::move(tcp).release_socket();
```

When the connection carries an owner lifetime (for example, a native resource
reservation), transfer it with the socket. The no-output overload rejects that
handoff with `tcp::exceptions::invalid_options` before detaching the socket.

```cpp
std::shared_ptr<void> native_owner;
auto socket = std::move(tcp).release_socket(native_owner);
```

If no upgrade is needed, call `std::move(tcp).into_transport_stream()` or use
`connector.async_connect(...)` directly.

### Registry

```cpp
import forge.net.tcp.transport;
import forge.net.transport.registry;

auto registry = forge::net::transport::registry{};
forge::net::tcp::register_stream(registry, executor);

auto listener = co_await registry.async_listen_stream(local);
auto outbound = co_await registry.async_connect_stream(listener.local_endpoint());
co_await outbound.stream.async_write_frame(payload);
```

## Boundaries

- Depends only on `forge_net_transport`, `forge_exceptions` and Boost.Asio.
- Throws typed `forge::net::tcp::exceptions::*` at the TCP boundary.
- `dns`, `dns4` and `dns6` are connect-only host kinds.
- Listen accepts only concrete `ip4` and `ip6` endpoints.
- IPv6 literals may carry a native interface name or numeric scope in `endpoint::zone`.
  TCP validates that scope before opening a socket; link-local IPv6 requires one.
  Returned endpoints preserve a numeric scope without resolving it back to an interface name.
- TLS-over-TCP belongs to `forge_net_stcp`.

## Security And Common Mistakes

- TCP is not encrypted or authenticated. Do not send credentials or trusted
  control data over raw TCP unless a higher layer provides protection.
- Do not keep detached async operations alive after the owning connector,
  listener or runtime is shutting down.
- Do not assume a byte stream preserves message boundaries. Use transport
  frames when the caller needs messages.

## Tests

- `test_forge_tcp`
