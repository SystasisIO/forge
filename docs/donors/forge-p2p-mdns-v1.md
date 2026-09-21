# P2P mDNS Donor Baseline

## Scope And Status

Stage 6 PR7 adds optional LAN discovery to the native node. The cryptographic
prerequisite is implemented separately from the network service. The mDNS
runtime, interface lifecycle and bilateral live evidence remain pending; this
note is not a production-support claim. Plugin configuration remains Stage 7.

## Inspected Sources

| Source | Accepted contract |
| --- | --- |
| libp2p specs `6b6203ee6f62938ce67efdb33498173f475851c0`, `discovery/mdns.md` | `_p2p._udp.local`, random instance label, PTR plus individual `dnsaddr` TXT attributes, private `_p2p-X._udp.local` namespace. |
| Pinned Go `p2p/discovery/mdns/mdns.go` | Explicit custom service names and ordinary libp2p addresses; private interop must configure the same name explicitly. |
| Rust `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`, `protocols/mdns/src/behaviour.rs` and `behaviour/iface.rs` | Per-interface discovery and expiry. Do not copy abort-without-join cleanup or the IPv6 interface-index-zero TODO. |
| Same Rust revision, `transports/pnet/src/lib.rs` | 16-byte fingerprint: Salsa20 with nonce `finprint`, 64 keystream bytes, then SHAKE128 with 16 output bytes. The donor golden was checked against Go-IPFS. |
| Vendored c-ares, `src/lib/record/ares_dns_record.c` and `ares_dns_mapping.c` | Its ordinary DNS class validation rejects mDNS QU/cache-flush high bits. It is not a transparent mDNS packet codec. |

The private service namespace is specified by libp2p; its fingerprint
computation is a donor convention, not specified by the mDNS document. The
existing Forge SHA-256 operational fingerprint remains unchanged and is not
used as this network fingerprint. Its 64 hexadecimal digits would also exceed
the DNS label limit when prefixed by `_p2p-`.

## Accepted Composition

- Crypto owns Salsa20 and SHAKE128 through the existing private backends;
  PNet owns the fingerprint composition. P2P does not call vendor crypto.
- A bounded private mDNS codec preserves class flags, TXT segments and DNS
  compression semantics. Do not patch c-ares or mask packet bits before decode.
- One node-owned service uses existing Asio execution, lifecycle tracking and
  resource reservations. Per-interface sockets retain their exact interface
  index and IPv6 scope. Removal and shutdown cancel and join workers before
  releasing their reservations.
- Interface change notifications cause generation-tagged snapshot
  reconciliation. Native adapters belong to production construction; test
  clocks and interface/socket ports remain private test dependencies.
- A bounded mDNS registry owns endpoint leases. Topology replaces only the
  mDNS source snapshot and performs normal authenticated dialing. mDNS TTL
  must not overwrite the peer-store record's shared discovery expiry or erase
  Identify, configured, DHT or other source state.
- Expiry or interface removal withdraws discovery hints, not established
  sessions. No second dial loop, durable-cache schema or plugin network loop
  is introduced. Static-only topology with enabled mDNS is rejected.

## Required Evidence

Prerequisite tests cover Salsa20 upstream vectors, SHAKE128 vectors and output
bounds, moved keys, the pinned Rust fingerprint, unchanged operational
fingerprints and independent package consumers.

Network acceptance additionally requires public Forge/Go/Rust discovery,
private Forge/Go separation, the explicit pinned Rust private-service
limitation, bounded malformed packets, TTL and source withdrawal, interface
churn, exact resource cleanup and shutdown. No runtime capability may be
promoted from planned until those tests and fresh live receipts exist.
