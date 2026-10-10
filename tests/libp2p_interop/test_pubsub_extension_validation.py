"""Negative and pure-property tests; these are not live evidence."""

from copy import deepcopy
from dataclasses import asdict, replace
import hashlib
import unittest
from unittest.mock import patch

import pubsub_extension_validation as validation
from pubsub_evidence import SOURCES

from partial_fixture import encode_part, group_id, part_bytes
from pubsub_extension_cases import Actors, Case
from pubsub_extension_evidence import _rpc
from pubsub_extension_validation import (
    _before_release, _delivered, _extension_closed, _held_chain, _partial_permissions, _reconstruction, _topic_flags, validate_capture,
)
from test_pubsub_extension_evidence import TOKEN, TOPIC, rpc
from test_pubsub_evidence import peer_id
from test_pubsub_wire import field, receipt
from rust_upgrade_evidence import _peer


def original_lifecycle():
    """Unit-only lifecycle model, intentionally no router/wire success proof."""
    spec = Case("rust", "forge", "partial", "native_quic")
    artifact = {"schema_version": 1, "suite": "pubsub-extensions", "case": asdict(spec),
                "scenario_id": spec.identifier, "acceptance_scenario_id": spec.scenario,
                "runner_scenario_id": spec.runner_id, "case_token": TOKEN, "raw": {}, "processes": {},
                "shutdown_barrier": {"source": "python.fixture.all_actor_prepare_barrier", "operations": []}}
    snapshots = {}
    for index, role in enumerate(("victim", "offender", "replacement", "sink"), 1):
        implementation = spec.implementation(role)
        raw = {"schema_version": 1, "implementation": implementation, "actor": role, "case_token": TOKEN,
               "local_peer_id": "unit-" + role, "version": spec.version, "extension": spec.mode(role),
               "requests_partial": spec.mode(role) == "partial", "finalized": False, "joined": False,
               "overflow": False, "error": None, "events": []}
        def event(kind, **fields):
            sequence = len(raw["events"]) + 1
            result = {"sequence": sequence, "mono_ns": sequence, "kind": kind,
                      "source": sorted(SOURCES[implementation][kind])[0], **fields}
            raw["events"].append(result)
            return result
        if implementation == "rust":
            raw["extension_state"] = {"admission_closed": True, "application_stopped": False, "error": None,
                                      "validation_hold_pending": False, "pending_hooks": 0}
            stack = {"transport": "quic", "security": "/tls/1.0.0", "muxer": "quic",
                     "authentication_basis": "native_QUIC_authenticated_transport_output"}
            owner = {"connection_trace_id": 1, "stream_trace_id": None, "remote_peer_id": "unit-victim",
                     "swarm_connection_id": "1", "peer_id": "unit-victim", "connection_id": "1", "stream_id": None,
                     "endpoint": {"kind": "Dialer", "address": "/ip4/127.0.0.1/udp/4000/quic-v1"},
                     "native_stack": stack}
            event("connection", **owner, authenticated=True, transport="quic")
        else:
            raw.update(extension_admission_closed=True, active_extension_validators=0, active_extension_work=0,
                       extension_drained=True, partial_registration_active=False, extension_drain_error=None,
                       extension_capture_error=None)
        ack = event("shutdown_prepared", actor=role, case_token=TOKEN, local_peer_id=raw["local_peer_id"],
                    command_sequence=1, admission_closed=True, pending_commands=0)
        event("command_done", command_sequence=1, command_kind="prepare_shutdown", status="ok")
        row = {"sequence": index, "kind": "prepare_ack", "actor": role, "case_token": TOKEN,
               "local_peer_id": raw["local_peer_id"], "command_sequence": 1,
               "ack_event_sequence": ack["sequence"], "evidence_file": "/unit/" + role + ".prepare.json"}
        artifact["shutdown_barrier"]["operations"].append(row)
        snapshots[role] = {"schema_version": 1, "source": "python.fixture.native_prepare_snapshot", "actor": role,
                           "case_token": TOKEN, "pid": 100 + index, "command_sequence": 1,
                           "ack_event_sequence": ack["sequence"], "result": deepcopy(raw)}
        raw.update(finalized=True, joined=True)
        artifact["processes"][role] = {"pid": 100 + index, "returncode": 0, "forced_termination": False,
                                       "log_file": "/unit/" + role + ".log"}
        if implementation == "rust":
            raw["extension_state"]["application_stopped"] = True
            event("native_io_error", **owner, operation="muxer_inbound", prepared=True, io_kind="Other",
                  raw_os_error=None, typed_cause="quic_connection_cause_unavailable", message="unit opaque cause")
            event("shutdown_requested", listeners=1)
            raw.update(status="error", error="unit opaque cause",
                       native_close={"live_muxers": 0, "live_streams": 0, "connections": [{
                           "connection_trace_id": 1, "connection_id": "1", "peer_id": "unit-victim",
                           "dropped": True, "close_returned": False, "native_terminal_observed": True}]},
                       task_join={"fixture_owned_tasks_joined": True, "overflow": False, "errors": []})
            artifact["processes"][role]["returncode"] = 1
        artifact["raw"][role] = raw
    process = artifact["processes"]["replacement"]
    diagnostic = f"pid={process['pid']}; log={process['log_file']}: terminal exit code 1"
    artifact.update(status="HARNESS_ERROR", cleanup_errors=[diagnostic, diagnostic],
                    errors=[f"RuntimeError: replacement donor shutdown did not actually close/join successfully: {[diagnostic]}"])
    artifact["shutdown_barrier"]["operations"].append({"sequence": 5, "kind": "stop_requested", "actor": "replacement",
        "case_token": TOKEN, "local_peer_id": "unit-replacement"})
    return artifact, snapshots


class OriginalExtensionScopeTests(unittest.TestCase):
    def check(self, artifact, snapshots):
        # Real prefix/ACK/terminal/error validation; no invented native traffic.
        with patch.object(validation, "_validate_traffic", return_value={"unit_lifecycle_only": True}) as traffic:
            result = validation.validate_active_capture(artifact, snapshots)
        self.assertEqual(result, {"unit_lifecycle_only": True})
        for role in snapshots:
            self.assertEqual(traffic.call_args.args[2][role], snapshots[role]["result"])

    def test_post_ack_opaque_diagnostic_keeps_original_error_and_actual_join(self):
        artifact, snapshots = original_lifecycle()
        before = deepcopy(artifact)
        self.check(artifact, snapshots)
        self.assertEqual(artifact, before)
        # The exact same lifecycle has no wire proof and cannot be promoted.
        with self.assertRaises(ValueError):
            validation.validate_active_capture(artifact, snapshots)
        with self.assertRaises(ValueError):
            validation.validate_capture(artifact)

    def test_only_exact_post_ack_muxer_cause_on_same_owned_carrier_is_diagnostic(self):
        for field, value in (("prepared", False), ("operation", "stream_read"), ("io_kind", "BrokenPipe"),
                             ("typed_cause", "quinn_application_closed_0"), ("typed_cause", "unclassified"),
                             ("raw_os_error", 32), ("connection_trace_id", 2), ("peer_id", "foreign"),
                             ("remote_peer_id", "foreign"), ("swarm_connection_id", "foreign"),
                             ("stream_id", "1:1"), ("endpoint", {}), ("native_stack", {}),
                             ("source", "rust.fixture.control_file"), ("message", "another first error")):
            artifact, snapshots = original_lifecycle()
            artifact["raw"]["replacement"]["events"][3][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.check(artifact, snapshots)

    def test_prepare_and_terminal_extension_states_are_distinct(self):
        artifact, snapshots = original_lifecycle()
        self.assertIs(snapshots["replacement"]["result"]["extension_state"]["application_stopped"], False)
        self.assertIs(artifact["raw"]["replacement"]["extension_state"]["application_stopped"], True)
        self.check(artifact, snapshots)
        for active in (False, True):
            for field, value in (("application_stopped", active), ("application_stopped", None),
                                 ("application_stopped", int(not active)), ("admission_closed", False),
                                 ("pending_hooks", 1), ("pending_hooks", True),
                                 ("validation_hold_pending", True), ("error", "failure")):
                artifact, snapshots = original_lifecycle()
                raw = snapshots["replacement"]["result"] if active else artifact["raw"]["replacement"]
                raw["extension_state"][field] = value
                with self.subTest(active=active, field=field, value=value), self.assertRaises(ValueError):
                    self.check(artifact, snapshots)
            for field in ("application_stopped", "error"):
                artifact, snapshots = original_lifecycle()
                raw = snapshots["replacement"]["result"] if active else artifact["raw"]["replacement"]
                del raw["extension_state"][field]
                with self.subTest(active=active, missing=field), self.assertRaises(ValueError):
                    self.check(artifact, snapshots)

    def test_no_pre_ack_failure_prefix_substitution_or_missing_prepare(self):
        for mutation in (
            lambda a, s: s["replacement"]["result"].update(error="pre-ACK failure"),
            lambda a, s: s["replacement"]["result"]["events"][0].update(peer_id="foreign"),
            lambda a, s: s["replacement"].update(pid=999),
            lambda a, s: a["shutdown_barrier"]["operations"].pop(0),
            lambda a, s: a["raw"]["replacement"]["events"][2].update(status="error"),
            lambda a, s: a["raw"]["replacement"].update(error="unrelated earlier failure"),
        ):
            artifact, snapshots = original_lifecycle()
            mutation(artifact, snapshots)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(artifact, snapshots)

    def test_no_other_actor_failure_forced_exit_missing_drop_or_extension_drain(self):
        for role, field, value in (("victim", "returncode", 1), ("offender", "returncode", 1),
                                   ("sink", "returncode", 1), ("replacement", "returncode", 2),
                                   ("replacement", "forced_termination", True)):
            artifact, snapshots = original_lifecycle()
            artifact["processes"][role][field] = value
            with self.subTest(role=role, field=field), self.assertRaises(ValueError):
                self.check(artifact, snapshots)
        for mutation in (
            lambda r: r.update(joined=False),
            lambda r: r.update(overflow=True),
            lambda r: r["task_join"].update(fixture_owned_tasks_joined=False),
            lambda r: r["native_close"].update(live_streams=1),
            lambda r: r["native_close"]["connections"][0].update(peer_id="foreign"),
            lambda r: r["native_close"]["connections"][0].update(native_terminal_observed=False),
            lambda r: r["extension_state"].update(pending_hooks=1),
        ):
            artifact, snapshots = original_lifecycle()
            mutation(artifact["raw"]["replacement"])
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(artifact, snapshots)

    def test_parser_fault_and_unrelated_cleanup_error_remain_fatal(self):
        for kind in ("incomplete_rpc_frame", "native_multistream_frame", "rpc", "command_done"):
            artifact, snapshots = original_lifecycle()
            events = artifact["raw"]["replacement"]["events"]
            events.append({"sequence": len(events) + 1, "mono_ns": len(events) + 1, "kind": kind,
                           "source": sorted(SOURCES["rust"].get(kind, {"rust.libp2p.passive-upgraded-stream-io"}))[0]})
            with self.subTest(kind=kind), self.assertRaises((ValueError, KeyError)):
                self.check(artifact, snapshots)
        for mutation in (
            lambda a: a["errors"].append("another failure"),
            lambda a: a["cleanup_errors"].append("forced termination"),
            lambda a: a.update(cleanup_errors=[]),
            lambda a: a["shutdown_barrier"]["operations"][-1].update(actor="victim"),
        ):
            artifact, snapshots = original_lifecycle()
            mutation(artifact)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(artifact, snapshots)


class ExtensionValidationTests(unittest.TestCase):
    def test_shutdown_does_not_infer_completion_from_mode(self):
        for implementation in ("forge", "go", "rust"):
            with self.assertRaises(ValueError):
                _extension_closed({"implementation": implementation, "extension": "partial"})
        go = {"implementation": "go", "extension_admission_closed": True, "active_extension_validators": 0,
              "pending_extension_work": 0, "active_extension_publish_callbacks": 0}
        _extension_closed(go)
        forge = {"implementation": "forge", "extension_admission_closed": True, "active_extension_validators": 0,
                 "active_extension_work": 0, "extension_drained": True, "partial_registration_active": False,
                 "extension_drain_error": None}
        _extension_closed(forge)
        for key, value in (("active_extension_work", True), ("active_extension_work", 1),
                           ("extension_drained", False), ("partial_registration_active", True),
                           ("extension_drain_error", "failure")):
            with self.assertRaises(ValueError):
                _extension_closed({**forge, key: value})
        for key in forge.keys() - {"implementation"}:
            with self.assertRaises(ValueError):
                _extension_closed({name: value for name, value in forge.items() if name != key})
        for key in ("active_extension_validators", "pending_extension_work", "active_extension_publish_callbacks"):
            for value in (True, 1, None):
                with self.assertRaises(ValueError):
                    _extension_closed({**go, key: value})
        rust = {"implementation": "rust", "extension_state": {"admission_closed": True, "application_stopped": True,
                "error": None, "validation_hold_pending": False, "pending_hooks": 0}}
        _extension_closed(rust)
        for key, value in (("admission_closed", False), ("error", "failure"), ("pending_hooks", 1),
                           ("application_stopped", False), ("validation_hold_pending", True)):
            changed = deepcopy(rust)
            changed["extension_state"][key] = value
            with self.assertRaises(ValueError):
                _extension_closed(changed)

    def test_reconstruction_binds_each_actual_incoming_event(self):
        payload = b"".join(part_bytes(TOKEN, i) for i in range(3))
        events, parts = [], []
        for index in range(3):
            body = encode_part(index, part_bytes(TOKEN, index)).hex()
            parts.append({"index": index, "read_sequence": 2 * index + 1, "encoded_hex": body})
            events.extend(({"kind": "rpc", "sequence": 2 * index + 1},
                           {"kind": "partial_incoming", "sequence": 2 * index + 2, "peer_id": "provider",
                            "source": "rust.libp2p.gossipsub.Event.Partial", "topic": TOPIC,
                            "group_id_hex": group_id(TOKEN).hex(), "group_bytes": len(group_id(TOKEN)),
                            "body_present": True, "body_hex": body, "body_bytes": len(bytes.fromhex(body))}))
        events.append({"kind": "partial_reconstructed", "sequence": 7, "group_id_hex": group_id(TOKEN).hex(),
                       "part_incoming_sequences": [2, 4, 6], "part_peer_ids": ["provider"] * 3,
                       "parts_hex": [part["encoded_hex"] for part in parts], "payload_hex": payload.hex(),
                       "payload_sha256": hashlib.sha256(payload).hexdigest(), "payload_bytes": len(payload)})
        exchange = {"group_id_hex": group_id(TOKEN).hex(), "payload_hex": payload.hex(), "parts": parts}
        self.assertEqual(_reconstruction(events, exchange, "provider", TOPIC), 7)
        for references in ([2, 4, 4], [1, 4, 6], [None, 4, 6], [[2], 4, 6], [2, 4, 100]):
            changed = deepcopy(events)
            changed[-1]["part_incoming_sequences"] = references
            with self.assertRaises(ValueError):
                _reconstruction(changed, exchange, "provider", TOPIC)
        for key, value in (("peer_id", "foreign"), ("body_present", False), ("body_hex", ""), ("kind", "partial_offer"),
                           ("topic", "foreign"), ("group_bytes", 21), ("body_bytes", 1024)):
            changed = deepcopy(events)
            changed[1][key] = value
            with self.assertRaises(ValueError):
                _reconstruction(changed, exchange, "provider", TOPIC)
        for key, value in (("part_peer_ids", ["provider", "other", "provider"]),
                           ("parts_hex", [parts[1]["encoded_hex"], parts[0]["encoded_hex"], parts[2]["encoded_hex"]])):
            changed = deepcopy(events)
            changed[-1][key] = value
            with self.assertRaises(ValueError):
                _reconstruction(changed, exchange, "provider", TOPIC)

    def test_delivery_cannot_be_replaced_by_validation(self):
        identity = {key: key for key in ("propagation_peer", "author_peer", "topic", "message_id", "seqno_hex", "payload_sha256")}
        validation = {**identity, "kind": "validation", "sequence": 5}
        delivery = {**identity, "kind": "delivery", "sequence": 6, "validation_commit": True,
                    "report_message_validation_result": True}
        for implementation in ("forge", "rust"):
            self.assertIs(_delivered([validation, delivery], validation, implementation), delivery)
            for rows in ([validation], [validation, {**delivery, "seqno_hex": "foreign"}],
                         [validation, {**delivery, "sequence": 4}]):
                with self.assertRaises(ValueError):
                    _delivered(rows, validation, implementation)
        go = {**delivery, "committed": True, "phase": "post_decision"}
        self.assertIs(_delivered([go], go, "go"), go)
        with self.assertRaises(ValueError):
            _delivered([delivery], delivery, "go")

    def test_held_identity_and_exact_release_chain(self):
        identity = {key: key for key in ("propagation_peer", "author_peer", "topic", "message_id", "seqno_hex", "payload_sha256")}
        forward = {**identity, "kind": "validation", "sequence": 6}
        events = [{"kind": "validation_hold_armed", "sequence": 1, "command_sequence": 3,
                   "payload_sha256": identity["payload_sha256"]},
                  {"kind": "rpc", "sequence": 2},
                  {**identity, "kind": "validation_held", "sequence": 3, "command_sequence": 3, "committed": False},
                  {"kind": "validation_release", "sequence": 4, "command_sequence": 4, "held_observation_sequence": 3},
                  {**identity, "kind": "validation_resumed", "sequence": 5, "held_observation_sequence": 3,
                   "released": True, "committed": False}, forward]
        for implementation in ("forge", "go"):
            _held_chain(events, forward, 2, implementation)
            for row, key, value in ((2, "seqno_hex", "other"), (2, "author_peer", "other"),
                                    (2, "propagation_peer", "other"), (2, "topic", "other"),
                                    (2, "message_id", "other"), (2, "command_sequence", 2),
                                    (3, "held_observation_sequence", 2), (4, "held_observation_sequence", 2),
                                    (4, "released", False), (4, "sequence", 7)):
                changed = deepcopy(events)
                changed[row][key] = value
                with self.assertRaises(ValueError):
                    _held_chain(changed, forward, 2, implementation)
        rust = [*events[:3], {**identity, "kind": "validation_release_requested", "sequence": 4,
                "hold_command_sequence": 3, "command_sequence": 4, "validation_commit": False,
                "cancellation": None}, forward, {**identity, "kind": "validation_released", "sequence": 8,
                "hold_command_sequence": 3, "command_sequence": 4, "validation_commit": True,
                "cancellation": None, "error": None}]
        _held_chain(rust, forward, 2, "rust")
        for implementation, rows in (("forge", events), ("go", events), ("rust", rust)):
            armed, _, release = _held_chain(rows, forward, 2, implementation)
            self.assertTrue(_before_release(3, armed, release))
            # Still before commit (6), but after the actual release request (4).
            self.assertFalse(_before_release(5, armed, release))
            self.assertFalse(_before_release(4, armed, release))
        with self.assertRaises(ValueError):
            _held_chain([*rust[:3], *rust[4:]], forward, 2, "rust")
        for key, value in (("seqno_hex", "other"), ("hold_command_sequence", 2), ("validation_commit", False),
                           ("error", "native failure"), ("cancellation", "timeout"), ("sequence", 5)):
            changed = deepcopy(rust)
            changed[-1][key] = value
            with self.assertRaises(ValueError):
                _held_chain(changed, forward, 2, "rust")

    def test_partial_permissions_must_precede_write_and_survive_no_revocation(self):
        extension = field(3, field(6, field(10, 1)))
        subscription = field(1, field(1, 1) + field(2, TOPIC.encode()) + field(3, 1))
        unsubscribe = field(1, field(1, 0) + field(2, TOPIC.encode()))
        metadata_only = field(1, field(1, 1) + field(2, TOPIC.encode()) + field(4, 1))
        body = field(10, field(1, TOPIC.encode()) + field(2, group_id(TOKEN)) + field(3, b""))
        metadata = field(10, field(1, TOPIC.encode()) + field(2, group_id(TOKEN)) + field(4, b""))
        outgoing = rpc(2, "write", "peer", extension)
        good = [rpc(1, "read", "peer", extension + subscription), outgoing, rpc(4, "write", "peer", body)]
        _partial_permissions(good)
        _partial_permissions([rpc(1, "read", "peer", extension + metadata_only), outgoing,
                              rpc(4, "write", "peer", metadata)])
        invalid = [
            [outgoing, good[-1], rpc(5, "read", "peer", extension + subscription)],
            [rpc(1, "read", "peer", extension), outgoing, good[-1], rpc(5, "read", "peer", subscription)],
            [*good[:2], rpc(3, "read", "peer", unsubscribe), good[-1]],
            [*good[:2], rpc(3, "read", "peer", metadata_only), good[-1]],
            [*good[:2], rpc(3, "read", "peer", extension, "new-generation"), good[-1]],
            [*good[:2], rpc(3, "read", "peer", subscription, "new-generation"), good[-1]],
            [*good[:2], rpc(4, "write", "peer", body, "new-generation")],
        ]
        for rows in invalid:
            with self.assertRaises(ValueError):
                _partial_permissions(rows)

    def test_global_support_does_not_substitute_for_topic_flags(self):
        spec = Case("forge", "go", "partial", "native_tcp_yamux")
        peers = {"victim": "a", "offender": "b"}
        graph = {"victim": {"b"}, "offender": {"a"}}
        def subscribe(partial):
            return field(1, field(1, 1) + field(2, TOPIC.encode()) + field(3, int(partial)))
        full, partial = subscribe(False), subscribe(True)
        views = {"victim": [rpc(1, "read", "b", full), rpc(2, "write", "b", partial)],
                 "offender": [rpc(1, "read", "a", partial), rpc(2, "write", "a", full)]}
        _topic_flags(spec, views, peers, graph, TOPIC)
        changed = {**views, "offender": [rpc(1, "read", "a", full), views["offender"][1]]}
        with self.assertRaises(ValueError):
            _topic_flags(spec, changed, peers, graph, TOPIC)
        changed = {**views, "victim": views["victim"][:1]}
        with self.assertRaises(ValueError):
            _topic_flags(spec, changed, peers, graph, TOPIC)

    def test_capture_without_proofs_is_not_accepted(self):
        for document in ({}, {"schema_version": 1, "suite": "pubsub-scoring"},
                         {"schema_version": 1, "suite": "pubsub-extensions", "status": "captured"}):
            with self.assertRaises(ValueError):
                validate_capture(document)


class IdontwantBarrierTests(unittest.TestCase):
    """Synthetic causal/wire models; authentication and signature verification are not live proof."""

    def model(self, victim="forge", replacement="go"):
        roles = ("victim", "offender", "replacement", "sink")
        peers = {role: peer_id(index) for index, role in enumerate(roles, 1)}
        actors = {role: {"local_peer_id": peers[role], "case_token": TOKEN,
                         "implementation": {"victim": victim, "replacement": replacement}.get(role, "forge")}
                  for role in roles}
        events, views = {role: [] for role in roles}, {role: [] for role in roles}
        graph = {"victim": ("offender", "replacement", "sink"), "offender": ("victim", "replacement"),
                 "replacement": ("victim", "offender"), "sink": ("victim",)}
        payload = "accept:" + TOKEN + ":idontwant:" + "x" * 1200
        seqno = bytes.fromhex("0000000000000001")
        identity = {"propagation_peer": peers["offender"], "author_peer": peers["offender"], "topic": TOPIC,
                    "message_id": (_peer(peers["offender"]) + seqno).hex(), "seqno_hex": seqno.hex(),
                    "payload_sha256": hashlib.sha256(payload.encode()).hexdigest()}
        body = field(2, field(1, _peer(peers["offender"])) + field(2, payload.encode())
                     + field(3, seqno) + field(4, TOPIC.encode()) + field(5, b"unit-only-signature"))

        def emit(role, kind, **fields):
            event = {"kind": kind, "sequence": len(events[role]) + 1,
                     "source": actors[role]["implementation"] + ".unit", **fields}
            events[role].append(event)
            return event

        def wire(source, target, value):
            for role, remote, direction in ((source, target, "write"), (target, source, "read")):
                encoded = receipt(value, direction)
                event = emit(role, "rpc", receipt=encoded)
                views[role].append(_rpc(event["sequence"], encoded, "/meshsub/1.2.0", direction, peers[remote],
                                        ("unit-connection", "unit-stream", direction)))

        def commit(role, source):
            fields = {**identity, "propagation_peer": peers[source], "outcome": "accept"}
            if actors[role]["implementation"] == "go":
                emit(role, "delivery", **fields, committed=True, phase="post_decision")
            else:
                emit(role, "validation", **fields)
                emit(role, "delivery", **fields, validation_commit=True, report_message_validation_result=True)

        for role in roles:
            emit(role, "snapshot", label="extension_before", mesh_peer_ids=[peers[other] for other in graph[role]],
                 peer_scores=[])
        for role in ("victim", "replacement"):
            emit(role, "validation_hold_armed", command_sequence=1, payload_sha256=identity["payload_sha256"])
            wire("offender", role, body)
            emit(role, "validation_held", **identity, command_sequence=1, committed=False)
        control = field(3, field(5, field(1, bytes.fromhex(identity["message_id"]))))
        wire("replacement", "victim", control)
        wire("victim", "replacement", control)
        for role in ("replacement", "victim"):
            held = validation._single(events[role], "validation_held")
            if actors[role]["implementation"] == "rust":
                emit(role, "validation_release_requested", **identity, hold_command_sequence=1, command_sequence=2,
                     validation_commit=False, cancellation=None)
                commit(role, "offender")
                emit(role, "validation_released", **identity, hold_command_sequence=1, command_sequence=2,
                     validation_commit=True, cancellation=None, error=None)
            else:
                emit(role, "validation_release", command_sequence=2, held_observation_sequence=held["sequence"])
                emit(role, "validation_resumed", **identity, held_observation_sequence=held["sequence"],
                     released=True, committed=False)
                commit(role, "offender")
        wire("victim", "sink", body)
        commit("sink", "victim")
        return actors, events, views

    def check(self, actors, events, views):
        # Substitute only the native-owner boundary. The real frame decoder,
        # message pairing, identity, hold, release and delivery checks run below.
        def rpcs(rows, peer, protocol, transport, direction, fingerprint):
            role = next(role for role in events if rows is events[role])
            return [(rows[rpc.sequence - 1], rpc.value) for rpc in views[role]
                    if rpc.peer == peer and rpc.stream[-1] == direction]
        with patch("pubsub_evidence._rpcs", side_effect=rpcs):
            return validation._idontwant(actors, events, views, TOKEN, "/meshsub/1.2.0", "tcp", None)

    def test_dual_holds_pair_real_control_bytes_for_all_actor_implementations(self):
        for victim, replacement in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")):
            with self.subTest(victim=victim, replacement=replacement):
                result = self.check(*self.model(victim, replacement))
                self.assertGreater(result["idontwant_read"], result["held_sequence"])
                self.assertGreater(result["reverse_idontwant_read"], result["replacement_held_sequence"])
                self.assertEqual(result["informed"]["message_id"], result["sink"]["message_id"])

    def test_both_holds_require_original_sender_and_successful_release(self):
        for role in ("victim", "replacement"):
            for kind, field_name, value in (
                ("validation_hold_armed", "kind", "unit_missing_hold"),
                ("validation_held", "propagation_peer", "foreign"),
                ("validation_held", "author_peer", "foreign"),
                ("validation_held", "message_id", "00"),
                ("validation_resumed", "released", False),
                ("validation_resumed", "error", "validation hold expired"),
                ("delivery", "propagation_peer", "foreign"),
                ("delivery", "committed", False),
            ):
                actors, events, views = self.model("go", "go")
                validation._single(events[role], kind)[field_name] = value
                with self.subTest(role=role, kind=kind, field=field_name), self.assertRaises(ValueError):
                    self.check(actors, events, views)

    def test_each_direction_requires_matching_control_before_both_releases(self):
        for role in ("victim", "replacement"):
            for direction in ("read", "write"):
                for mutation in ("missing", "late", "before_input", "foreign_peer", "wrong_id", "different_frame"):
                    actors, events, views = self.model()
                    index = next(index for index, rpc in enumerate(views[role])
                                 if rpc.stream[-1] == direction and rpc.value["idontwant"])
                    old = views[role][index]
                    if mutation == "missing":
                        views[role].pop(index)
                    elif mutation in {"late", "before_input", "foreign_peer"}:
                        fields = {"peer": peer_id(9)} if mutation == "foreign_peer" else {"sequence": (
                            validation._single(events[role], "validation_release")["sequence"] if mutation == "late" else 2)}
                        views[role][index] = replace(old, **fields)
                    else:
                        identity = validation._single(events[role], "validation_held")["message_id"]
                        ids = field(1, b"foreign") if mutation == "wrong_id" else (
                            field(1, bytes.fromhex(identity)) + field(1, b"extra"))
                        views[role][index] = _rpc(old.sequence, receipt(field(3, field(5, ids)), direction),
                                                  "/meshsub/1.2.0", direction, old.peer, old.stream)
                    with self.subTest(role=role, direction=direction, mutation=mutation), self.assertRaises(ValueError):
                        self.check(actors, events, views)

    def test_full_message_on_either_cross_edge_cannot_masquerade_as_suppression(self):
        for role, remote in (("victim", "replacement"), ("replacement", "victim")):
            for direction in ("read", "write"):
                actors, events, views = self.model()
                message = next(rpc for rpc in views[role] if rpc.value["messages"])
                views[role].append(replace(message, peer=actors[remote]["local_peer_id"],
                                           stream=(*message.stream[:-1], direction)))
                with self.subTest(role=role, direction=direction), self.assertRaises(ValueError):
                    self.check(actors, events, views)

    def test_runner_arms_both_before_publish_and_waits_for_committed_replacement(self):
        for implementation in ("go", "rust"):
            actors, events, views = self.model("forge", implementation)
            case = Actors(Case(implementation, "forge", "idontwant", "native_tcp_yamux"), {}, "/not-created", None, None, None)
            case.token, case.actors = TOKEN, dict.fromkeys(actors)
            steps = []

            def control(role, kind, **fields):
                steps.append(("control", role, kind))

            def wait(role, predicate):
                raw = {**actors[role], "events": events[role]}
                if steps[-1:] == [("control", "replacement", "validation_release")]:
                    uncommitted = deepcopy(raw)
                    delivery = validation._single(uncommitted["events"], "delivery")
                    delivery.update(committed=False, phase="decision", validation_commit=False,
                                    report_message_validation_result=False)
                    self.assertFalse(predicate(uncommitted))
                self.assertTrue(predicate(raw))
                steps.append(("wait", role, len(events[role])))
                return raw

            def native(rows, *args):
                return views[next(role for role in events if rows is events[role])]

            with patch.object(case, "connect"), patch.object(case, "peer", side_effect=lambda role: actors[role]["local_peer_id"]), \
                    patch.object(case, "control", side_effect=control), patch.object(case, "wait", side_effect=wait), \
                    patch("pubsub_extension_cases.native_rpcs", side_effect=native):
                case.idontwant()
            publish = steps.index(("control", "offender", "publish_extension"))
            release = steps.index(("control", "replacement", "validation_release"))
            self.assertEqual(steps[publish - 2:publish], [("control", "victim", "validation_hold"),
                                                        ("control", "replacement", "validation_hold")])
            self.assertEqual([step[1] for step in steps[publish + 1:release]],
                             ["victim", "replacement", "replacement", "victim", "victim", "replacement"])
            self.assertEqual(steps[release + 1][0:2], ("wait", "replacement"))
            self.assertEqual(steps[release + 2], ("control", "victim", "validation_release"))
            self.assertEqual(steps[-1][0:2], ("wait", "sink"))


if __name__ == "__main__":
    unittest.main()
