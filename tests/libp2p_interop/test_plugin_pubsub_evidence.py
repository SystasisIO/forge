"""Synthetic verifier regressions, not live plugin/donor acceptance evidence."""

from copy import deepcopy
import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from plugin_pubsub_cases import (
    ACTORS, CASE_SECONDS, Case, _append_control, _wait, case_specs, command, probes, run_case,
)
from plugin_pubsub_evidence import (
    FORGE_SOURCES, _forge_connection, delivery_ready, donor_marker_written, forge_delivery,
    validate_case, validate_delivery, validate_terminal_sources,
)
from pubsub_evidence import AUTHENTICATION, _same_json
from rust_upgrade_evidence import _peer
from test_pubsub_evidence import peer_id
from test_pubsub_wire import field, receipt

BINARY_PATHS = {implementation: Path("/synthetic/" + implementation) for implementation in ("forge", "go", "rust")}


def synthetic_record(spec, root):
    """Minimal actual-schema-shaped input; all wire signatures here are synthetic."""
    root = root.resolve()
    work = root / "case"
    work.mkdir()
    token, topic = "a" * 32, "forge-pr11:" + "a" * 32
    peers = {"victim": peer_id(1), "replacement": peer_id(2)}
    binary, oversized, marker = probes(spec, token)
    pids = {"victim": 101, "replacement": 102}
    pubsub = {"enabled": True, "preferred-version": "v1-1", "signature-policy": "strict-sign",
              "sign-publishes": True, "max-message-size": 128, "flood-publish": True, "allowed-topics": [topic]}
    node = {"allow-insecure-test-mode": False, "peer-store": {"store": "peers"},
        "identity": {"certificate-secret": "identity/certificate", "private-key-secret": "identity/private-key"},
        "path": {"policy": "direct-only"}, "topology": {"mode": "static-only"},
        "relay": {"client-enabled": False, "server-enabled": False, "auto-discovery-enabled": False},
        "listen": ["/ip4/127.0.0.1/tcp/0"], "tcp": {"security": "noise"}, "pubsub": pubsub}
    document = {"plugins": {"db": {"store": {"stores": [{"name": "peers", "driver": spec.backend,
        "path": str(work / "victim.store"), "object": {"family": "objectdb", "write-policy": "single-writer"}}]}},
        "net": {"p2p": {"node": node}}}}
    settings = {"source": "official-shell.accepted-document", "secret_material": "redacted", "backend_fallback": False,
                "identity_source": "Secrets.get_bytes", "identity_key_type": "ed25519", "psk_source": "absent", "document": document}
    if spec.policy == "typed-override":
        competing = deepcopy(document)
        competing["plugins"]["net"]["p2p"]["node"]["pubsub"].update({"preferred-version": "v1-0", "flood-publish": False})
        settings.update(node_reconfigure={"source": "official-node-plugin.configure", "component": "plugins.net.p2p.node",
                                         "document": competing}, typed_contribution={
            "source": "node.configuration_api.set_pubsub_options", "full_native_snapshot": True,
            "preferred": "v1-2", "max_data_size": 96, "signatures": "strict-sign", "flood_publish": True,
            "tracer_installed": True, "application_score_callback_installed": True,
            "application_sample": 3.0, "application_weight": 2.0})
    forge = {"schema_version": 1, "suite": "plugin-node", "implementation": "forge", "fixture_kind": "configured-plugin",
        "scenario": "pubsub_exchange", "role": "listener", "actor": "victim", "case_token": token, "topic": topic,
        "local_peer_id": peers["victim"], "version": spec.version, "pubsub_policy": spec.policy,
        "store_backend": spec.backend, "configuration_accepted": True, "accepted_configuration": settings,
        "psk_fingerprint": "", "status": "ok", "errors": [], "error": None, "overflow": False, "capture_error": False,
        "final": False, "finalized": False, "joined": False, "command_admission_closed": False, "subscription_drained": False,
        "commands_completed": 0, "events": [], "native_tracer_installed": spec.policy == "typed-override",
        "declarative_source": "forge.codec.yaml.read_document", "lifecycle": {"plugins_joined": False, "scheduler_joined": False}}
    donor = {"schema_version": 1, "implementation": spec.donor, "actor": "replacement", "case_token": token,
             "local_peer_id": peers["replacement"], "finalized": False, "joined": False, "overflow": False,
             "error": None, "events": []}
    if spec.donor == "go":
        donor.update(pid=102, active_callbacks=0, active_stream_handlers_and_io=0, active_fixture_workers=1)
    raws = {"victim": forge, "replacement": donor}
    command_source = {"forge": FORGE_SOURCES["command_done"], "go": "go.fixture.append_only_control",
                      "rust": "rust.fixture.control-native-operation-completion"}
    wire_source = "go.pubsub.native_stream." if spec.donor == "go" else "rust.libp2p.passive-upgraded-stream-io"
    behaviour = "go.pubsub.RawTracer.DeliverMessage" if spec.donor == "go" else "rust.libp2p.gossipsub.public-Behaviour"

    def emit(actor_label, kind, source=None, **fields):
        events = raws[actor_label]["events"]
        event = {"sequence": len(events) + 1, "mono_ns": len(events) + 1, "kind": kind,
                 "source": source or FORGE_SOURCES[kind], **fields}
        events.append(event)
        return event

    def done(actor, sequence, kind):
        emit(actor, "command_done", command_source[spec.implementation(actor)],
             command_sequence=sequence, command_kind=kind, status="ok")

    def snapshot(label, *, published=1, accepted=0, received=0, drained=False):
        return emit("victim", "snapshot", label=label, subscriptions=0 if drained else 1, active_handlers=0,
            messages_published=published, messages_delivered=received, messages_accepted=accepted, handler_failures=0,
            application_score_calls=2 if spec.policy == "typed-override" and received else 0,
            mesh_peer_ids=[peers["replacement"]], peer_scores=[{"peer_id": peers["replacement"], "value": 6.0,
                "app_specific_sample": 3.0, "app_specific_score": 6.0}] if spec.policy == "typed-override" else [],
            core={"peers": 1, "mesh_edges": 1, "messages_received": received, "messages_delivered": received,
                  "trace_failures": 0, "application_score_failures": 0})

    def facts(payload):
        return {"bytes": len(payload), "payload_sha256": hashlib.sha256(payload).hexdigest(), "contains_zero": 0 in payload}

    def message(payload, author, sequence):
        return field(2, field(1, _peer(author)) + field(2, payload) + field(3, sequence.to_bytes(8, "big"))
                     + field(4, topic.encode()) + field(5, b"synthetic-signature"))

    def rpc(actor, direction, body):
        return emit(actor, "rpc", FORGE_SOURCES["rpc"] if actor == "victim" else
                    wire_source + direction if spec.donor == "go" else wire_source,
                    peer_id=peers["replacement" if actor == "victim" else "victim"], connection_id="1", stream_id="1",
                    protocol=spec.protocol, direction=direction, receipt=receipt(body, direction))

    snapshot("startup", published=0)
    if spec.policy == "typed-override":
        emit("victim", "configuration_frozen", phase="pubsub.post-start.configure", error_category="forge.plugins.net.p2p.node",
             error_code=2, secret_material="redacted")
    emit("victim", "connection", peer_id=peers["replacement"], connection_id="1", authenticated=True,
         remote_address="/ip4/127.0.0.1/tcp/41002", local_address="/ip4/127.0.0.1/tcp/42001", transport="tcp",
         security="/noise", muxer="/yamux/1.0.0", authentication_basis="native_session_upgrade_output")
    emit("victim", "session", command_sequence=1, session_id=1, local_peer_id=peers["victim"], remote_peer_id=peers["replacement"],
         remote_address="/ip4/127.0.0.1/tcp/41002", local_address="/ip4/127.0.0.1/tcp/42001",
         authentication="noise", authentication_basis="native_session_upgrade_output", closed=False,
         identify_state="identified" if spec.donor == "go" else "unknown", identify_observed=spec.donor == "go",
         identify_required=False, path="direct", direction="outbound",
         muxer="/yamux/1.0.0")
    done("victim", 1, "connect")
    connection_source = "go.network.Conn.authenticated_output" if spec.donor == "go" else wire_source
    emit("replacement", "connection", connection_source, peer_id=peers["victim"], connection_id="1", authenticated=True,
         remote_address="/ip4/127.0.0.1/tcp/42001", transport="tcp", security="/noise", muxer="/yamux/1.0.0",
         authentication_basis=AUTHENTICATION[spec.donor]["tcp"])
    emit("replacement", "protocol", "go.network.Stream.Protocol" if spec.donor == "go" else wire_source,
         peer_id=peers["victim"], connection_id="1", stream_id="1", protocol=spec.protocol)
    if spec.policy == "typed-override":
        emit("victim", "protocol", peer_id=peers["replacement"], connection_id="1", stream_id="1", protocol=spec.protocol)
    rpc("replacement", "read", field(1, field(1, 1) + field(2, topic.encode())))
    emit("replacement", "snapshot", "go.pubsub.RawTracer_mesh_and_score_inspection" if spec.donor == "go" else behaviour,
         label="periodic", mesh_peer_ids=[peers["victim"]], peer_scores=[])
    emit("victim", "publish", command_sequence=2, topic=topic, author_peer=peers["victim"], seqno_hex="0000000000000001",
         at_payload_limit=True, payload_hex=binary.hex(), **facts(binary))
    body = message(binary, peers["victim"], 1)
    if spec.policy == "typed-override":
        rpc("victim", "write", body)
    done("victim", 2, "publish_binary")
    rpc("replacement", "read", body)
    binary_fields = {"payload_sha256": hashlib.sha256(binary).hexdigest(), "author_peer": peers["victim"],
        "propagation_peer": peers["victim"], "topic": topic, "seqno_hex": "0000000000000001", "outcome": "accept"}
    if spec.donor == "go":
        emit("replacement", "delivery", behaviour, **binary_fields, payload_bytes=len(binary), committed=True, phase="post_decision")
        emit("replacement", "subscription_delivery", "go.pubsub.Subscription.Next", **binary_fields, payload_bytes=len(binary))
    else:
        emit("replacement", "validation", behaviour, **binary_fields, validation_commit=True, report_message_validation_result=True)
        emit("replacement", "delivery", behaviour, **binary_fields, validation_commit=True, report_message_validation_result=True)
    emit("victim", "publish_rejected", command_sequence=3, expected_limit=spec.payload_limit, payload=facts(oversized),
         error_category="forge.net.p2p" if spec.policy == "typed-override" else "forge.plugins.net.p2p.node",
         error_code=11 if spec.policy == "typed-override" else 7, phase="pubsub.oversized_publish", secret_material="redacted")
    emit("victim", "publish_rejection_counters", command_sequence=3, messages_published_before=1, messages_published_after=1,
         core_published_before=1, core_published_after=1)
    done("victim", 3, "publish_oversized")
    snapshot("before-donor")
    done("victim", 4, "sample")
    published = emit("replacement", "publish", "go.pubsub.Topic.Publish" if spec.donor == "go" else behaviour,
         command_sequence=1, payload_sha256=hashlib.sha256(marker.encode()).hexdigest(), topic=topic, error=None, outcome="published")
    if spec.donor == "go":
        published["payload_bytes"] = len(marker)
    marker_body = message(marker.encode(), peers["replacement"], 2)
    rpc("replacement", "write", marker_body)
    done("replacement", 1, "publish")
    if spec.policy == "typed-override":
        rpc("victim", "read", marker_body)
    delivery_fields = {**facts(marker.encode()), "propagation_peer": peers["replacement"], "author_peer": peers["replacement"],
                       "topic": topic, "seqno_hex": "0000000000000002", "outcome": "accept"}
    emit("victim", "subscription_delivery", **delivery_fields, payload_hex=marker.encode().hex(), native_validation_commit=False)
    if spec.policy == "typed-override":
        emit("victim", "validation", **delivery_fields)
    snapshot("after-donor", accepted=1, received=1)
    done("victim", 5, "sample")
    snapshot("drained", accepted=1, received=1, drained=True)
    forge.update(command_admission_closed=True, subscription_drained=True, commands_completed=6)
    emit("victim", "shutdown_prepared", command_sequence=6, actor="victim", case_token=token, local_peer_id=peers["victim"],
         admission_closed=True, pending_commands=0, active_handlers=0)
    done("victim", 6, "prepare_shutdown")
    emit("replacement", "shutdown_prepared", spec.donor + ".fixture.prepare_shutdown", command_sequence=2, actor="replacement",
         case_token=token, local_peer_id=peers["replacement"], admission_closed=True, pending_commands=0)
    done("replacement", 2, "prepare_shutdown")
    ready = {}
    for actor, port in (("victim", 41001), ("replacement", 41002)):
        ready[actor] = {"schema_version": 1, "implementation": spec.implementation(actor), "actor": actor,
            "case_token": token, "local_peer_id": peers[actor], "peer_id": peers[actor], "ready": True,
            "subscription_created": True, "topic": topic, "listen_addrs": [f"/ip4/127.0.0.1/tcp/{port}/p2p/" + peers[actor]]}
        if actor == "victim":
            ready[actor].update(fixture_kind="configured-plugin", configuration_accepted=True, store_backend=spec.backend,
                                status="ready", version=spec.version, pubsub_policy=spec.policy)
    prepared, operations = {}, []

    def row(kind, actor, **fields):
        operations.append({"sequence": len(operations) + 1, "kind": kind, "actor": actor, "case_token": token,
                           "local_peer_id": peers[actor], "pid": pids[actor], **fields})

    for actor in ACTORS:
        ack = next(event for event in raws[actor]["events"] if event["kind"] == "shutdown_prepared")
        prepared[actor] = {"schema_version": 1, "source": "python.fixture.native_prepare_snapshot", "actor": actor,
            "case_token": token, "pid": pids[actor], "command_sequence": ack["command_sequence"],
            "ack_event_sequence": ack["sequence"], "result": deepcopy(raws[actor])}
        row("prepare_ack", actor, command_sequence=ack["command_sequence"], ack_event_sequence=ack["sequence"],
            evidence_file=str(work / (actor + ".prepare.json")))
    if spec.donor == "go":
        ack_sequence = prepared["replacement"]["ack_event_sequence"]
        row("quiesce_requested", "replacement", command_sequence=3, prepare_ack_sequence=ack_sequence)
        emit("replacement", "pre_cancel_retained_resets_returned", "go.fixture.owned_pre_cancel_retained_resets",
            actor="replacement", case_token=token, local_peer_id=peers["replacement"], pid=102,
            command_sequence=3, prepare_ack_sequence=ack_sequence, retained_owners=[], reset_return_receipt_sequences=[],
            native_admission_closed=True, pubsub_callback_admission_closed=True, pubsub_context_cancelled=False,
            subscriber_context_cancelled=False, active_stream_handlers_and_io=0, active_pubsub_streams=0,
            active_callbacks=0, phase_scope="retained_stream_Reset_returns_not_IO_framing_callback_lower_QUIC_or_router_join")
        quiesced = emit("replacement", "shutdown_quiesced", "go.fixture.owned_pubsub_quiesce", actor="replacement", case_token=token,
            local_peer_id=peers["replacement"], pid=102, command_sequence=3, prepare_ack_sequence=ack_sequence,
            native_admission_closed=True, pubsub_callback_admission_closed=True, pubsub_context_cancelled=True,
            subscriber_context_cancelled=True, active_stream_handlers_and_io=0, active_pubsub_streams=0,
            active_fixture_workers=0, active_callbacks=0,
            joined_scope="fixture_subscriber_admitted_stream_IO_framing_pending_terminal_and_observer_callbacks")
        done("replacement", 3, "quiesce_shutdown")
        row("quiesce_ack", "replacement", command_sequence=3, prepare_ack_sequence=ack_sequence, ack_event_sequence=quiesced["sequence"])
        donor.update(host_close_returned=True, active_callbacks=0, active_fixture_workers=0, active_stream_handlers_and_io=0)
        emit("replacement", "shutdown", "go.fixture.owned_context_cancel_and_drain", context_cancelled=True,
             joined=True, host_close_returned=True, active_stream_handlers_and_io=0, active_fixture_workers=0)
    else:
        donor.update(native_close={"live_muxers": 0, "live_streams": 0, "connections": [{"connection_id": "1",
            "dropped": True, "close_returned": True}]}, task_join={"fixture_owned_tasks_joined": True, "overflow": False, "errors": []})
        emit("replacement", "shutdown_requested", "rust.fixture.native_host_close", listeners=1)
    donor.update(finalized=True, joined=True)
    forge.update(final=True, finalized=True, joined=True, lifecycle={"plugins_joined": True, "scheduler_joined": True})
    for kind, actor in (("donor_stop_published", "replacement"), ("donor_joined", "replacement"),
                        ("forge_stop_published", "victim"), ("forge_joined", "victim")):
        row(kind, actor)
    controls = {"victim": [{"sequence": 1, "kind": "connect", "peer_id": peers["replacement"], "address": ready["replacement"]["listen_addrs"][0]},
        {"sequence": 2, "kind": "publish_binary", "payload_hex": binary.hex()},
        {"sequence": 3, "kind": "publish_oversized", "payload_hex": oversized.hex()},
        {"sequence": 4, "kind": "sample", "label": "before-donor"}, {"sequence": 5, "kind": "sample", "label": "after-donor"},
        {"sequence": 6, "kind": "prepare_shutdown", "actor": "victim", "case_token": token, "local_peer_id": peers["victim"]}],
        "replacement": [{"sequence": 1, "kind": "publish", "payload": marker},
                        {"sequence": 2, "kind": "prepare_shutdown", "actor": "replacement", "case_token": token,
                         "local_peer_id": peers["replacement"]}]}
    if spec.donor == "go":
        controls["replacement"].append({"sequence": 3, "kind": "quiesce_shutdown", "actor": "replacement", "case_token": token,
            "local_peer_id": peers["replacement"], "prepare_ack_sequence": prepared["replacement"]["ack_event_sequence"]})
    endpoints, attempts = {}, []
    for actor in ACTORS:
        argv = command(spec, actor, BINARY_PATHS[spec.implementation(actor)], work, token)
        process = {"pid": pids[actor], "command": argv, "log_file": str(work / (actor + ".log")), "ready": ready[actor],
            "terminal_status": {"exit_code": 0, "termination": "graceful"}, "outputs": [],
            "stop_budget": {"native_close_seconds": 10 if actor == "victim" else 8, "post_stop_seconds": 0,
                            "scheduler_allowance_seconds": 10 if actor == "victim" else 2, "seconds": 20 if actor == "victim" else 10}}
        for flag, suffix, payload in (("--result-file", ".json", raws[actor]), ("--ready-file", ".ready.json", ready[actor])):
            path = work / (actor + suffix)
            captured = work / (actor + ".log." + flag[2:] + ".json")
            path.write_text(json.dumps(payload))
            captured.write_text(json.dumps(payload))
            process["outputs"].append({"argument": flag, "path": str(path), "exists": True, "log_file": str(captured)})
        (work / (actor + ".prepare.json")).write_text(json.dumps(prepared[actor]))
        (work / (actor + ".control")).write_text("".join(json.dumps(operation) + "\n" for operation in controls[actor]))
        endpoints[actor] = {"implementation": spec.implementation(actor), "process": process, "result": raws[actor],
                           "result_file": str(work / (actor + ".json")), "store_existed_before_spawn": False}
        attempts.append({"kind": actor, "scenario_id": spec.scenario_id, "attempt_id": 1, "command": argv,
            "pid": pids[actor], "exit_code": 0, "terminal_status": process["terminal_status"], "outputs": process["outputs"],
            "requested_log_file": process["log_file"], "log_file": process["log_file"], "timeout_seconds": 20})
    return {"suite": "plugin-pubsub", "scenario_id": spec.scenario_id, "spec": vars(spec), "case_token": token,
        "operation_budget_seconds": 40, "endpoints": endpoints, "controls": controls, "prepared": prepared,
        "shutdown_barrier": {"source": "python.fixture.configured_pubsub_prepare_barrier", "operations": operations},
        "owned_processes": [endpoint["process"] for endpoint in endpoints.values()], "attempts": attempts}


class PluginPubSubEvidenceTests(unittest.TestCase):
    def assert_positive_baseline(self, record, spec, root):
        self.assertEqual(validate_case(record, spec), [])
        self.assertEqual(validate_terminal_sources(record, BINARY_PATHS, root), [])

    def test_all_eight_synthetic_cases_and_strict_sources(self):
        for backend in ("mdbx", "rocksdb"):
            for spec in case_specs(backend):
                with self.subTest(spec=spec), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve()
                    record = synthetic_record(spec, root)
                    before = deepcopy(record)
                    self.assert_positive_baseline(record, spec, root)
                    self.assertTrue(_same_json(record, before))

    def test_authenticated_session_accepts_actual_identify_states_without_stream_claim(self):
        for state in ("unknown", "identifying", "identified", "failed"):
            for policy in ("yaml-only", "typed-override"):
                with self.subTest(state=state, policy=policy), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve()
                    spec = Case("mdbx", "rust", policy)
                    record = synthetic_record(spec, root)
                    self.assert_positive_baseline(record, spec, root)
                    raw = deepcopy(record["endpoints"]["victim"]["result"])
                    session = next(event for event in raw["events"] if event["kind"] == "session")
                    session.update(identify_state=state, identify_observed=state == "identified")
                    self.assertNotIn("stream_authentication", session)
                    remote = record["endpoints"]["replacement"]["result"]["local_peer_id"]
                    ready = record["endpoints"]["replacement"]["process"]["ready"]
                    self.assertIsNotNone(_forge_connection(raw, remote, ready))

    def test_session_contract_rejects_missing_or_forged_authentication_and_owners(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            spec = Case("mdbx", "rust", "typed-override")
            record = synthetic_record(spec, root)
            self.assert_positive_baseline(record, spec, root)
            forge = record["endpoints"]["victim"]["result"]
            remote = record["endpoints"]["replacement"]["result"]["local_peer_id"]
            ready = record["endpoints"]["replacement"]["process"]["ready"]
            missing = object()
            mutations = [("session", field, missing) for field in (
                "source", "session_id", "local_peer_id", "remote_peer_id", "closed", "authentication",
                "authentication_basis", "muxer", "identify_state", "identify_observed", "identify_required",
                "local_address", "remote_address")]
            mutations += [("session", field, value) for field, value in (
                ("source", "forge.node.diagnostics_api"), ("session_id", 0), ("session_id", True),
                ("local_peer_id", peer_id(3)), ("remote_peer_id", peer_id(3)), ("closed", True), ("closed", 0),
                ("authentication", "test_insecure"), ("authentication", "tls"),
                ("authentication_basis", "fixture.configured_security"), ("muxer", "/mplex/6.7.0"),
                ("identify_state", "pending"), ("identify_state", "identified"),
                ("identify_observed", True), ("identify_observed", 0),
                ("identify_required", True), ("identify_required", 0), ("stream_authentication", "noise"),
                ("path", "relayed"), ("direction", "inbound"), ("remote_address", "/ip4/127.0.0.1/tcp/1"))]
            mutations += [("connection", field, value) for field, value in (
                ("authenticated", missing), ("authenticated", False), ("security", missing),
                ("authentication_basis", missing), ("connection_id", "2"), ("peer_id", peer_id(3)))]
            for kind, field, value in mutations:
                with self.subTest(kind=kind, field=field, value="missing" if value is missing else value):
                    raw = deepcopy(forge)
                    self.assertIsNotNone(_forge_connection(raw, remote, ready))
                    witness = next(event for event in raw["events"] if event["kind"] == kind)
                    if value is missing:
                        del witness[field]
                    else:
                        witness[field] = value
                    with self.assertRaises(ValueError):
                        _forge_connection(raw, remote, ready)
            for kind in ("session", "connection"):
                with self.subTest(missing=kind):
                    raw = deepcopy(forge)
                    self.assertIsNotNone(_forge_connection(raw, remote, ready))
                    raw["events"] = [event for event in raw["events"] if event["kind"] != kind]
                    with self.assertRaises(ValueError):
                        _forge_connection(raw, remote, ready)
                with self.subTest(duplicate=kind):
                    raw = deepcopy(forge)
                    self.assertIsNotNone(_forge_connection(raw, remote, ready))
                    raw["events"].append(deepcopy(next(event for event in raw["events"] if event["kind"] == kind)))
                    with self.assertRaises(ValueError):
                        _forge_connection(raw, remote, ready)

    def test_donor_socket_matches_forge_local_with_optional_owned_peer_suffix(self):
        def replace_donor_address(record, address):
            endpoint = record["endpoints"]["replacement"]
            prepared = record["prepared"]["replacement"]
            for raw in (endpoint["result"], prepared["result"]):
                for event in raw["events"]:
                    if event["kind"] == "connection":
                        event["remote_address"] = address
            # Keep terminal files/captures and the independent active prepare
            # coherent so rejection cannot come from a stale source or prefix.
            for output in endpoint["process"]["outputs"]:
                if output["argument"] == "--result-file":
                    payload = json.dumps(endpoint["result"])
                    Path(output["path"]).write_text(payload)
                    Path(output["log_file"]).write_text(payload)
            work = Path(endpoint["process"]["log_file"]).parent
            (work / "replacement.prepare.json").write_text(json.dumps(prepared))

        for donor in ("go", "rust"):
            for peer_suffix in (False, True):
                with self.subTest(donor=donor, peer_suffix=peer_suffix), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve()
                    spec = Case("mdbx", donor, "typed-override")
                    record = synthetic_record(spec, root)
                    self.assert_positive_baseline(record, spec, root)
                    forge = record["endpoints"]["victim"]["result"]
                    suffix = "/p2p/" + forge["local_peer_id"] if peer_suffix else ""
                    local = next(event for event in forge["events"] if event["kind"] == "connection")["local_address"]
                    replace_donor_address(record, local + suffix)
                    self.assert_positive_baseline(record, spec, root)
                    replace_donor_address(record, "/ip4/127.0.0.1/tcp/1" + suffix)
                    self.assertEqual(validate_terminal_sources(record, BINARY_PATHS, root), [])
                    self.assertEqual(validate_case(record, spec), [
                        "plugin-pubsub evidence: donor remote socket differs from Forge local socket"])

    def test_rejects_missing_acceptance_wrong_protocol_policy_boundary_and_join(self):
        modes = ("callback-only", "wrong-rpc-version", "wrong-wire-hash", "wrong-author", "wrong-signature",
                 "wrong-limit", "wrong-typed-error", "rejected-counter-changed", "missing-delivery", "score-not-called",
                 "score-not-weighted", "typed-not-competing", "tracer-without-typed", "unjoined", "wrong-order",
                 "missing-prepare", "prepare-after-join", "control-retry", "command-error", "same-peer", "insecure",
                 "wrong-event-source", "boolean-counter", "invalid-identify-state", "foreign-socket", "native-rejected")
        spec = Case("mdbx", "rust", "typed-override")
        for mode in modes:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                record = synthetic_record(spec, root)
                self.assert_positive_baseline(record, spec, root)
                forge, donor = (record["endpoints"][actor]["result"] for actor in ACTORS)
                after = next(event for event in forge["events"] if event.get("label") == "after-donor")
                rpc = next(event for event in donor["events"] if event["kind"] == "rpc" and event["direction"] == "read"
                           and len(event["receipt"]["framed_hex"]) > 400)
                if mode == "callback-only":
                    after["messages_accepted"] = after["messages_delivered"] = 0
                    after["core"]["messages_received"] = after["core"]["messages_delivered"] = 0
                elif mode == "wrong-rpc-version":
                    rpc["protocol"] = "/meshsub/1.0.0"
                elif mode == "wrong-wire-hash":
                    rpc["receipt"]["read"]["framed_sha256"] = "f" * 64
                elif mode == "wrong-author":
                    next(event for event in forge["events"] if event["kind"] == "subscription_delivery")["author_peer"] = peer_id(3)
                elif mode == "wrong-signature":
                    payload = probes(spec, record["case_token"])[0]
                    body = field(2, field(1, _peer(forge["local_peer_id"])) + field(2, payload)
                                 + field(3, bytes.fromhex("0000000000000001")) + field(4, forge["topic"].encode()))
                    rpc["receipt"] = receipt(body)
                elif mode == "wrong-limit":
                    next(event for event in forge["events"] if event["kind"] == "publish")["bytes"] = 128
                elif mode == "wrong-typed-error":
                    next(event for event in forge["events"] if event["kind"] == "publish_rejected")["error_code"] = 15
                elif mode == "rejected-counter-changed":
                    next(event for event in forge["events"] if event["kind"] == "publish_rejection_counters")["core_published_after"] = 2
                elif mode == "missing-delivery":
                    next(event for event in donor["events"] if event["kind"] == "delivery")["kind"] = "validation"
                elif mode == "score-not-called":
                    after["application_score_calls"] = 0
                elif mode == "score-not-weighted":
                    after["peer_scores"][0]["app_specific_score"] = 0
                elif mode == "typed-not-competing":
                    forge["accepted_configuration"]["node_reconfigure"]["document"]["plugins"]["net"]["p2p"]["node"]["pubsub"]["preferred-version"] = "v1-2"
                elif mode == "tracer-without-typed":
                    forge["native_tracer_installed"] = False
                elif mode == "unjoined":
                    donor["joined"] = False
                elif mode == "wrong-order":
                    record["shutdown_barrier"]["operations"][-4]["kind"] = "forge_stop_published"
                elif mode == "missing-prepare":
                    del record["prepared"]["replacement"]
                elif mode == "prepare-after-join":
                    record["prepared"]["victim"]["result"]["joined"] = True
                elif mode == "control-retry":
                    record["controls"]["victim"].append(deepcopy(record["controls"]["victim"][1]))
                elif mode == "command-error":
                    next(event for event in forge["events"] if event["kind"] == "command_done")["status"] = "error"
                elif mode == "same-peer":
                    donor["local_peer_id"] = forge["local_peer_id"]
                elif mode == "insecure":
                    forge["accepted_configuration"]["document"]["plugins"]["net"]["p2p"]["node"]["allow-insecure-test-mode"] = True
                elif mode == "wrong-event-source":
                    after["source"] = "forge.fixture.configured_counter"
                elif mode == "boolean-counter":
                    after["core"]["messages_received"] = True
                elif mode == "invalid-identify-state":
                    next(event for event in forge["events"] if event["kind"] == "session")["identify_state"] = "pending"
                elif mode == "foreign-socket":
                    next(event for event in forge["events"] if event["kind"] == "session")["remote_address"] = "/ip4/127.0.0.1/tcp/1"
                elif mode == "native-rejected":
                    next(event for event in forge["events"] if event["kind"] == "validation")["outcome"] = "reject"
                self.assertTrue(validate_case(record, spec), mode)

    def test_go_prepare_requires_separate_quiesce_and_actual_subscriber_delivery(self):
        for mode in ("quiesce", "subscriber", "unjoined-host", "borrowed-prepare"):
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                spec = Case("rocksdb", "go", "yaml-only")
                root = Path(directory).resolve()
                record = synthetic_record(spec, root)
                self.assert_positive_baseline(record, spec, root)
                donor = record["endpoints"]["replacement"]["result"]
                if mode == "quiesce":
                    next(event for event in donor["events"] if event["kind"] == "shutdown_quiesced")["active_callbacks"] = 1
                elif mode == "subscriber":
                    next(event for event in donor["events"] if event["kind"] == "subscription_delivery")["payload_sha256"] = "f" * 64
                elif mode == "unjoined-host":
                    donor["host_close_returned"] = False
                else:
                    record["prepared"]["replacement"]["pid"] = 101
                self.assertTrue(validate_case(record, spec))

    def test_callback_without_later_native_counter_is_not_ready(self):
        with tempfile.TemporaryDirectory() as directory:
            spec = Case("mdbx", "go", "yaml-only")
            root = Path(directory).resolve()
            record = synthetic_record(spec, root)
            self.assert_positive_baseline(record, spec, root)
            forge = record["endpoints"]["victim"]["result"]
            delivery = next(event for event in forge["events"] if event["kind"] == "subscription_delivery")
            forge["events"] = forge["events"][:delivery["sequence"]]
            self.assertIsNone(forge_delivery(forge, spec, record["endpoints"]["replacement"]["result"]["local_peer_id"], record["case_token"]))

    def test_marker_wait_requires_matching_owned_signed_native_write(self):
        for backend in ("mdbx", "rocksdb"):
            for spec in case_specs(backend):
                with self.subTest(spec=spec), tempfile.TemporaryDirectory() as directory:
                    root = Path(directory).resolve()
                    record = synthetic_record(spec, root)
                    self.assert_positive_baseline(record, spec, root)
                    forge = record["prepared"]["victim"]["result"]
                    donor = record["prepared"]["replacement"]["result"]
                    remote, token = forge["local_peer_id"], record["case_token"]
                    delivered = forge_delivery(forge, spec, donor["local_peer_id"], token)
                    self.assertIsNotNone(delivered)
                    self.assertTrue(donor_marker_written(donor, spec, remote, token, delivered))
                    marker = probes(spec, token)[2].encode()
                    for mode in ("identity", "rpc-peer", "connection", "stream", "hash", "payload",
                                 "author", "seqno", "topic", "signature", "direction"):
                        with self.subTest(mode=mode):
                            raw = deepcopy(donor)
                            rpc = next(event for event in raw["events"] if event["kind"] == "rpc"
                                       and event["direction"] == "write")
                            if mode == "identity":
                                raw["local_peer_id"] = peer_id(3)
                            elif mode == "rpc-peer":
                                rpc["peer_id"] = peer_id(3)
                            elif mode == "connection":
                                rpc["connection_id"] = "foreign"
                            elif mode == "stream":
                                rpc["stream_id"] = "foreign"
                            elif mode == "hash":
                                rpc["receipt"]["write"]["framed_sha256"] = "f" * 64
                            else:
                                body = field(1, _peer(peer_id(3) if mode == "author" else donor["local_peer_id"]))
                                body += field(2, marker + b"!" if mode == "payload" else marker)
                                body += field(3, (3 if mode == "seqno" else 2).to_bytes(8, "big"))
                                body += field(4, ("foreign" if mode == "topic" else forge["topic"]).encode())
                                if mode != "signature":
                                    body += field(5, b"synthetic-signature")
                                rpc["direction"] = "read" if mode == "direction" else "write"
                                if mode == "direction" and spec.donor == "go":
                                    rpc["source"] = "go.pubsub.native_stream.read"
                                rpc["receipt"] = receipt(field(2, body), rpc["direction"])
                            if mode in ("identity", "rpc-peer", "connection", "stream", "hash"):
                                with self.assertRaises(ValueError):
                                    donor_marker_written(raw, spec, remote, token, delivered)
                            else:
                                self.assertFalse(donor_marker_written(raw, spec, remote, token, delivered))
                            with self.assertRaises(ValueError):
                                validate_delivery({"victim": forge, "replacement": raw}, spec, token)

    def test_delayed_marker_write_receipt_waits_without_republishing(self):
        for implementation in ("go", "rust"):
            with self.subTest(implementation=implementation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                spec = Case("rocksdb", implementation, "yaml-only")
                record = synthetic_record(spec, root)
                self.assert_positive_baseline(record, spec, root)
                forge = record["prepared"]["victim"]["result"]
                donor = deepcopy(record["prepared"]["replacement"]["result"])
                # Model command completion published before the native write.
                wire = next(event for event in donor["events"] if event["kind"] == "rpc"
                            and event["direction"] == "write")
                donor["events"].remove(wire)
                done = next(index for index, event in enumerate(donor["events"])
                            if event["kind"] == "command_done" and event["command_kind"] == "publish")
                donor["events"].insert(done + 1, wire)
                for sequence, event in enumerate(donor["events"], 1):
                    event.update(sequence=sequence, mono_ns=sequence)
                stale = deepcopy(donor)
                stale["events"] = stale["events"][:done + 1]
                token, remote = record["case_token"], forge["local_peer_id"]
                delivered = forge_delivery(forge, spec, donor["local_peer_id"], token)
                self.assertIsNotNone(delivered)
                self.assertFalse(donor_marker_written(stale, spec, remote, token, delivered))
                with self.assertRaisesRegex(ValueError, "marker lacks native donor write"):
                    validate_delivery({"victim": forge, "replacement": stale}, spec, token)
                path = root / "delayed.json"
                path.write_text(json.dumps(stale))
                owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
                original_controls = deepcopy(record["controls"])
                with patch("plugin_pubsub_cases.time.monotonic", side_effect=(1, 1.1)), \
                        patch("plugin_pubsub_cases.time.sleep", side_effect=lambda _: path.write_text(json.dumps(donor))) as poll:
                    observed = _wait(owner, path,
                        lambda raw: donor_marker_written(raw, spec, remote, token, delivered), 2)
                self.assertEqual(poll.call_count, 1)
                self.assertEqual(observed, donor)
                self.assertEqual(record["controls"], original_controls)
                validate_delivery({"victim": forge, "replacement": observed}, spec, token)

    def test_missing_marker_write_expires_same_observation_deadline(self):
        for implementation in ("go", "rust"):
            with self.subTest(implementation=implementation), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                spec = Case("mdbx", implementation, "yaml-only")
                record = synthetic_record(spec, root)
                self.assert_positive_baseline(record, spec, root)
                forge = record["prepared"]["victim"]["result"]
                donor = deepcopy(record["prepared"]["replacement"]["result"])
                wire = next(event for event in donor["events"] if event["kind"] == "rpc"
                            and event["direction"] == "write")
                donor["events"].remove(wire)
                for sequence, event in enumerate(donor["events"], 1):
                    event.update(sequence=sequence, mono_ns=sequence)
                token, remote = record["case_token"], forge["local_peer_id"]
                delivered = forge_delivery(forge, spec, donor["local_peer_id"], token)
                self.assertIsNotNone(delivered)
                self.assertFalse(donor_marker_written(donor, spec, remote, token, delivered))
                path = root / "missing.json"
                path.write_text(json.dumps(donor))
                owner = SimpleNamespace(process=SimpleNamespace(poll=lambda: None))
                with patch("plugin_pubsub_cases.time.monotonic", side_effect=(1, 2)), \
                        patch("plugin_pubsub_cases.time.sleep") as poll, \
                        self.assertRaisesRegex(TimeoutError, "observation deadline: missing.json"):
                    _wait(owner, path, lambda raw: donor_marker_written(raw, spec, remote, token, delivered), 2)
                self.assertEqual(poll.call_count, 1)
                with self.assertRaisesRegex(ValueError, "marker lacks native donor write"):
                    validate_delivery({"victim": forge, "replacement": donor}, spec, token)

    def assert_run_case_marker_barrier(self, spec, root, *, receipt_arrives):
        record = synthetic_record(spec, root)
        self.assert_positive_baseline(record, spec, root)
        token = record["case_token"]
        work = root / spec.scenario_id.replace("/", "-")
        native = {actor: deepcopy(record["endpoints"][actor]["result"]) for actor in ACTORS}
        donor_events = native["replacement"]["events"]
        wire = next(event for event in donor_events if event["kind"] == "rpc" and event["direction"] == "write")
        donor_events.remove(wire)
        done_index = next(index for index, event in enumerate(donor_events)
                          if event["kind"] == "command_done" and event["command_kind"] == "publish")
        donor_events.insert(done_index + 1, wire)
        for sequence, event in enumerate(donor_events, 1):
            event.update(sequence=sequence, mono_ns=sequence)
        controls, timeline, deadlines, owners = {actor: [] for actor in ACTORS}, [], [], {}
        clock = SimpleNamespace(now=100.0)
        deadline = clock.now + 0.25 + CASE_SECONDS
        scope = SimpleNamespace(attempts=[], evidence=lambda: [])

        def monotonic():
            clock.now += 0.25
            return clock.now

        def capture(actor, count):
            raw = deepcopy(record["prepared"][actor]["result"])
            raw["events"] = deepcopy(native[actor]["events"][:count])
            if actor == "victim":
                prepared = any(event["kind"] == "shutdown_prepared" for event in raw["events"])
                raw.update(command_admission_closed=prepared, subscription_drained=prepared,
                           commands_completed=sum(event["kind"] == "command_done" for event in raw["events"]))
            elif any(event["kind"] == "shutdown_quiesced" for event in raw["events"]):
                raw["active_fixture_workers"] = 0
            (work / (actor + ".json")).write_text(json.dumps(raw))
            return raw

        def completion(actor, kind, sequence):
            return next(event["sequence"] for event in native[actor]["events"]
                        if event["kind"] == "command_done" and event["command_kind"] == kind
                        and event["command_sequence"] == sequence)

        def attempt(argv, log, scenario, number, actor, budget):
            self.assertEqual((scenario, number), (spec.scenario_id, 1))
            scope.attempts.append({"actor": actor, "command": argv})
            return scope.attempts[-1]

        def spawn(argv, log, stop, attempted, *, stop_budget):
            actor = argv[argv.index("--actor") + 1]
            endpoint = record["endpoints"][actor]
            (work / (actor + ".ready.json")).write_text(json.dumps(endpoint["process"]["ready"]))
            capture(actor, 1)
            def close():
                timeline.append(actor + ":joined")
                (work / (actor + ".json")).write_text(json.dumps(native[actor]))
                return []
            owner = SimpleNamespace(process=SimpleNamespace(pid=endpoint["process"]["pid"], poll=lambda: None),
                request_stop=lambda: timeline.append(actor + ":stop"), close=close,
                evidence=lambda: endpoint["process"])
            owners[actor] = owner
            return owner

        def issue(path, issued, process_role, kind, **fields):
            actor = process_role
            sequence = _append_control(path, issued, actor, kind, **fields)
            controls[actor].append(deepcopy(issued[actor][-1]))
            self.assertEqual(controls[actor][-1], record["controls"][actor][sequence - 1])
            timeline.append(actor + ":" + kind)
            raw = capture(actor, completion(actor, kind, sequence))
            if actor == "victim" and kind == "connect":
                # Connect publishes both peers' authenticated stream/subscription
                # and mesh evidence before either side is allowed to publish.
                donor = capture("replacement", next(event["sequence"] for event in donor_events
                                if event["kind"] == "snapshot" and event["label"] == "periodic"))
                _forge_connection(raw, donor["local_peer_id"], owners["replacement"].ready)
                self.assertTrue(delivery_ready(donor, spec, raw["local_peer_id"], token))
                timeline.append("replacement:delivery_ready")
            if actor == "victim" and kind == "publish_binary":
                capture("replacement", next(event["sequence"] - 1 for event in donor_events if event["kind"] == "publish"))
            if actor == "replacement" and kind == "publish":
                # The receiver has delivered and sampled before the writer's
                # next receipt publication, despite command_done being visible.
                capture("victim", completion("victim", "sample", 5) - 1)
            return sequence

        def wait(owner, path, predicate, observed_deadline):
            deadlines.append((path.name, observed_deadline))
            return _wait(owner, path, predicate, observed_deadline)

        def poll_receipt(_):
            self.assertEqual([item["kind"] for item in controls["victim"]],
                             ["connect", "publish_binary", "publish_oversized", "sample"])
            self.assertEqual([item["kind"] for item in controls["replacement"]], ["publish"])
            self.assertEqual(deadlines[-1], ("replacement.json", deadline))
            forge = json.loads((work / "victim.json").read_text())
            donor = json.loads((work / "replacement.json").read_text())
            delivered = forge_delivery(forge, spec, donor["local_peer_id"], token)
            self.assertIsNotNone(delivered)
            self.assertEqual(donor["events"][-1]["command_kind"], "publish")
            self.assertFalse(donor_marker_written(donor, spec, forge["local_peer_id"], token, delivered))
            timeline.append("marker:pending")
            if receipt_arrives:
                donor = capture("replacement", wire["sequence"])
                self.assertTrue(donor_marker_written(donor, spec, forge["local_peer_id"], token, delivered))
                timeline.append("marker:written")
            else:
                clock.now = deadline

        with patch("plugin_pubsub_cases.current_scope", return_value=scope), \
                patch("plugin_pubsub_cases.spawn_owned", side_effect=spawn) as spawned, \
                patch("plugin_pubsub_cases.secrets.token_hex", return_value=token), \
                patch("plugin_pubsub_cases.time.monotonic", side_effect=monotonic), \
                patch("plugin_pubsub_cases.time.sleep", side_effect=poll_receipt) as poll, \
                patch("plugin_pubsub_cases._append_control", side_effect=issue), \
                patch("plugin_pubsub_cases._wait", side_effect=wait):
            if receipt_arrives:
                result = run_case(spec, BINARY_PATHS, root, command_attempt=attempt)
                self.assertEqual(result["controls"], record["controls"])
                self.assertEqual(controls, record["controls"])
                validate_delivery({actor: result["prepared"][actor]["result"] for actor in ACTORS}, spec, token)
                written = timeline.index("marker:written")
                self.assertLess(timeline.index("marker:pending"), written)
                for actor in ACTORS:
                    self.assertLess(written, timeline.index(actor + ":prepare_shutdown"))
                    self.assertLess(written, timeline.index(actor + ":stop"))
                if spec.donor == "go":
                    self.assertLess(written, timeline.index("replacement:quiesce_shutdown"))
            else:
                with self.assertRaisesRegex(TimeoutError, "observation deadline: replacement.json"):
                    run_case(spec, BINARY_PATHS, root, command_attempt=attempt)
                self.assertEqual(timeline[-1], "marker:pending")
                self.assertEqual(controls["victim"], record["controls"]["victim"][:4])
                self.assertEqual(controls["replacement"], record["controls"]["replacement"][:1])
                self.assertFalse(any(work.glob("*.prepare.json")))
                with self.assertRaisesRegex(ValueError, "marker lacks native donor write"):
                    validate_delivery({actor: json.loads((work / (actor + ".json")).read_text())
                                       for actor in ACTORS}, spec, token)
            self.assertEqual(poll.call_count, 1)
            self.assertEqual(spawned.call_count, len(ACTORS))
            self.assertEqual([item["actor"] for item in scope.attempts], list(ACTORS))
            self.assertEqual(set(owners), set(ACTORS))
            self.assertLess(timeline.index("replacement:delivery_ready"), timeline.index("victim:publish_binary"))
            self.assertTrue(deadlines)
            self.assertTrue(all(observed == deadline for _, observed in deadlines))
            for actor in ACTORS:
                persisted = [json.loads(line) for line in (work / (actor + ".control")).read_text().splitlines()]
                self.assertEqual(persisted, controls[actor])

    def test_run_case_waits_for_matching_marker_write_before_prepare_and_quiesce(self):
        for backend in ("mdbx", "rocksdb"):
            for spec in case_specs(backend):
                with self.subTest(spec=spec), tempfile.TemporaryDirectory() as directory:
                    self.assert_run_case_marker_barrier(spec, Path(directory).resolve(), receipt_arrives=True)

    def test_run_case_missing_marker_write_fails_closed_without_republish(self):
        for backend in ("mdbx", "rocksdb"):
            for spec in case_specs(backend):
                with self.subTest(spec=spec), tempfile.TemporaryDirectory() as directory:
                    self.assert_run_case_marker_barrier(spec, Path(directory).resolve(), receipt_arrives=False)

    def test_subscription_alone_does_not_release_nonflood_donor_publish(self):
        for donor in ("go", "rust"):
            with self.subTest(donor=donor), tempfile.TemporaryDirectory() as directory:
                spec = Case("mdbx", donor, "yaml-only")
                root = Path(directory).resolve()
                record = synthetic_record(spec, root)
                self.assert_positive_baseline(record, spec, root)
                raw = deepcopy(record["prepared"]["replacement"]["result"])
                remote, token = record["endpoints"]["victim"]["result"]["local_peer_id"], record["case_token"]
                self.assertTrue(delivery_ready(raw, spec, remote, token))
                next(event for event in raw["events"] if event["kind"] == "snapshot")["mesh_peer_ids"] = []
                self.assertFalse(delivery_ready(raw, spec, remote, token))

    def test_terminal_sources_reject_forged_receipts_controls_owner_and_commands(self):
        modes = ("binary", "policy", "extra-owner", "retry", "capture", "ready-capture", "prepare-capture", "control",
                 "duplicate-json", "nonfinite-json", "join-budget", "store", "mutable-inline")
        for mode in modes:
            with self.subTest(mode=mode), tempfile.TemporaryDirectory() as directory:
                spec = Case("mdbx", "rust", "typed-override")
                root = Path(directory).resolve()
                record = synthetic_record(spec, root)
                self.assert_positive_baseline(record, spec, root)
                endpoint = record["endpoints"]["victim"]
                work = Path(endpoint["process"]["log_file"]).parent
                if mode == "binary":
                    endpoint["process"]["command"][0] = "/different/binary"
                elif mode == "policy":
                    endpoint["process"]["command"][-1] = "yaml-only"
                elif mode == "extra-owner":
                    record["owned_processes"].append(deepcopy(endpoint["process"]))
                elif mode == "retry":
                    record["attempts"][0]["attempt_id"] = 2
                elif mode == "capture":
                    Path(endpoint["process"]["outputs"][0]["log_file"]).write_text('{}')
                elif mode == "ready-capture":
                    Path(endpoint["process"]["outputs"][1]["log_file"]).write_text('{}')
                elif mode == "prepare-capture":
                    (work / "victim.prepare.json").write_text('{}')
                elif mode == "control":
                    (work / "victim.control").write_text('{"sequence":1,"kind":"sample","label":"fake"}\n')
                elif mode == "duplicate-json":
                    (work / "victim.json").write_text('{"events":[],"events":[]}')
                elif mode == "nonfinite-json":
                    (work / "victim.json").write_text('{"value":NaN}')
                elif mode == "join-budget":
                    endpoint["process"]["stop_budget"]["seconds"] = 200
                elif mode == "store":
                    endpoint["store_existed_before_spawn"] = True
                else:
                    endpoint["result"]["final"] = False
                self.assertTrue(validate_terminal_sources(record, BINARY_PATHS, root), mode)


if __name__ == "__main__":
    unittest.main()
