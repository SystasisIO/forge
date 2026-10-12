"""Configured PubSub receipts: actual wire, callbacks, counters and owned joins."""

import hashlib
import json
import math
from pathlib import Path
import re

from plugin_node_cases import listener_socket, socket
from plugin_node_evidence import accepted_configuration, counter, load_json, require
from plugin_pubsub_cases import ACTORS, CASE_SECONDS, Case, command, probes
from provenance import reject_duplicate_json_keys
from pubsub_cases import _mesh_is, _subscription_received
from pubsub_evidence import (
    EVENT_LIMIT, _connection_owner, _donor_shutdown_event, _events, _owner, _rpc_peer, _same_json,
    _terminal_owners, prepared_snapshot, quiesce_ack, shutdown_ack,
)
from pubsub_wire import validate_rpc_receipt
from rust_upgrade_evidence import _peer

FORGE_SOURCES = {
    "connection": "forge.node.diagnostics_api", "session": "node.api.connect-and-node.diagnostics-api.session",
    "snapshot": "forge.node.pubsub_api-and-diagnostics_api",
    "subscription_delivery": "forge.node.pubsub_api.subscribe",
    "publish": "forge.node.pubsub_api.publish", "publish_rejected": "forge.node.pubsub_api.publish",
    "publish_rejection_counters": "forge.node.pubsub_api.snapshot",
    "protocol": "forge.pubsub.native_stream", "rpc": "forge.pubsub.native_stream",
    "validation": "forge.pubsub.committed_validation", "delivery": "forge.pubsub.committed_validation",
    "configuration_frozen": "forge.node.configuration_api",
    "shutdown_prepared": "forge.fixture.official_api_drain", "command_done": "forge.fixture.official_api_operation",
}


def ready_actor(raw, spec, actor, token):
    local = raw.get("local_peer_id")
    require(counter(raw.get("schema_version"), 1, 1) and raw.get("implementation") == spec.implementation(actor)
            and raw.get("actor") == actor and raw.get("case_token") == token
            and isinstance(local, str) and 0 < len(local) <= 256 and raw.get("peer_id") == local
            and raw.get("ready") is True and raw.get("subscription_created") is True
            and raw.get("topic") == "forge-pr11:" + token, "foreign/incomplete configured PubSub readiness")
    addresses = raw.get("listen_addrs")
    require(isinstance(addresses, list) and len(addresses) == 1, "ambiguous configured PubSub listener")
    address = addresses[0]
    listener_socket(address, local) if "/p2p/" in address else socket(address)
    if actor == "victim":
        require(raw.get("fixture_kind") == "configured-plugin" and raw.get("configuration_accepted") is True
                and raw.get("store_backend") == spec.backend and raw.get("status") == "ready"
                and raw.get("pubsub_policy") == spec.policy and raw.get("version") == spec.version,
                "Forge readiness is not the requested configured plugin")
    return local


def _forge_events(raw, spec, token, local, *, active):
    require(counter(raw.get("schema_version"), 1, 1) and raw.get("implementation") == "forge"
            and raw.get("actor") == "victim" and raw.get("local_peer_id") == local
            and raw.get("case_token") == token and raw.get("topic") == "forge-pr11:" + token
            and raw.get("version") == spec.version and raw.get("pubsub_policy") == spec.policy
            and raw.get("finalized") is (not active) and raw.get("joined") is (not active)
            and raw.get("final") is (not active) and raw.get("status") == "ok" and raw.get("errors") == []
            and raw.get("overflow") is False and raw.get("capture_error") is False and raw.get("error") is None,
            "Forge PubSub receipt failed, overflowed or has wrong active/terminal identity")
    require(_same_json(raw.get("lifecycle"), {"plugins_joined": not active, "scheduler_joined": not active}),
            "Forge plugin/scheduler join receipt disagrees with its phase")
    events, previous = raw.get("events"), 0
    require(isinstance(events, list) and 0 < len(events) <= EVENT_LIMIT
            and len(json.dumps(raw).encode()) <= 16 * 1024 * 1024, "unbounded/missing Forge PubSub capture")
    for sequence, event in enumerate(events, 1):
        require(isinstance(event, dict) and counter(event.get("sequence"), sequence, sequence)
                and counter(event.get("mono_ns"), max(1, previous), 2**63 - 1)
                and event.get("kind") in FORGE_SOURCES and event.get("source") == FORGE_SOURCES[event["kind"]]
                and len(json.dumps(event).encode()) <= 64 * 1024, "invalid Forge event sequence/clock/source/bound")
        if event["kind"] == "command_done":
            require(event.get("status") == "ok", "Forge official API command failed")
        if event["kind"] == "snapshot":
            _snapshot_fields(event)
        if event["kind"] in {"rpc", "protocol", "validation", "delivery"}:
            require(spec.policy == "typed-override", "YAML-only receipt invented a native tracer")
        previous = event["mono_ns"]
    return events


def active_actor(raw, spec, actor, token, local):
    if actor == "victim":
        return _forge_events(raw, spec, token, local, active=True)
    require(raw.get("local_peer_id") == local, "donor changed its live identity")
    return _events(raw, spec.donor, token, actor, active=True)


def command_done(raw, sequence, kind):
    matches = [event for event in raw.get("events", []) if event.get("kind") == "command_done"
               and event.get("command_sequence") == sequence]
    require(len(matches) <= 1, "duplicate actual command completion")
    if not matches:
        return None
    event = matches[0]
    require(counter(event.get("command_sequence"), sequence, sequence)
            and event.get("command_kind") == kind and event.get("status") == "ok", "command completion failed/rebound")
    return event


def _single(events, kind, **fields):
    matches = [event for event in events if event.get("kind") == kind
               and all(event.get(key) == value for key, value in fields.items())]
    require(len(matches) == 1, "missing/duplicate PubSub witness: " + kind)
    return matches[0]


def _snapshot_fields(event):
    require(all(counter(event.get(key)) for key in ("application_score_calls", "subscriptions", "active_handlers",
            "messages_published", "messages_delivered", "messages_accepted", "handler_failures"))
            and event["handler_failures"] == 0, "invalid/failed official consumer counters")
    core = event.get("core", {})
    require(all(counter(core.get(key)) for key in ("peers", "mesh_edges", "messages_received", "messages_delivered",
            "trace_failures", "application_score_failures")) and core["trace_failures"] == core["application_score_failures"] == 0,
            "invalid/failed native snapshot counters")


def _numeric_address(value, peer):
    return listener_socket(value, peer) if isinstance(value, str) and "/p2p/" in value else socket(value)


def _forge_connection(raw, remote, ready):
    session = _single(raw["events"], "session", command_sequence=1)
    state = session.get("identify_state")
    require(session.get("source") == FORGE_SOURCES["session"] and counter(session.get("session_id"), 1)
            and session.get("local_peer_id") == raw["local_peer_id"] and session.get("remote_peer_id") == remote
            and session.get("closed") is False and state in ("unknown", "identifying", "identified", "failed")
            and session.get("identify_observed") is (state == "identified") and session.get("identify_required") is False
            and session.get("authentication") == "noise"
            and session.get("authentication_basis") == "native_session_upgrade_output"
            and "stream_authentication" not in session
            and session.get("path") == "direct" and session.get("direction") == "outbound"
            and session.get("muxer") == "/yamux/1.0.0", "official connect lacks retained authenticated session facts")
    connection = _single(raw["events"], "connection", connection_id=str(session["session_id"]))
    require(connection.get("peer_id") == remote and connection.get("authenticated") is True
            and connection.get("transport") == "tcp" and connection.get("security") == "/noise"
            and connection.get("muxer") == "/yamux/1.0.0"
            and connection.get("authentication_basis") == "native_session_upgrade_output"
            and _numeric_address(connection.get("remote_address"), remote) == _numeric_address(session.get("remote_address"), remote)
            == _numeric_address(ready["listen_addrs"][0], remote)
            and _numeric_address(connection.get("local_address"), raw["local_peer_id"])
            == _numeric_address(session.get("local_address"), raw["local_peer_id"]), "official session/socket owner changed")
    return connection


def _rpcs(raw, spec, remote, direction):
    result = []
    for event in raw["events"]:
        if event["kind"] != "rpc":
            continue
        peer = _rpc_peer(event)
        require(peer == remote, "PubSub RPC belongs to another peer")
        if raw["implementation"] == "forge":
            connections = [owner for owner in raw["events"] if owner["kind"] == "connection"
                           and owner.get("connection_id") == event.get("connection_id")]
            selected = [owner for owner in raw["events"] if owner["kind"] == "protocol"
                        and owner.get("connection_id") == event.get("connection_id")
                        and owner.get("stream_id") == event.get("stream_id")]
            # Diagnostics may observe the real session after the trace fires;
            # only stream selection, not periodic sampling, orders the RPC.
            require(len(connections) == len(selected) == 1 and connections[0].get("peer_id") == remote
                    and connections[0].get("authenticated") is True and connections[0].get("transport") == "tcp"
                    and connections[0].get("security") == "/noise" and connections[0].get("muxer") == "/yamux/1.0.0"
                    and connections[0].get("authentication_basis") == "native_session_upgrade_output"
                    and selected[0].get("peer_id") == remote and selected[0].get("protocol") == spec.protocol
                    and event.get("protocol") == spec.protocol and selected[0]["sequence"] <= event["sequence"]
                    and isinstance(event.get("stream_id"), str) and bool(event["stream_id"]),
                    "Forge RPC lacks its actual Noise/Yamux session and selected stream")
            _numeric_address(connections[0]["remote_address"], remote)
            _numeric_address(connections[0]["local_address"], raw["local_peer_id"])
        else:
            _owner(raw["events"], event, remote, spec.protocol, "tcp", None)
        decoded = validate_rpc_receipt(event.get("receipt"), spec.protocol, event.get("direction"))
        if event["direction"] == direction:
            result.append((event, decoded))
    return result


def delivery_ready(raw, spec, remote, token):
    active_actor(raw, spec, "replacement", token, raw.get("local_peer_id"))
    subscribed = _subscription_received(raw, remote, spec.protocol, "forge-pr11:" + token, "tcp",
                                       implementation=spec.donor, local_peer=raw["local_peer_id"])
    return subscribed and _mesh_is(raw, [remote])


def _input(raw, spec, remote, token, payload, event, direction):
    digest = hashlib.sha256(payload).hexdigest()
    return [(rpc, message) for rpc, decoded in _rpcs(raw, spec, remote, direction)
            for message in decoded["messages"] if message["payload_sha256"] == digest
            and message["payload_bytes"] == len(payload) and message["topic"] == "forge-pr11:" + token
            and message["author_hex"] == _peer(event["author_peer"]).hex()
            and message["seqno_hex"] == event["seqno_hex"] and bool(message["signature_hex"])]


def donor_marker_written(raw, spec, remote, token, delivered):
    active_actor(raw, spec, "replacement", token, delivered["author_peer"])
    return bool(_input(raw, spec, remote, token, probes(spec, token)[2].encode(), delivered, "write"))


def donor_delivery(raw, spec, remote, token, payload):
    _events(raw, spec.donor, token, "replacement", active=raw.get("finalized") is False)
    digest = hashlib.sha256(payload).hexdigest()
    matches = [event for event in raw["events"] if event["kind"] == "delivery"
               and event.get("payload_sha256") == digest]
    require(len(matches) <= 1, "duplicate native donor delivery")
    if not matches:
        return None
    event = matches[0]
    require(event.get("outcome") == "accept" and event.get("author_peer") == event.get("propagation_peer") == remote
            and event.get("topic") == "forge-pr11:" + token
            and isinstance(event.get("seqno_hex"), str) and re.fullmatch(r"[0-9a-f]{16}", event["seqno_hex"]),
            "donor delivery lacks exact accepted author/topic/sequence")
    if spec.donor == "go":
        require(event.get("committed") is True and event.get("phase") == "post_decision"
                and counter(event.get("payload_bytes"), len(payload), len(payload)), "Go callback is not native acceptance")
        delivered = [item for item in raw["events"] if item["kind"] == "subscription_delivery"
                     and item.get("payload_sha256") == digest and item.get("seqno_hex") == event["seqno_hex"]
                     and item.get("author_peer") == item.get("propagation_peer") == remote
                     and item.get("topic") == event["topic"] and item.get("payload_bytes") == len(payload)]
        if not delivered:
            return None
        require(len(delivered) == 1, "duplicate donor subscriber delivery")
    else:
        require(event.get("validation_commit") is True and event.get("report_message_validation_result") is True,
                "Rust delivery lacks actual native validation commit")
        committed = _single(raw["events"], "validation", payload_sha256=digest, seqno_hex=event["seqno_hex"])
        require(committed.get("validation_commit") is True and committed.get("outcome") == "accept",
                "Rust public Behaviour did not accept the message")
    wire = _input(raw, spec, remote, token, payload, event, "read")
    require(any(rpc["sequence"] < event["sequence"] for rpc, _ in wire), "donor delivery lacks preceding signed binary RPC")
    return event


def forge_delivery(raw, spec, remote, token, *, before=None):
    payload = probes(spec, token)[2].encode()
    digest = hashlib.sha256(payload).hexdigest()
    matches = [event for event in raw["events"] if event["kind"] == "subscription_delivery"
               and event.get("payload_sha256") == digest]
    require(len(matches) <= 1, "duplicate official subscription delivery")
    if not matches:
        return None
    event = matches[0]
    require(event.get("payload_hex") == payload.hex() and counter(event.get("bytes"), len(payload), len(payload))
            and event.get("contains_zero") is False and event.get("outcome") == "accept"
            and event.get("native_validation_commit") is False
            and event.get("author_peer") == event.get("propagation_peer") == remote
            and event.get("topic") == "forge-pr11:" + token
            and isinstance(event.get("seqno_hex"), str) and re.fullmatch(r"[0-9a-f]{16}", event["seqno_hex"]),
            "official callback is not the actual donor marker (or claims native commit)")
    baseline = _single((before or raw)["events"], "snapshot", label="before-donor")
    require(baseline["sequence"] < event["sequence"], "donor delivery precedes its actual counter baseline")
    snapshots = [sample for sample in raw["events"] if sample["kind"] == "snapshot" and sample["sequence"] > event["sequence"]]
    if not snapshots:
        return None
    sample = snapshots[-1]
    _snapshot_fields(sample)
    if not (sample["messages_accepted"] > baseline["messages_accepted"]
            and sample["messages_delivered"] > baseline["messages_delivered"]
            and sample["core"]["messages_received"] > baseline["core"]["messages_received"]
            and sample["core"]["messages_delivered"] > baseline["core"]["messages_delivered"]):
        return None
    if spec.policy == "typed-override":
        committed = [item for item in raw["events"] if item["kind"] == "validation"
                     and item.get("payload_sha256") == digest and item.get("seqno_hex") == event["seqno_hex"]]
        if not committed:
            return None
        require(len(committed) == 1 and committed[0].get("outcome") == "accept"
                and committed[0].get("author_peer") == committed[0].get("propagation_peer") == remote
                and committed[0].get("topic") == event["topic"], "native Forge validation was not accepted")
        require(any(rpc["sequence"] < committed[0]["sequence"] for rpc, _ in
                    _input(raw, spec, remote, token, payload, event, "read")), "native acceptance lacks actual signed input")
    return event


def validate_delivery(raws, spec, token):
    forge, donor = raws["victim"], raws["replacement"]
    local, remote = forge["local_peer_id"], donor["local_peer_id"]
    binary, oversized, marker = probes(spec, token)
    accepted = _single(forge["events"], "publish", payload_sha256=hashlib.sha256(binary).hexdigest())
    require(accepted.get("payload_hex") == binary.hex() and counter(accepted.get("bytes"), len(binary), len(binary))
            and accepted.get("contains_zero") is True and accepted.get("at_payload_limit") is True
            and accepted.get("author_peer") == local and accepted.get("topic") == "forge-pr11:" + token,
            "Forge did not publish the exact binary boundary")
    received = donor_delivery(donor, spec, local, token, binary)
    require(received is not None, "binary boundary never reached actual donor acceptance")
    require(received.get("seqno_hex") == accepted.get("seqno_hex"), "binary author sequence changed over the wire")
    rejected = _single(forge["events"], "publish_rejected")
    expected = ("forge.net.p2p", 11) if spec.policy == "typed-override" else ("forge.plugins.net.p2p.node", 7)
    require((rejected.get("error_category"), rejected.get("error_code")) == expected
            and type(rejected.get("error_code")) is int and rejected.get("expected_limit") == spec.payload_limit
            and rejected.get("payload") == {"bytes": len(oversized), "payload_sha256": hashlib.sha256(oversized).hexdigest(),
                                            "contains_zero": True}, "oversized boundary lacks its actual typed rejection")
    counters = _single(forge["events"], "publish_rejection_counters", command_sequence=rejected["command_sequence"])
    require(all(counter(counters.get(key)) for key in ("messages_published_before", "messages_published_after",
            "core_published_before", "core_published_after"))
            and counters["messages_published_before"] == counters["messages_published_after"] == 1
            and counters["core_published_before"] == counters["core_published_after"] == 1,
            "rejected publish changed actual native/consumer counters")
    require(not any(event.get("payload_sha256") == hashlib.sha256(oversized).hexdigest()
                    for event in donor["events"]), "oversized payload escaped to donor")
    after = _single(forge["events"], "snapshot", label="after-donor")
    delivered = forge_delivery({**forge, "events": forge["events"][:after["sequence"]]}, spec, remote, token)
    require(delivered is not None, "callback alone does not prove actual native acceptance/delivery")
    require(after["sequence"] > delivered["sequence"], "after-donor sample precedes delivery")
    published = _single(donor["events"], "publish", payload_sha256=hashlib.sha256(marker.encode()).hexdigest())
    require(published.get("error") is None and published.get("outcome", "published") == "published"
            and published.get("topic") == "forge-pr11:" + token, "donor publish failed")
    if spec.donor == "go":
        require(counter(published.get("payload_bytes"), len(marker.encode()), len(marker.encode())), "Go publish length differs")
    # Rust's publish event has no length field. Its native write below supplies
    # the exact actual length/hash; never invent a donor receipt field.
    require(_input(donor, spec, local, token, marker.encode(), delivered, "write"), "marker lacks native donor write")
    if spec.policy == "typed-override":
        for direction, payload, event in (("write", binary, accepted), ("read", marker.encode(), delivered)):
            own = _input(forge, spec, remote, token, payload, event, direction)
            peer = _input(donor, spec, local, token, payload, event, "read" if direction == "write" else "write")
            require(any(left[0]["receipt"]["framed_hex"] == right[0]["receipt"]["framed_hex"]
                        for left in own for right in peer), "typed snapshot lacks matching native RPC bytes in both actors")
        require(counter(after.get("application_score_calls"), 1) and any(
            score.get("peer_id") == remote and type(score.get("value")) in (int, float) and math.isfinite(score["value"])
            and type(score.get("app_specific_sample")) in (int, float) and score["app_specific_sample"] == 3
            and type(score.get("app_specific_score")) in (int, float) and score["app_specific_score"] == 6
            for score in after.get("peer_scores", [])), "no actual typed application score callback/sample/weight")
    else:
        require(after["application_score_calls"] == 0, "YAML-only claims an undeclared callback")


def forge_ack(raw, token, local, sequence, *, active):
    require(raw.get("actor") == "victim" and raw.get("case_token") == token and raw.get("local_peer_id") == local
            and raw.get("finalized") is (not active) and raw.get("joined") is (not active), "foreign Forge drain phase")
    matches = [event for event in raw["events"] if event["kind"] == "shutdown_prepared"]
    require(len(matches) <= 1, "duplicate Forge drain acknowledgement")
    if not matches:
        return None
    ack = matches[0]
    require(ack.get("source") == FORGE_SOURCES["shutdown_prepared"] and ack.get("actor") == "victim"
            and ack.get("case_token") == token and ack.get("local_peer_id") == local
            and counter(ack.get("command_sequence"), sequence, sequence) and ack.get("admission_closed") is True
            and counter(ack.get("pending_commands"), 0, 0) and counter(ack.get("active_handlers"), 0, 0)
            and raw.get("command_admission_closed") is True and raw.get("subscription_drained") is True,
            "Forge drain did not close admission and join callbacks")
    drained = _single(raw["events"], "snapshot", label="drained")
    require(drained["subscriptions"] == drained["active_handlers"] == 0 and drained["sequence"] < ack["sequence"],
            "Forge prepare lacks actual unsubscribed/drained snapshot")
    done = command_done(raw, sequence, "prepare_shutdown")
    if done is None:
        return None
    require(done["sequence"] > ack["sequence"], "Forge prepare completion precedes its drain")
    return ack


def _configuration(raw, spec, token):
    node = accepted_configuration(raw, spec, "listener", "")
    expected = {"enabled": True, "preferred-version": "v1-1", "signature-policy": "strict-sign",
        "sign-publishes": True, "max-message-size": 128, "flood-publish": True,
        "allowed-topics": ["forge-pr11:" + token]}
    require(_same_json(node.get("pubsub"), expected) and raw.get("declarative_source") == "forge.codec.yaml.read_document"
            and raw.get("native_tracer_installed") is (spec.policy == "typed-override"),
            "PubSub did not accept its exact YAML policy")
    settings = raw["accepted_configuration"]
    if spec.policy == "typed-override":
        typed = {"source": "node.configuration_api.set_pubsub_options", "full_native_snapshot": True,
            "preferred": "v1-2", "max_data_size": 96, "signatures": "strict-sign", "flood_publish": True,
            "tracer_installed": True, "application_score_callback_installed": True,
            "application_sample": 3.0, "application_weight": 2.0}
        actual = settings.get("typed_contribution", {})
        for key in ("application_sample", "application_weight"):
            require(type(actual.get(key)) in (int, float) and math.isfinite(actual[key]) and actual[key] == typed[key],
                    "typed application score inputs differ")
            typed[key] = actual[key]
        require(_same_json(actual, typed), "missing complete typed native contribution")
        competing = settings.get("node_reconfigure", {})
        require(competing.get("source") == "official-node-plugin.configure" and competing.get("component") == "plugins.net.p2p.node",
                "competing YAML did not use official node configure")
        document = settings["document"]
        expected_document = json.loads(json.dumps(document))
        expected_document["plugins"]["net"]["p2p"]["node"]["pubsub"].update({"preferred-version": "v1-0", "flood-publish": False})
        require(_same_json(competing.get("document"), expected_document), "typed test did not preserve competing YAML v1.0")
        frozen = _single(raw["events"], "configuration_frozen")
        require(frozen.get("phase") == "pubsub.post-start.configure" and frozen.get("error_category") == "forge.plugins.net.p2p.node"
                and counter(frozen.get("error_code"), 2, 2), "typed contribution was not frozen after start")
    else:
        require("typed_contribution" not in settings and "node_reconfigure" not in settings,
                "YAML-only fixture installed an undeclared typed override")


def _controls(record, spec, token):
    controls, endpoints = record["controls"], record["endpoints"]
    require(set(controls) == set(ACTORS), "missing/foreign actor controls")
    binary, oversized, marker = probes(spec, token)
    for actor in ACTORS:
        raw = endpoints[actor]["result"]
        commands = controls[actor]
        require(isinstance(commands, list) and 0 < len(commands) <= 64, "missing/bounded append-only controls")
        expected = ([{"kind": "connect", "peer_id": endpoints["replacement"]["result"]["local_peer_id"],
                      "address": endpoints["replacement"]["process"]["ready"]["listen_addrs"][0]},
                     {"kind": "publish_binary", "payload_hex": binary.hex()},
                     {"kind": "publish_oversized", "payload_hex": oversized.hex()},
                     {"kind": "sample", "label": "before-donor"}, {"kind": "sample", "label": "after-donor"}]
                    if actor == "victim" else [{"kind": "publish", "payload": marker}])
        expected.append({"kind": "prepare_shutdown", "actor": actor, "case_token": token, "local_peer_id": raw["local_peer_id"]})
        if actor == "replacement" and spec.donor == "go":
            expected.append({"kind": "quiesce_shutdown", "actor": actor, "case_token": token,
                "local_peer_id": raw["local_peer_id"], "prepare_ack_sequence": record["prepared"][actor]["ack_event_sequence"]})
        expected = [{"sequence": index, **operation} for index, operation in enumerate(expected, 1)]
        require(_same_json(commands, expected), "controls changed boundaries/targets/prepare identity or retried")
        for operation in commands:
            require(command_done(raw, operation["sequence"], operation["kind"]) is not None, "uncompleted owned control")
        completions = [event for event in raw["events"] if event["kind"] == "command_done"]
        require(len(completions) == len(commands), "unrequested control completion")
        if actor == "victim":
            require(counter(raw.get("commands_completed"), len(commands), len(commands)), "Forge command ledger differs")


def _barrier(record, spec, token):
    barrier, prepared, endpoints = record["shutdown_barrier"], record["prepared"], record["endpoints"]
    require(set(prepared) == set(ACTORS) and barrier.get("source") == "python.fixture.configured_pubsub_prepare_barrier",
            "missing independent prepare captures")
    operations = barrier.get("operations")
    expected = [("prepare_ack", actor) for actor in ACTORS]
    if spec.donor == "go":
        expected += [("quiesce_requested", "replacement"), ("quiesce_ack", "replacement")]
    expected += [("donor_stop_published", "replacement"), ("donor_joined", "replacement"),
                 ("forge_stop_published", "victim"), ("forge_joined", "victim")]
    require(isinstance(operations, list) and len(operations) == len(expected), "incomplete prepare/join barrier")
    for sequence, (row, (kind, actor)) in enumerate(zip(operations, expected), 1):
        endpoint = endpoints[actor]
        require(counter(row.get("sequence"), sequence, sequence) and row.get("kind") == kind and row.get("actor") == actor
                and row.get("case_token") == token and row.get("local_peer_id") == endpoint["result"]["local_peer_id"]
                and counter(row.get("pid"), endpoint["process"]["pid"], endpoint["process"]["pid"]),
                "stop/join order or actual actor identity changed")
    for row in operations[:2]:
        actor, capture = row["actor"], prepared[row["actor"]]
        raw, pid = endpoints[actor]["result"], endpoints[actor]["process"]["pid"]
        if actor == "replacement":
            prepared_snapshot(capture, row, raw, pid)
            ack = shutdown_ack(raw, spec.donor, actor, token, raw["local_peer_id"], row["command_sequence"])
            require(_donor_shutdown_event(raw, raw["events"])["sequence"] > ack["sequence"], "donor stopped before prepare")
        else:
            require(set(capture) == {"schema_version", "source", "actor", "case_token", "pid", "command_sequence",
                    "ack_event_sequence", "result"} and counter(capture.get("schema_version"), 1, 1)
                    and capture.get("source") == "python.fixture.native_prepare_snapshot" and capture.get("actor") == actor
                    and capture.get("case_token") == token and counter(capture.get("pid"), pid, pid)
                    and counter(capture.get("command_sequence"), row["command_sequence"], row["command_sequence"]),
                    "Forge prepare snapshot is not independently bound to its owner")
            active_actor(capture["result"], spec, actor, token, raw["local_peer_id"])
            ack = forge_ack(capture["result"], token, raw["local_peer_id"], row["command_sequence"], active=True)
            final = forge_ack(raw, token, raw["local_peer_id"], row["command_sequence"], active=False)
            require(ack is not None and _same_json(ack, final)
                    and _same_json(raw["events"][:len(capture["result"]["events"])], capture["result"]["events"]),
                    "Forge active drain/event prefix changed after owner join")
        require(counter(row.get("ack_event_sequence"), ack["sequence"], ack["sequence"])
                and counter(capture.get("ack_event_sequence"), ack["sequence"], ack["sequence"]), "prepare ACK reference changed")
    if spec.donor == "go":
        requested, row = operations[2:4]
        raw = endpoints["replacement"]["result"]
        ack = quiesce_ack(raw, "replacement", token, raw["local_peer_id"], endpoints["replacement"]["process"]["pid"],
                          row["command_sequence"], prepared["replacement"]["ack_event_sequence"])
        require(ack is not None and row.get("ack_event_sequence") == ack["sequence"]
                and requested.get("command_sequence") == row["command_sequence"]
                and requested.get("prepare_ack_sequence") == row.get("prepare_ack_sequence") == prepared["replacement"]["ack_event_sequence"],
                "Go stop lacks its actual independent quiesce join")


def validate_case(record, spec):
    try:
        require(record.get("suite") == "plugin-pubsub" and record.get("scenario_id") == spec.scenario_id
                and _same_json(record.get("spec"), vars(spec)) and record.get("operation_budget_seconds") == CASE_SECONDS,
                "wrong configured PubSub case/budget")
        token = record["case_token"]
        probes(spec, token)
        endpoints = record["endpoints"]
        require(set(endpoints) == set(ACTORS), "configured PubSub needs exactly two owned actors")
        peers, pids, raws = [], [], {}
        for actor in ACTORS:
            endpoint = endpoints[actor]
            process, raw = endpoint["process"], endpoint["result"]
            require(endpoint.get("implementation") == spec.implementation(actor) and counter(process.get("pid"), 1)
                    and _same_json(process.get("terminal_status"), {"exit_code": 0, "termination": "graceful"}),
                    "PubSub process was not successfully joined")
            local = ready_actor(process["ready"], spec, actor, token)
            require(raw.get("local_peer_id") == local, "terminal peer differs from actual readiness")
            if actor == "victim":
                _forge_events(raw, spec, token, local, active=False)
                _configuration(raw, spec, token)
            else:
                _events(raw, spec.donor, token, actor)
                _terminal_owners(raw)
            peers.append(local)
            pids.append(process["pid"])
            raws[actor] = raw
        require(len(set(peers)) == len(set(pids)) == 2, "PubSub actors shared identity/process")
        remote = peers[0]
        donor = raws["replacement"]
        connections = [event for event in donor["events"] if event["kind"] == "connection"]
        require(bool(connections) and all(event.get("peer_id") == remote for event in connections), "donor connected to a foreign peer")
        forge_connection = _forge_connection(raws["victim"], peers[1], endpoints["replacement"]["process"]["ready"])
        for connection in connections:
            _connection_owner(donor["events"], connection["connection_id"], remote, spec.donor, "tcp", None)
            require(_numeric_address(connection.get("remote_address"), remote)
                    == _numeric_address(forge_connection.get("local_address"), remote),
                    "donor remote socket differs from Forge local socket")
        _controls(record, spec, token)
        validate_delivery(raws, spec, token)
        _barrier(record, spec, token)
        return []
    except (ValueError, TypeError, KeyError, AttributeError, IndexError, OSError) as error:
        return ["plugin-pubsub evidence: " + str(error)]


def validate_terminal_sources(record, binary_paths, artifact_root):
    """Call AFTER runner-owned teardown; never substitute pre-stop inline data."""
    try:
        spec = Case(**record["spec"])
        root, workdirs, stores = Path(artifact_root).resolve(), [], []
        owners, attempts = record["owned_processes"], record["attempts"]
        require(len(owners) == len(attempts) == 2, "missing/extra PubSub process owners or retries")
        for actor in ACTORS:
            endpoint = record["endpoints"][actor]
            process = endpoint["process"]
            require(sum(_same_json(owner, process) for owner in owners) == 1, "actor is not its unique actual case owner")
            log = Path(process["log_file"]).resolve()
            log.relative_to(root)
            work = log.parent
            require(log.name == actor + ".log", "PubSub actor log rebound")
            argv = command(spec, actor, binary_paths[spec.implementation(actor)], work, record["case_token"])
            require(process.get("command") == argv, "PubSub command changed binary/version/backend/policy/token")
            matches = [attempt for attempt in attempts if attempt.get("kind") == actor]
            require(len(matches) == 1, "missing/duplicate PubSub attempt")
            attempt = matches[0]
            require(attempt.get("command") == argv and attempt.get("scenario_id") == spec.scenario_id
                    and counter(attempt.get("attempt_id"), 1, 1) and attempt.get("pid") == process["pid"]
                    and counter(attempt.get("exit_code"), 0, 0)
                    and _same_json(attempt.get("terminal_status"), process.get("terminal_status"))
                    and _same_json(attempt.get("outputs"), process.get("outputs"))
                    and attempt.get("requested_log_file") == attempt.get("log_file") == str(log), "actual PubSub attempt rebound")
            seconds = attempt.get("timeout_seconds")
            require(type(seconds) in (int, float) and math.isfinite(seconds) and 0 < seconds <= 20, "unbounded PubSub attempt")
            expected_budget = {"native_close_seconds": 10, "post_stop_seconds": 0, "scheduler_allowance_seconds": 10, "seconds": 20} \
                if actor == "victim" else {"native_close_seconds": 8, "post_stop_seconds": 0, "scheduler_allowance_seconds": 2, "seconds": 10}
            require(_same_json(process.get("stop_budget"), expected_budget), "PubSub join budget changed")
            for flag, suffix, inline in (("--result-file", ".json", endpoint["result"]),
                                          ("--ready-file", ".ready.json", process["ready"])):
                path = work / (actor + suffix)
                if flag == "--result-file":
                    require(endpoint.get("result_file") == str(path), "inline PubSub result path rebound")
                captures = [capture for capture in process.get("outputs", []) if capture.get("argument") == flag
                            and capture.get("path") == str(path) and capture.get("exists") is True]
                require(len(captures) == 1, "missing unique post-join " + flag + " capture")
                captured = Path(captures[0]["log_file"]).resolve()
                captured.relative_to(root)
                require(captured != path.resolve() and not path.is_symlink()
                        and path.stat().st_size <= 16 * 1024 * 1024 and captured.stat().st_size <= 16 * 1024 * 1024
                        and _same_json(load_json(path), inline) and _same_json(load_json(captured), inline),
                        "inline PubSub receipt differs from actual file/post-join capture")
            rows = [row for row in record["shutdown_barrier"]["operations"] if row["kind"] == "prepare_ack" and row["actor"] == actor]
            require(len(rows) == 1 and rows[0].get("evidence_file") == str(work / (actor + ".prepare.json")), "prepare file rebound")
            prepare = Path(rows[0]["evidence_file"])
            require(not prepare.is_symlink() and prepare.stat().st_size <= 16 * 1024 * 1024
                    and _same_json(load_json(prepare), record["prepared"][actor]), "independent active prepare capture changed")
            control = work / (actor + ".control")
            require(not control.is_symlink() and control.stat().st_size <= 1024 * 1024, "unbounded/aliased control file")
            contents = control.read_text()
            require(contents.endswith("\n"), "incomplete PubSub control tail")
            operations = [json.loads(line, object_pairs_hook=reject_duplicate_json_keys,
                                     parse_constant=lambda value: (_ for _ in ()).throw(ValueError("nonfinite JSON")))
                          for line in contents.splitlines()]
            require(_same_json(operations, record["controls"][actor]), "append-only actual controls differ from case ledger")
            store = (work / (actor + ".store")).resolve()
            store.relative_to(root)
            require(endpoint.get("store_existed_before_spawn") is False, "PubSub store was reused")
            if actor == "victim":
                settings = endpoint["result"]["accepted_configuration"]
                for document in (settings["document"], settings.get("node_reconfigure", {}).get("document", settings["document"])):
                    require(document["plugins"]["db"]["store"]["stores"][0]["path"] == str(store), "ObjectDB store escaped its actual actor")
            workdirs.append(work)
            stores.append(store)
        require(len(set(workdirs)) == 1 and len(set(stores)) == 2, "PubSub actors escaped case ownership or shared stores")
        return []
    except (ValueError, TypeError, KeyError, AttributeError, IndexError, OSError) as error:
        return ["plugin-pubsub terminal sources: " + str(error)]
