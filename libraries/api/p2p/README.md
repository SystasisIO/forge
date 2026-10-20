# Forge API P2P

`forge_api_p2p` is the typed API binding adapter for negotiated P2P protocol streams.

It owns only API frame binding behavior:

- public modules live under `forge.api.p2p.*`;
- public namespace is `forge::api::p2p`;
- P2P identity, discovery, sessions and protocol streams stay in `forge_net_p2p` / `forge::net::p2p`;
- generic stream frame serving stays in `forge_api_stream`.

The default application protocol is `/forge/api/2`. Every selected stream,
including a product-owned custom protocol id, performs the mandatory symmetric
Forge API wire-v2 hello before accepting calls. This does not change libp2p
security, multiplexing, peer identity or protocol negotiation.

Use this library when an API contract should be published through a P2P node.

Each accepted P2P API stream creates `forge::api::p2p::authenticated_peer` only
from the already authenticated incoming session's `remote_peer`. The binding
places that value in core trusted invocation context for typed request
enrichment, and also preserves the legacy `forge.p2p.remote_peer` trusted
metadata injection for interceptors. The metadata value is not an authority
source, so a client-supplied `forge.p2p.remote_peer` cannot impersonate a peer.
`authenticated_peer` is a trusted source type, not a wire field: product DTOs
specialize `server_supplied<Field>` and read it with
`trusted_invocation::find<authenticated_peer>()`.
The P2P session has already authenticated `remote_peer` through its negotiated
security channel before `api_binding::make_session(...)` constructs the typed
context; that factory exposes the configured stream session when the caller owns
its serve lifecycle.

`forge.api.p2p.publication` is the move-only ownership token used by P2P API
publishers. Destroying it, or calling `close()`, closes admission for that API
generation and requests cancellation of only its owned API sessions.
`async_close()` additionally waits for those sessions to drain. Replacing a
publication installs a new generation atomically; an older token cannot remove
the replacement. This is application-level lifecycle ownership and does not
change libp2p negotiation, Identify, or raw stream semantics.

`publication::stop_token()` is sticky generation-retirement notification,
including replacement of an idle publication. It is not a drain barrier;
retain and await `async_close()` before releasing owned runtime resources.
Retirement callbacks may reenter and run outside registry/state mutexes.
