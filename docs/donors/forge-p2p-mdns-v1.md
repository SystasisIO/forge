# P2P mDNS Donor Baseline

## Scope And Status

Stage 6 PR7 adds optional LAN discovery to the native node. The cryptographic
prerequisite is implemented separately from the network service. The mDNS
runtime and interface lifecycle are implemented. Development Linux runs have
passed 28 bilateral positives, 8 namespace-isolation cases and 2 interface-churn
cases. These are not final-SHA acceptance: the canonical promotion invocation
must rebuild/bind the frozen sources and validate all 38 indexed cases. This
note is not a production-support claim. Plugin configuration remains Stage 7.

The private packet codec and bounded policy validation are implemented. Codec
tests cover DNS compression, all four sections, raw labels, QU/cache-flush
flags, individual TXT attributes, malformed packets and resource limits. These
are packet-level tests, not evidence of multicast discovery or interoperability.

Standard `ip6zone` (code 42) is supported by the generic multiaddr codec using
the Go donor's non-empty, slash-free, length-prefixed value contract. Its
golden covers `/ip6zone/en0/ip6/fe80::1/tcp/4001`. This does not perform OS
interface lookup. Native TCP/QUIC use the shared strict endpoint conversion;
reverse conversion preserves numeric scope without interface-name lookup.
Network-origin zones are rejected; link-local mDNS scope comes only from the
receiving local interface. Isolated ULA IPv6 interop is not link-local routing
or cross-platform acceptance evidence.

## Inspected Sources

| Source | Accepted contract |
| --- | --- |
| libp2p specs `6b6203ee6f62938ce67efdb33498173f475851c0`, `discovery/mdns.md` | `_p2p._udp.local`, random instance label, PTR plus individual `dnsaddr` TXT attributes, private `_p2p-X._udp.local` namespace. |
| Pinned Go `p2p/discovery/mdns/mdns.go` | Explicit custom service names and ordinary libp2p addresses; private interop must configure the same name explicitly. |
| Rust `22fb4c784fc55ad8b15d05fdc9f98d663107d4cb`, `protocols/mdns/src/behaviour.rs` and `behaviour/iface.rs` | Per-interface discovery and expiry. Do not copy abort-without-join cleanup or the IPv6 interface-index-zero TODO. |
| Same Rust revision, `protocols/mdns/src/behaviour/iface/dns.rs`, `generate_peer_name` and `query_response_packet` | A random single-label PTR target is valid donor output; do not require an instance-plus-service suffix. The PTR owner must still match our service. |
| Same Rust revision, `protocols/mdns/src/behaviour/iface/query.rs`, `MdnsPeer` parsing | Correlates service PTR answers with TXT records in additionals. An all-records-in-answers encoding does not establish Rust interoperability. |
| Same Rust revision, `transports/pnet/src/lib.rs` | 16-byte fingerprint: Salsa20 with nonce `finprint`, 64 keystream bytes, then SHAKE128 with 16 output bytes. The donor golden was checked against Go-IPFS. |
| Vendored c-ares, `src/lib/record/ares_dns_record.c` and `ares_dns_mapping.c` | Its ordinary DNS class validation rejects mDNS QU/cache-flush high bits. It is not a transparent mDNS packet codec. |
| [RFC 6762 Appendix C](https://www.rfc-editor.org/rfc/rfc6762.html#appendix-C) | Expanded mDNS names allow 255 bytes before the root terminator, 256 bytes total. Encode, uncompressed decode and compressed decode accept that boundary and reject 257 bytes. |

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
- Canonical advertisements place PTR in answers and TXT/SRV/address records in
  additionals (RFC 6763 section 12.1). Direct TXT/SRV/address questions promote
  the requested records into answers. Known-answer suppression is independent
  per record (RFC 6762 section 7.1): a suppressed PTR leaves no unsolicited
  dependency bundle, while a known SRV does not suppress unknown A/AAAA
  dependencies of a surviving PTR. The pinned Rust query/dns paths above and
  the actual 28-case wire matrix motivated and exercised this layout.
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
promoted to production support solely from executable registration.

## Executable Acceptance

`tests/libp2p_interop/mdns_acceptance.py` owns the exact 38-case matrix and raw
receipt validation. Public cases are
`mdns-ipv{4,6}-{tcp,tcp-tls,quic}-{forge-to-go,go-to-forge,forge-to-rust,rust-to-forge}`;
private cases are `mdns-ipv{4,6}-tcp-pnet-{forge-to-go,go-to-forge}`. These expand
to 28 distinct IDs. No remote peer ID/address is passed to either participant;
echo evidence binds the actual authenticated connection and actual transport.
Forge runtime observation counts are not represented as peer/address snapshots.

The 8 isolation IDs are
`mdns-isolation-ipv{4,6}-{mismatched-psk,public-private}-forge-{client,server}`.
Both processes remain alive for at least three seconds after readiness, then
stop explicitly and join gracefully with zero observed activity. A fresh pair
must subsequently discover and authenticate with matching keys on the same
owned network. Deadline expiry is failure, never isolation success. Pinned Go
spawns detached notifier callbacks: evidence covers entered callbacks through
the final snapshot, not a guarantee that every donor callback was joined.
Forge service-error diagnostics remain explicitly unavailable.

The 2 churn IDs are `mdns-churn-ipv{4,6}-forge-dialer`. Checked local `eth0`
down/up transitions must produce observation counts 1/0/1 with the same owned
PID and peer identity. Every captured post-echo sample must retain exactly one
opened and zero closed sessions. Epoch-bound snapshots are saved before reuse;
final lifecycle and echo receipts are checked after join.

Run `promote_stage6_acceptance.py --suite mdns` with the existing canonical
arguments and `--expected-head` set to the final clean commit. The shared
checker binds the execution receipt, source/fixture locks, binaries, donors,
manifest, full evidence index and cleanup. Standalone edited JSON or earlier
development receipts cannot substitute for that invocation. Rust private
mDNS remains excluded because the pinned donor cannot configure its service
namespace. The operational fingerprint is not the network fingerprint.

The CTest entry `test_forge_libp2p_mdns_evidence` explicitly runs all seven
`test_mdns*` Python modules with bytecode disabled and a 120-second timeout.
For final clean-head live acceptance, configure at that HEAD and run
`cmake --build <build-dir> --target test_forge_p2p_mdns_acceptance` inside the
prepared Linux isolation environment with its outer loopback interface UP.
The target invokes the canonical wrapper with `--suite mdns`, uses the dedicated
`${FORGE_P2P_STAGE6_PROMOTION_DIRECTORY}/mdns` directory and retains the same
fixture/inventory dependencies and exact-HEAD gate as the AutoNAT target.
