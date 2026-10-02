import copy
import hashlib
import unittest

from autorelay_evidence import ECHO, HOP, IDENTIFY, PUSH, _connection, _native_pushes, circuit, validate_case
from autorelay_wire import address_bytes, varint, validate_push_frame
from rust_upgrade_evidence import BASE58, _peer

NOW = 1_700_000_000_000
def public_peer(byte):
    key = b"\x08\x01\x12\x20" + bytes([byte]) * 32
    raw = b"\x00" + varint(len(key)) + key
    number, encoded = int.from_bytes(raw, "big"), ""
    while number:
        number, digit = divmod(number, 58)
        encoded = BASE58[digit] + encoded
    return "1" * (len(raw) - len(raw.lstrip(b"\0"))) + encoded


TARGET, RELAY, REPLACEMENT, OBSERVER, SOURCE = (public_peer(byte) for byte in range(1, 6))


def push_wire_receipt(addresses, index=1, key_byte=1):
    public = b"\x08\x01\x12\x20" + bytes([key_byte]) * 32
    payload = b"\x0a" + varint(len(public)) + public
    for address in addresses:
        encoded = address_bytes(address)
        payload += b"\x12" + varint(len(encoded)) + encoded
    framed = varint(len(payload)) + payload
    return {"protocol": PUSH, "direction": "inbound", "connection_trace_id": 1, "stream_trace_id": index,
            "framed_hex": framed.hex(), "read": {"complete_frames": True, "invalid_or_over_limit": False,
                "framed_bytes": len(framed), "frames": 1, "framed_sha256": hashlib.sha256(framed).hexdigest()}}


def reservation_wire_receipt(addresses=None, signed=False, relay=RELAY, target=TARGET):
    addresses = [address(relay)] if addresses is None else addresses
    expiry = (NOW + 9000) // 1000

    def field(number, value):
        return varint((number << 3) | 2) + varint(len(value)) + value

    reservation = b"\x08" + varint(expiry)
    for value in addresses:
        reservation += field(2, address_bytes(value))
    if signed:
        public = b"\x08\x01\x12\x20" + bytes([2]) * 32
        payload = field(1, _peer(relay)) + field(2, _peer(target)) + b"\x18" + varint(expiry)
        envelope = field(1, public) + field(2, b"\x03\x02") + field(3, payload) + field(5, b"\x01" * 64)
        reservation += field(3, envelope)
    response = b"\x08\x02" + field(3, reservation) + b"\x28\x64"
    framed = varint(len(response)) + response
    receipt = {"basis": "passive_native_client_HOP_STATUS_response", "protocol": HOP, "status": 100,
               "expires_unix_ms": expiry * 1000, "addresses": addresses, "unix_ms": NOW+1000,
               "connection_trace_id": 1, "stream_trace_id": 1, "framed_hex": framed.hex(), "voucher": signed,
               "read": {"complete_frames": True, "invalid_or_over_limit": False, "framed_bytes": len(framed),
                        "frames": 1, "framed_sha256": hashlib.sha256(framed).hexdigest()}}
    if signed:
        receipt.update(voucher_validated=True,
                       voucher_validation_basis="pinned_SignedEnvelope_signature_domain_payload_signer_peer_expiry",
                       voucher_relay=relay, voucher_peer=target, voucher_expiration=expiry)
    return receipt


def address(peer=RELAY, port=12345):
    return f"/ip4/127.0.0.1/udp/{port}/quic-v1/p2p/{peer}"


def circuit_address(peer=RELAY):
    return address(peer) + "/p2p-circuit/p2p/" + TARGET


def connection(peer, relayed=False):
    return {"connection_id": "real-connection-1", "peer_id": peer,
            "remote_addr": circuit_address() if relayed else address(peer),
            "negotiated_transport": "/quic-v1"}


def process(command, extra=()):
    return {"pid": 123, "command": ["/fixture", command, "--scenario", "autorelay", "--transport", "quic", *extra],
            "terminal_status": {"termination": "graceful", "exit_code": 0}}


def actor(implementation, role, peer, result):
    return {"process": process("autorelay-destination" if role == "destination" else "autorelay-" + role),
            "ready": {"implementation": implementation, "status": "ready", "peer_id": peer},
            "result": {"schema_version": 1, "implementation": implementation, "role": role,
                       "peer_id": peer, "scenario": "autorelay", "transport": "quic", "complete": True,
                       "overflow": False, "fixture_task_lifecycle": {"fixture_owned_tasks_joined": True,
                           "overflow": False, "errors": []}, **result}}


def observation(elapsed, relay=None, expiry=None, phase="running"):
    stopped = phase in ("stopped", "post_stop")
    return {"elapsed_ms": elapsed, "unix_ms": NOW + elapsed, "phase": phase, "reachability": "unknown",
            "stopped": stopped, "discovery_attempts": 3,
            "addresses": [circuit_address(relay)] if relay else [],
            "reservations": [{"relay_peer_id": relay, "expires_unix_ms": NOW + expiry, "id": 1, "ttl_ms": 8000,
                "max_streams": 64, "max_bytes": 4096, "max_queued_bytes": 4096, "endpoints": [address(relay)],
                "voucher_present": False, "remote_limit": None}] if relay else [],
            "autorelay": {"enabled": True, "running": not stopped and phase != "before_start", "permitted": bool(relay),
                "stopping": stopped, "candidates": 1 if relay else 0, "pending_reservations": 0,
                "reservations": 1 if relay else 0, "automatic_reservations": 1 if relay else 0, "waiting_refreshes": 0,
                "max_candidates": 4, "max_parallel_reservations": 1, "target_reservations": 1,
                "refreshes": 3, "attempts": 3, "successes": 3, "failures": 0, "renewals": 1 if elapsed >= 6000 else 0,
                "invalidated_completions": 0},
            "sessions": [{"connection_id": 1, "peer_id": relay, "identified": True, "direct": True, "closed": False}] if relay else [],
            "peers": [{"peer_id": relay, "protocols": [HOP]}] if relay else []}


def echo(phase, peer, elapsed):
    return {"phase": phase,
            "process": process("dial-relay", ["--peer-id", TARGET, "--relay-peer-id", peer, "--relay-addr", address(peer)]),
            "result": {"implementation": "go", "scenario": "autorelay", "status": "ok", "relay_echo": True,
                       "protocol": ECHO, "echo_bytes": 10, "relay_peer": peer, "target_peer": TARGET,
                       "local_peer_id": SOURCE, "relayed_addr": circuit_address(peer), "unix_ms": NOW + elapsed,
                       "echo_connection": {**connection(TARGET, True), "remote_addr": circuit_address(peer)},
                       "relay_connection": connection(peer)}}


def valid_case():
    events = []
    for revision, elapsed, peer in (("before_acquire", 500, None), ("acquired", 1500, RELAY),
                                   ("withdrawn", 11000, None), ("replacement", 12500, REPLACEMENT),
                                   ("replacement_withdrawn", 14500, None)):
        events.append({**connection(TARGET), "kind": "identify", "protocol": IDENTIFY,
                       "basis": "independent_authenticated_identify_stream", "revision": revision,
                       "unix_ms": NOW + elapsed, "addresses": [circuit_address(peer)] if peer else []})
    native = [{**connection(TARGET), "kind": "reservation_response", "protocol": HOP,
               "capture_complete": True, "status": 100, "unix_ms": NOW + elapsed,
               "expires_unix_ms": NOW + expiry} for elapsed, expiry in ((1000, 9000), (6000, 14000))]
    for sequence, (elapsed, peer) in enumerate(((1400, RELAY), (10900, None), (12400, REPLACEMENT), (14400, None)), 1):
        events.append({**connection(TARGET), "kind": "identify_push", "protocol": PUSH,
                       "basis": "native_EvtPeerProtocolsUpdated_then_EvtPeerIdentificationCompleted",
                       "native_update_sequence": sequence*2, "native_completed_sequence": sequence*2+1,
                       "unix_ms": NOW+elapsed, "addresses": [circuit_address(peer)] if peer else []})
    rows = [observation(0, phase="before_start"), observation(1000, RELAY, 9000),
            observation(6000, RELAY, 14000), observation(11000), observation(12000, REPLACEMENT, 20000),
            observation(15000, phase="stopped"), observation(16200, phase="post_stop")]
    replacement = [{**connection(TARGET), "kind": "connection", "unix_ms": NOW + 12000},
                   {"kind": "reservation_accepted", "peer_id": TARGET, "protocol": HOP,
                    "basis": "native_relay_ReservationReqAccepted", "renewed": False, "unix_ms": NOW + 12010}]
    return {"scenario": "autorelay", "scenario_id": "autorelay-lifecycle-quic-go-go-forge", "cleanup_errors": [],
            "case": {"source": "go", "relay": "go", "destination": "forge", "transport": "quic", "kind": "lifecycle"},
            "raw": {"relay": actor("go", "service", RELAY, {"events": native}),
                    "replacement": actor("rust", "service", REPLACEMENT, {"events": replacement}),
                    "destination": actor("forge", "destination", TARGET, {"observations": rows,
                        "reservations_basis": "diagnostics.snapshot.relay_reservations",
                        "operation_basis": "async_start_authenticated_connect_only"}),
                    "observer": actor("go", "observer", OBSERVER, {"events": events})},
            "echoes": [echo("acquired_echo", RELAY, 2000), echo("renewed_echo", RELAY, 10000),
                       echo("replacement_echo", REPLACEMENT, 13000)],
            "faults": [{"kind": "relay_stop", "peer_id": RELAY, "unix_ms": NOW + 10500},
                       {"kind": "relay_stop", "peer_id": REPLACEMENT, "unix_ms": NOW + 14000}]}


def valid_service_case(destination="rust", signed=False):
    source = "go" if destination == "rust" else "rust"
    ready = {"implementation": destination, "role": "destination", "peer_id": TARGET, "status": "ready",
             "protocol": HOP, "relay_peer_id": RELAY, "relay_connection": connection(RELAY),
             "expires_unix_ms": NOW+9000, "voucher": signed}
    if destination == "rust":
        ready.update(reservation_accepted=True,
                     reservation_basis="native_relay_client_ReservationReqAccepted_and_NewListenAddr",
                     native_reservation_receipt=reservation_wire_receipt(signed=signed))
    else:
        ready.update(reservation_basis="native_relayclient.Reserve", voucher_validated=signed,
                     voucher_validation_basis="native_Reserve_ConsumeEnvelope_signature_then_fixture_identity_expiry_checks",
                     voucher_relay=RELAY, voucher_peer=TARGET, voucher_expiration=(NOW+9000)//1000)
    echoed = echo("service_echo", RELAY, 2000)
    echoed["result"]["implementation"] = source
    return {"scenario": "autorelay", "scenario_id": f"autorelay-service-quic-{source}-forge-{destination}",
            "cleanup_errors": [], "case": {"source": source, "relay": "forge", "destination": destination,
                "transport": "quic", "kind": "service"},
            "raw": {"relay": actor("forge", "service", RELAY, {"observations": [
                        {"relay_bytes": 10, "service_reservations": 1, "stopped": False},
                        {"relay_bytes": 10, "service_reservations": 0, "stopped": True}]}),
                    "destination": {"ready": ready, "process": process("destination")}},
            "echoes": [echoed], "faults": []}


class AutoRelayEvidenceTests(unittest.TestCase):
    def test_valid_lifecycle(self):
        self.assertEqual(validate_case(valid_case()), [])

    def test_native_service_acceptance_and_vouchers_are_bound_to_actual_response(self):
        for destination in ("go", "rust"):
            for signed in (False, True):
                with self.subTest(destination=destination, signed=signed):
                    self.assertEqual(validate_case(valid_service_case(destination, signed)), [])
        for destination in ("go", "rust"):
            for field, wrong in (("voucher_validated", False), ("voucher_relay", SOURCE),
                                 ("voucher_peer", SOURCE), ("voucher_expiration", 1), ("voucher_validation_basis", "assumed")):
                value = valid_service_case(destination, True)
                ready = value["raw"]["destination"]["ready"]
                receipt = ready if destination == "go" else ready["native_reservation_receipt"]
                receipt[field] = wrong
                with self.subTest(destination=destination, field=field):
                    self.assertTrue(validate_case(value))
        for field, wrong in (("framed_hex", "00"), ("voucher", True), ("status", 200),
                             ("expires_unix_ms", NOW+5000), ("addresses", []), ("protocol", IDENTIFY)):
            value = valid_service_case()
            value["raw"]["destination"]["ready"]["native_reservation_receipt"][field] = wrong
            with self.subTest(field=field):
                self.assertTrue(validate_case(value))
        value = valid_service_case()
        value["raw"]["destination"]["ready"].pop("native_reservation_receipt")
        self.assertTrue(validate_case(value))

    def test_rejects_mutated_proofs(self):
        mutations = {
            "manual reserve": lambda x: x["raw"]["destination"]["result"].update(operation_basis="async_reserve_relay"),
            "historical peer store": lambda x: x["raw"]["destination"]["result"].update(reservations_basis="snapshot.peers[].relay_reservations"),
            "unowned historical lease": lambda x: x["raw"]["destination"]["result"]["observations"][1]["autorelay"].update(reservations=0),
            "manual owned lease": lambda x: x["raw"]["destination"]["result"]["observations"][1]["autorelay"].update(automatic_reservations=0),
            "missing full lease": lambda x: x["raw"]["destination"]["result"]["observations"][1]["reservations"][0].pop("ttl_ms"),
            "wrong live endpoint": lambda x: x["raw"]["destination"]["result"]["observations"][1]["reservations"][0].update(endpoints=[address(SOURCE)]),
            "unjoined pending reserve": lambda x: x["raw"]["destination"]["result"]["observations"][-1]["autorelay"].update(pending_reservations=1),
            "manager worked after stop": lambda x: x["raw"]["destination"]["result"]["observations"][-1]["autorelay"].update(attempts=4),
            "expired advertised lease": lambda x: x["raw"]["destination"]["result"]["observations"][1]["reservations"][0].update(expires_unix_ms=NOW),
            "missing reservation": lambda x: x["raw"]["destination"]["result"]["observations"][1].update(reservations=[]),
            "unauthenticated candidate": lambda x: x["raw"]["destination"]["result"]["observations"][1]["sessions"][0].update(identified=False),
            "wrong exact hop": lambda x: x["raw"]["destination"]["result"]["observations"][1]["peers"][0].update(protocols=["/libp2p/circuit/relay/0.1.0"]),
            "forged renewal": lambda x: x["raw"]["destination"]["result"]["observations"][2]["reservations"][0].update(expires_unix_ms=NOW+9000),
            "only self query renewal": lambda x: x["raw"]["relay"]["result"]["events"].pop(),
            "native reserve denied": lambda x: x["raw"]["relay"]["result"]["events"][0].update(status=200),
            "bool native status": lambda x: x["raw"]["relay"]["result"]["events"][0].update(status=True),
            "partial native wire": lambda x: x["raw"]["relay"]["result"]["events"][0].update(capture_complete=False),
            "native wrong peer": lambda x: x["raw"]["relay"]["result"]["events"][0].update(peer_id=SOURCE),
            "direct echo": lambda x: x["echoes"][0]["result"]["echo_connection"].update(remote_addr=address(TARGET)),
            "wrong echo protocol": lambda x: x["echoes"][0]["result"].update(protocol="/forge/p2p/echo/1"),
            "zero echo bytes": lambda x: x["echoes"][0]["result"].update(echo_bytes=0),
            "bool bytes": lambda x: x["echoes"][0]["result"].update(echo_bytes=True),
            "echo before expiry only": lambda x: x["echoes"][1]["result"].update(unix_ms=NOW+8000),
            "self echo": lambda x: x["echoes"][0]["result"].update(local_peer_id=TARGET),
            "launch target mismatch": lambda x: x["echoes"][0]["process"]["command"].__setitem__(7, SOURCE),
            "unadvertised address": lambda x: x["raw"]["observer"]["result"]["events"][1].update(addresses=[]),
            "stale Identify withdrawal": lambda x: x["raw"]["observer"]["result"]["events"][2].update(addresses=[circuit_address()]),
            "no initial Identify": lambda x: x["raw"]["observer"]["result"]["events"].pop(0),
            "observer is destination": lambda x: x["raw"]["observer"]["ready"].update(peer_id=TARGET),
            "missing fault": lambda x: x["faults"].pop(),
            "reversed fault clock": lambda x: x["faults"][0].update(unix_ms=NOW),
            "no replacement acceptance": lambda x: x["raw"]["replacement"]["result"]["events"].pop(),
            "trace overflow": lambda x: x["raw"]["relay"]["result"].update(overflow=True),
            "not finalized": lambda x: x["raw"]["destination"]["result"].update(complete=False),
            "post stop work": lambda x: x["raw"]["destination"]["result"]["observations"][-1].update(discovery_attempts=4),
            "post stop lease": lambda x: x["raw"]["destination"]["result"]["observations"][-1].update(reservations=[{"relay_peer_id": RELAY}]),
            "shutdown not measured": lambda x: x["raw"]["destination"]["result"]["observations"][-1].update(elapsed_ms=15010),
            "SIGTERM": lambda x: x["raw"]["destination"]["process"]["terminal_status"].update(termination="terminated"),
            "bool exit code": lambda x: x["raw"]["destination"]["process"]["terminal_status"].update(exit_code=False),
            "unjoined donor tasks": lambda x: x["raw"]["replacement"]["result"]["fixture_task_lifecycle"].update(fixture_owned_tasks_joined=False),
            "manual Identify only": lambda x: x["raw"]["observer"]["result"].update(events=x["raw"]["observer"]["result"]["events"][:5]),
            "Push wrong wire ID": lambda x: x["raw"]["observer"]["result"]["events"][5].update(protocol=IDENTIFY),
            "Push self-query basis": lambda x: x["raw"]["observer"]["result"]["events"][5].update(basis="raw_identify_query"),
            "Push unpaired event": lambda x: x["raw"]["observer"]["result"]["events"][5].update(native_update_sequence=100),
            "Push missing withdrawal": lambda x: x["raw"]["observer"]["result"]["events"][6].update(addresses=[circuit_address()]),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                value = valid_case()
                mutate(value)
                self.assertTrue(validate_case(value))

    def test_rust_native_push_requires_exact_unique_completed_wire_frames(self):
        rows = []
        for index in range(4):
            rows.append({**connection(TARGET), "kind": "identify_push", "protocol": PUSH, "unix_ms": NOW+index,
                         "addresses": [], "basis": "native_Identify_Received_with_unique_inbound_push_wire_receipt",
                         "wire_receipts": [push_wire_receipt([], index+1)]})
        result = {"implementation": "rust", "events": rows}
        self.assertEqual(len(_native_pushes(result, TARGET, "quic")), 4)
        for key, wrong in (("complete_frames", False), ("invalid_or_over_limit", True), ("frames", 0), ("framed_sha256", "bad")):
            changed = copy.deepcopy(result)
            changed["events"][0]["wire_receipts"][0]["read"][key] = wrong
            with self.assertRaises(ValueError):
                _native_pushes(changed, TARGET, "quic")
        reused = copy.deepcopy(result)
        reused["events"][1]["wire_receipts"] = reused["events"][0]["wire_receipts"]
        with self.assertRaises(ValueError):
            _native_pushes(reused, TARGET, "quic")

    def test_push_wire_binds_native_bytes_key_addresses_hash_and_framing(self):
        for addresses in ([], [address(TARGET), circuit_address()],
                          [address(TARGET).replace("/udp/", "/tcp/").replace("/quic-v1", "")]):
            receipt = push_wire_receipt(addresses)
            validate_push_frame(receipt, TARGET, addresses)
            for change in (
                    lambda r: r.update(framed_hex="00"),
                    lambda r: r["read"].update(framed_sha256="a" * 64),
                    lambda r: r["read"].update(framed_bytes=True)):
                wrong = copy.deepcopy(receipt)
                change(wrong)
                with self.assertRaises(ValueError):
                    validate_push_frame(wrong, TARGET, addresses)
            with self.assertRaises(ValueError):
                validate_push_frame(receipt, RELAY, addresses)
            with self.assertRaises(ValueError):
                validate_push_frame(push_wire_receipt(addresses, key_byte=2), TARGET, addresses)
        receipt = push_wire_receipt([circuit_address()])
        with self.assertRaises(ValueError):
            validate_push_frame(receipt, TARGET, [])
        for raw in (bytes.fromhex(receipt["framed_hex"]) + b"\0", b"\x80\0", b"\x01\0", b"\x01\x0b"):
            wrong = copy.deepcopy(receipt)
            wrong.update(framed_hex=raw.hex())
            wrong["read"].update(framed_bytes=len(raw), framed_sha256=hashlib.sha256(raw).hexdigest())
            with self.assertRaises(ValueError):
                validate_push_frame(wrong, TARGET, [])

    def test_tcp_native_receipts_are_not_configuration_claims(self):
        receipt = {**connection(TARGET), "remote_addr": "/ip4/127.0.0.1/tcp/12345",
                   "negotiated_transport": "tcp", "negotiated_security": "/noise", "negotiated_muxer": "/yamux/1.0.0"}
        _connection(receipt, TARGET, "tcp")
        with self.assertRaises(ValueError):
            _connection(receipt, TARGET, "tcp-tls")
        receipt["negotiated_security"] = "/tls/1.0.0"
        _connection(receipt, TARGET, "tcp-tls")
        receipt.pop("negotiated_muxer")
        with self.assertRaises(ValueError):
            _connection(receipt, TARGET, "tcp-tls")

    def test_rust_exact_native_security_and_muxer(self):
        receipt = {"peer_id": TARGET, "connection_id": "swarm-1", "remote_addr": "/ip4/127.0.0.1/tcp/12345",
                   "upgrade_observation": {"overflow": False, "connections": [{"authenticated_remote_peer_id": TARGET,
                       "security_complete": True, "muxer_complete": True, "selected_security": "/noise", "selected_muxer": "/yamux/1.0.0"}]}}
        _connection(receipt, TARGET, "tcp")
        for key in ("security_complete", "muxer_complete", "selected_security", "selected_muxer"):
            bad = copy.deepcopy(receipt)
            bad["upgrade_observation"]["connections"][0][key] = None
            with self.assertRaises(ValueError):
                _connection(bad, TARGET, "tcp")
        receipt["upgrade_observation"]["connections"].append(copy.deepcopy(receipt["upgrade_observation"]["connections"][0]))
        with self.assertRaises(ValueError):
            _connection(receipt, TARGET, "tcp")

    def test_circuit_rejects_wrong_target_relay_transport_and_noncanonical_input(self):
        for value, relay, target, transport in (
                (circuit_address(), SOURCE, TARGET, "quic"), (circuit_address(), RELAY, SOURCE, "quic"),
                (circuit_address(), RELAY, TARGET, "tcp"), (circuit_address()+"/p2p/extra", RELAY, TARGET, "quic"),
                (circuit_address().replace("12345", "0"), RELAY, TARGET, "quic")):
            with self.assertRaises(ValueError):
                circuit(value, relay, target, transport)


if __name__ == "__main__":
    unittest.main()
