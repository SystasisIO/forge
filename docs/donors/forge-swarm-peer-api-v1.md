# Swarm peer protocol, API and conversion

Status: Preview. This note covers PR5 values/contracts, not the PR6 production
service, admission/quota implementation, discovery or product integration.

## Fixed Donor Baseline

Local `donors/libtorrent` revision:
`b4e9e2471e7abce2a58585a7d7fbc4e8d1edc47e`.
It is a source reference/test oracle, not a production link dependency.

| Inspected source | Accepted pattern | Forge ownership and checks |
| --- | --- | --- |
| `src/ut_metadata.cpp`, `simulation/test_metadata_extension.cpp` | Bounded metadata pieces, complete identity verification before accepting metadata | Protocol reader/assembler, ordered chunks, canonical Raw and requested-ID tests |
| `src/bt_peer_connection.cpp` | Explicit typed messages and strict size/geometry validation | Protocol message variants, golden discriminants, malformed/oversized item tests |
| `src/peer_connection.cpp` | Availability is distinct from local verification and permission; independent input/output progress | Reuse PR4 Core request ownership; real Forge.API duplex/half-close/cancel tests |
| `src/hash_picker.cpp`, `test/test_hash_picker.cpp` | Authenticate piece hashes against an expected root with bounded proofs | Core Merkle verification through manifest-aware message validation |

Protocol references: [BEP 9](https://www.bittorrent.org/beps/bep_0009.html),
[BEP 3](https://www.bittorrent.org/beps/bep_0003.html),
[BEP 52](https://www.bittorrent.org/beps/bep_0052.html).

## Deliberate Differences

- Swarm is a Forge.API contract over canonical Forge Raw DTOs, not a BitTorrent,
  Bitswap or IPFS wire implementation. No donor handshake, bencoding or framing
  is copied. Metadata pieces are 16 KiB; descriptor bytes/identity are Core's.
- Availability uses Core's little-bit-first bitmap, not BitTorrent's bit order.
  File-local piece IDs and per-session request IDs reuse Core semantics.
- PR4 Core retires canceled request ownership and rejects late responses; this
  is not BEP 52's obligation to send a terminal response after cancellation.
  PR6 must document and enforce that chosen state machine, not claim identical
  peer scheduling behavior.
- A piece hash proof authenticates the expected piece hash. Receiving an
  individual block or HAVE does not prove local payload durability/verification.
- Session order, duplicate/late request handling, choke scheduling and quota
  are runtime/Core concerns. Stateless DTO validation cannot prove them.

## Reused Forge Components

`swarm_core` supplies canonical descriptor/identity, file geometry and Merkle
proofs. `raw` supplies serialization and bounded exact decoding. `api_core`
supplies generated live streams, cancellation, flow control, terminal errors
and trusted invocation. `api_auth::authenticated_caller` is the existing required
server-supplied value; no second identity or authorization system is added.
The P2P binding already derives it from the authenticated TLS/Noise session.

The normal API descriptor customization installs Protocol's stricter input and
output item validators, including in generated proxy descriptors. The additive
builder setters use existing descriptor callbacks, not a second codec path.

`swarm_conversion` specializes `forge::core::conversion<Result>` explicitly.
Exported metadata omits every local DB authority field. Validated imports
produce a descriptor only; they cannot allocate a persisted record, restore a
checkpoint, or grant permission. ObjectDB I/O remains with the owner.

## Evidence Boundary

Protocol fixtures freeze every variant discriminant, field ordering and enum
width. Tests cover bounded/canonical decode, metadata assembly, bitmap tails,
block geometry, proofs and corruption. API tests run a real TCP binding with
one-item windows and server-supplied context. They do not substitute for the
production runtime or product authorization tests in PR6.

Independent installed Protocol/API consumers reject DB/storage/P2P in their
reachable link/module dependency closure. Unrelated target declarations in the
common installed export are not themselves dependencies.
Conversion deliberately imports DB models but not concrete drivers or storage.
Focused acceptance logs and exact-head reviews are recorded outside build
directories before their cleanup. No BitTorrent interoperability is claimed.
