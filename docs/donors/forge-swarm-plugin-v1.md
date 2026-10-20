# Swarm Plugin Composition

Source traceability for PR7. Inspection is not runtime acceptance; focused
results and fixed source revisions are recorded separately.

## Donor Boundaries

- go-libp2p-kad-dht `d40b14b5b2cc713f78173f411b8eec4c1827552f`,
  `routing.go:588`: provider AddrInfo is made available through temporary peer
  addresses. The result must remain dialable even for client-only providers
  that are not members of the Kademlia routing table.
- rust-libp2p `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`,
  `protocols/kad/src/behaviour.rs:1235,2272`: query-owned addresses participate
  in dial acquisition without becoming durable membership facts.
- Forge native P2P owns DNSADDR expansion, address racing, expected-peer
  verification, connection gating, scoped resources and relay fallback. The
  plugin does not reproduce any of those algorithms.
- Existing Swarm runtime/Core and file-storage donor notes remain authoritative
  for picking, choking, verification, resume, admission and quotas. This adapter
  does not claim BitTorrent, Bitswap or IPFS application-wire interoperability.

## Accepted Composition

An owned runtime route retains bounded immutable backend context. P2P address
hints remain scoped to one open, including all available relay carriers and
DNSADDR roots. Known independently learned roots take priority at the common
root bound; hints cannot evict them. Hint-only outcomes neither persist endpoint
backoff nor manufacture DHT membership or relay reservations. Authenticated
Identify facts remain independently eligible for the native peer store.

Circuit cancellation ownership follows its admitted resource from HOP through
Noise and muxer negotiation. The final application stream has its own admission
and deadline scope. Neither cancellation closes a shared relay carrier. Failed
handoff must join resource close even while security wrappers retain references;
the dispatcher-only `async_close_unescaped()` use-count rule is not suitable here.

The open root owns cancellation and join, while each native phase consumes the
same absolute setup budget and retains its own timeout classification. A second
outer timer must not recast a remote timeout as local cancellation or suppress
peer failure accounting. Subscription announcement has its own bounded stream
scope after session commit; failing it cannot close the authenticated carrier.
An AutoRelay refresh wait is cancelable independently of its shared manager.

Provider registrations support a nonblocking renewal stop while retaining the
explicit withdrawal barrier. Publication generation retirement issues a sticky
stop token, including replacement without incoming RPC. The token means lost
admission, not completed drain. Swarm cancels that generation's engine and joins
the existing runtime owners; it adds no polling or renewal worker.

Catalog mode uses a guarded named Object Store handle, the PR3 catalog algorithm
and an explicitly supplied application-owned DB executor. There is no raw driver
escape or network-executor fallback for blocking database flush. File mode opens
no additional Swarm catalog; official P2P still has its own DB/Secrets dependencies.

Admission is product-owned or explicitly free. Realm/domain hashing separates
discovery namespaces, not permission. Encrypted erasure fragments remain behind
the specialized paid-provider API until a separate verified product adapter exists.

## Evidence Limits

Focused PR7 tests qualify composition and lifecycle. The independent PR8 gate
owns the multi-process, multi-transport Swarm acceptance. Existing Go/Rust
evidence qualifies underlying libp2p protocols, not the custom Forge Swarm API.

The source inventory refresh includes the already changed resolver public
snapshot at the PR6 base, as well as the new native/node options. Feature
classification and protocol claims are unchanged; a snapshot hash alone is not
live interoperability evidence.
