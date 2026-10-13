"""Causal PR12 validation of one completed native execution.

Source/binary provenance and the original/patched Rust shutdown split are separate
promotion gates. A successful result here is not production acceptance.
"""

from dataclasses import asdict
import hashlib
import json
import re

from pubsub_evidence import (
    PROFILES, _events, _message_pair, _score_observation, _shutdown_barrier, _single, _snapshot,
    _terminal_owners, _validate_native_graph, _validation, require, prepared_snapshot,
    EVENT_FIELDS, EVENT_LIMIT, RUST_WIRE_OWNER_FIELDS, SOURCES, _same_json, _framing_terminal,
    _rust_multistream_frame, _rust_non_pubsub_failure, _donor_shutdown_event,
    _owner, _rpc_peer,
)
from pubsub_extension_events import validate_extension_event
from pubsub_extension_cases import case_specs
from pubsub_extension_evidence import advertisements, native_rpcs, partial_exchange
from pubsub_wire import validate_rpc_receipt


def _extension_closed(raw, *, active=False):
    if raw["implementation"] == "rust":
        state = raw.get("extension_state")
        require(isinstance(state, dict) and state.get("admission_closed") is True
                and state.get("application_stopped") is (not active)
                and "error" in state and state["error"] is None
                and state.get("validation_hold_pending") is False
                and type(state.get("pending_hooks")) is int and state["pending_hooks"] == 0,
                "Rust extension application did not drain")
    else:
        fields = ("active_extension_validators",)
        if raw["implementation"] == "go":
            fields += ("pending_extension_work", "active_extension_publish_callbacks")
        elif raw["implementation"] == "forge":
            fields += ("active_extension_work",)
            require(raw.get("extension_drained") is True
                    and raw.get("partial_registration_active") is False
                    and "extension_drain_error" in raw and raw["extension_drain_error"] is None,
                    "Forge extension registration/work did not drain")
        require(raw.get("extension_admission_closed") is True
                and all(type(raw.get(field)) is int and raw[field] == 0 for field in fields),
                "extension callbacks/work did not drain")


def _graph(spec, peers):
    if spec.extension == "idontwant":
        graph = {"victim": ("offender", "replacement", "sink"), "offender": ("victim", "replacement"),
                 "replacement": ("victim", "offender"), "sink": ("victim",)}
    else:
        graph = {"victim": ("offender", "replacement"), "offender": ("victim",),
                 "replacement": ("victim", "sink"), "sink": ("replacement",)}
    return {role: {peers[other] for other in others} for role, others in graph.items()}


def _full(actors, events, source, target, payload, protocol, transport, fingerprint, *, author=None, after=0):
    author = source if author is None else author
    validation = _validation(events[target], payload, actors[source]["local_peer_id"], actors[author]["local_peer_id"],
                             "forge-pr11:" + actors[target]["case_token"], "accept", protocol, transport, fingerprint)
    sent, received = _message_pair(actors, events, source, target, validation, protocol, transport, fingerprint, after=after)
    delivery = _delivered(events[target], validation, actors[target]["implementation"])
    return {"write_sequence": sent["sequence"], "read_sequence": received["sequence"],
            "validation_sequence": validation["sequence"], "delivery_sequence": delivery["sequence"],
            "message_id": validation["message_id"]}


def _delivered(events, validation, implementation):
    identity = {key: validation[key] for key in
                ("propagation_peer", "author_peer", "topic", "message_id", "seqno_hex", "payload_sha256")}
    delivery = _single(events, "delivery", **identity)
    if implementation == "go":
        require(delivery is validation and delivery.get("committed") is True
                and delivery.get("phase") == "post_decision", "Go delivery lacks native committed observation")
    else:
        require(delivery["sequence"] > validation["sequence"], "application delivery did not follow accepted validation")
        if implementation == "rust":
            require(delivery.get("validation_commit") is True
                    and delivery.get("report_message_validation_result") is True,
                    "Rust delivery lacks successful native validation commit")
    return delivery


def _held_chain(events, forward, incoming, implementation):
    digest = forward["payload_sha256"]
    armed = _single(events, "validation_hold_armed", payload_sha256=digest)
    held = _single(events, "validation_held", payload_sha256=digest)
    identity = ("propagation_peer", "author_peer", "topic", "seqno_hex", "payload_sha256")
    require(all(held.get(key) == forward[key] for key in identity)
            and ("message_id" not in held or held["message_id"] == forward["message_id"])
            and held.get("committed") is False and held.get("command_sequence") == armed.get("command_sequence")
            and type(armed.get("command_sequence")) is int and armed["command_sequence"] > 0
            and armed["sequence"] < incoming < held["sequence"] < forward["sequence"],
            "hold does not bind the same preceding signed input and committed validation")
    if implementation == "rust":
        release = _single(events, "validation_released", payload_sha256=digest)
        requested = _single(events, "validation_release_requested", payload_sha256=digest)
        require(all(release.get(key) == forward[key] for key in (*identity, "message_id"))
                and release.get("hold_command_sequence") == armed["command_sequence"]
                and type(release.get("command_sequence")) is int
                and release["command_sequence"] > armed["command_sequence"]
                and release.get("validation_commit") is True and release.get("cancellation") is None
                and release.get("error") is None and forward["sequence"] < release["sequence"],
                "Rust release does not confirm the exact held native validation")
        require(all(requested.get(key) == release[key] for key in (*identity, "message_id", "hold_command_sequence", "command_sequence"))
                and requested.get("validation_commit") is False and requested.get("cancellation") is None
                and held["sequence"] < requested["sequence"] < forward["sequence"],
                "Rust hold lacks the exact release request before native commit")
        release = requested
    else:
        release = _single(events, "validation_release", held_observation_sequence=held["sequence"])
        resumed = _single(events, "validation_resumed", held_observation_sequence=held["sequence"])
        require(all(resumed.get(key) == forward[key] for key in identity)
                and ("message_id" not in resumed or resumed["message_id"] == forward["message_id"])
                and resumed.get("released") is True and resumed.get("committed") is False
                and "error" not in resumed and type(release.get("command_sequence")) is int
                and release["command_sequence"] > armed["command_sequence"]
                and held["sequence"] < release["sequence"] < resumed["sequence"] < forward["sequence"],
                "release/resume does not bind the exact held input before validation")
    return armed, held, release


def _before_release(sequence, armed, release):
    return armed["sequence"] < sequence < release["sequence"]


def _idontwant(actors, events, views, token, protocol, transport, fingerprint):
    graph = {"victim": ("offender", "replacement", "sink"), "offender": ("victim", "replacement"),
             "replacement": ("victim", "offender"), "sink": ("victim",)}
    for role, members in graph.items():
        before = _snapshot(events[role], "extension_before")
        _score_observation(events[role], before)
        require(set(before["mesh_peer_ids"]) == {actors[other]["local_peer_id"] for other in members},
                "IDONTWANT case lacks actual required native mesh edges")
    payload = "accept:" + token + ":idontwant:" + "x" * 1200
    digest = hashlib.sha256(payload.encode()).hexdigest()
    forwards, incoming, holds = {}, {}, {}
    for role in ("victim", "replacement"):
        forwards[role] = _validation(events[role], payload, actors["offender"]["local_peer_id"],
                                     actors["offender"]["local_peer_id"], "forge-pr11:" + token, "accept",
                                     protocol, transport, fingerprint)
        _, incoming[role] = _message_pair(actors, events, "offender", role, forwards[role], protocol, transport, fingerprint)
        holds[role] = _held_chain(events[role], forwards[role], incoming[role]["sequence"], actors[role]["implementation"])
        _delivered(events[role], forwards[role], actors[role]["implementation"])
    forward = forwards["victim"]
    require(forwards["replacement"]["message_id"] == forward["message_id"], "held actors validated different messages")
    paired = {}
    for source, target in (("replacement", "victim"), ("victim", "replacement")):
        source_armed, _, source_release = holds[source]
        target_armed, _, target_release = holds[target]
        sent = [rpc for rpc in views[source] if rpc.peer == actors[target]["local_peer_id"]
                and rpc.stream[-1] == "write" and _before_release(rpc.sequence, source_armed, source_release)
                and incoming[source]["sequence"] < rpc.sequence
                and any(forward["message_id"] in item["ids_hex"] for item in rpc.value["idontwant"])]
        received = [rpc for rpc in views[target] if rpc.peer == actors[source]["local_peer_id"]
                    and rpc.stream[-1] == "read" and _before_release(rpc.sequence, target_armed, target_release)
                    and any(forward["message_id"] in item["ids_hex"] for item in rpc.value["idontwant"])]
        pairs = [(left, right) for left in sent for right in received if left.frame == right.frame]
        require(bool(pairs), f"IDONTWANT {source}->{target} lacks matched native emission/reception before release")
        paired[source] = pairs[0]
        # Also reject a receiver-only duplicate: Rust excludes its originating peers independently of IDONTWANT.
        require(not any(message["payload_sha256"] == digest for rpc in views[source]
                        if rpc.peer == actors[target]["local_peer_id"] for message in rpc.value["messages"]),
                f"full message crossed the suppressed {source}/{target} edge")
    informed = _full(actors, events, "offender", "replacement", payload, protocol, transport, fingerprint)
    delivered = _full(actors, events, "victim", "sink", payload, protocol, transport, fingerprint,
                      author="offender", after=forward["sequence"])
    require(informed["message_id"] == delivered["message_id"] == forward["message_id"],
            "suppression/delivery evidence combines different signed messages")
    return {"held_sequence": holds["victim"][1]["sequence"],
            "replacement_held_sequence": holds["replacement"][1]["sequence"],
            "idontwant_write": paired["replacement"][0].sequence, "idontwant_read": paired["replacement"][1].sequence,
            "reverse_idontwant_write": paired["victim"][0].sequence, "reverse_idontwant_read": paired["victim"][1].sequence,
            "informed": informed, "sink": delivered}


def _reconstruction(events, exchange, peer, topic):
    reconstructed = _single(events, "partial_reconstructed", group_id_hex=exchange["group_id_hex"],
                            payload_hex=exchange["payload_hex"])
    require(reconstructed.get("payload_sha256") == hashlib.sha256(bytes.fromhex(exchange["payload_hex"])).hexdigest()
            and reconstructed.get("payload_bytes") == len(bytes.fromhex(exchange["payload_hex"])),
            "reconstruction digest/length differs from independently checked bytes")
    references = reconstructed.get("part_observation_sequences", reconstructed.get("part_incoming_sequences"))
    require(isinstance(references, list) and len(references) == 3
            and all(type(value) is int and 0 < value <= len(events) for value in references)
            and len(set(references)) == 3,
            "reconstruction lacks three distinct incoming application events")
    if "part_peer_ids" in reconstructed:
        require(reconstructed["part_peer_ids"] == [peer] * 3, "reconstruction peer references differ from incoming owners")
    if "parts_hex" in reconstructed:
        require(reconstructed["parts_hex"] == [part["encoded_hex"] for part in exchange["parts"]],
                "reconstruction parts differ from actual native bytes")
    for part, sequence in zip(exchange["parts"], references):
        require(type(sequence) is int and part["read_sequence"] < sequence < reconstructed["sequence"],
                "application part does not follow its actual native read")
        event = events[sequence - 1]
        require(event["kind"] == "partial_incoming" and event.get("peer_id") == peer
                and event.get("group_id_hex") == exchange["group_id_hex"]
                and event.get("body_present") is True and event.get("body_hex") == part["encoded_hex"],
                "application reconstruction borrowed offered bytes or a foreign incoming part")
        if event.get("source") in {"rust.libp2p.gossipsub.Event.Partial", "forge.pubsub.partial_handler"}:
            require(event.get("topic") == topic and event.get("group_bytes") == len(bytes.fromhex(exchange["group_id_hex"]))
                    and event.get("body_bytes") == len(bytes.fromhex(part["encoded_hex"])),
                    "reconstruction borrowed foreign topic or truncated diagnostic bytes")
    return reconstructed["sequence"]


def _partial_permissions(rpcs):
    """Only earlier local reads authorize a send; replacement revokes prior facts."""
    remote, incoming_seen, outgoing, previous = {}, set(), {}, 0
    for rpc in rpcs:
        require(rpc.sequence > previous, "capability observations are not in local causal order")
        previous = rpc.sequence
        key = (rpc.peer, *rpc.stream)
        if rpc.stream[-1] == "read":
            if key not in incoming_seen:
                incoming_seen.add(key)
                advertised = rpc.value.get("extensions")
                remote[rpc.peer] = (rpc.stream, advertised is not None and advertised.get("partial_messages") == 1, {})
            stream, enabled, topics = remote[rpc.peer]
            require(stream == rpc.stream, "retired incoming stream supplied replacement permissions")
            for row in rpc.value["subscriptions"]:
                if row["subscribe"]:
                    topics[row["topic"]] = (row["requests_partial"] == 1,
                                           row["requests_partial"] == 1 or row["supports_partial"] == 1)
                else:
                    topics.pop(row["topic"], None)
            continue
        if key not in outgoing:
            advertised = rpc.value.get("extensions")
            outgoing[key] = advertised is not None and advertised.get("partial_messages") == 1
        value = rpc.value.get("partial")
        if value is None:
            continue
        require(outgoing[key] and rpc.peer in remote, "partial write preceded local/remote extension support")
        stream, enabled, topics = remote[rpc.peer]
        request, support = topics.get(value["topic"], (False, False))
        require(stream[0] == rpc.stream[0] and enabled and support and (value["data_hex"] is None or request),
                "partial write borrowed missing, future, revoked or foreign-session topic capability")


def _topic_flags(spec, views, peers, graph, topic):
    roles = {peer: role for role, peer in peers.items()}
    for role, rpcs in views.items():
        _partial_permissions(rpcs)
        seen = set()
        for rpc in rpcs:
            sender = role if rpc.stream[-1] == "write" else roles[rpc.peer]
            receiver = roles[rpc.peer] if rpc.stream[-1] == "write" else role
            expected = spec.mode(sender) == "partial"
            for value in rpc.value["subscriptions"]:
                require(value["topic"] == topic, "extension actor subscribed to a foreign topic")
                if value["subscribe"] is not True:
                    continue
                require((value["requests_partial"] == 1) is expected
                        and ((value["requests_partial"] == 1 or value["supports_partial"] == 1) is expected),
                        "actual topic flags differ from partial/full-only role")
                seen.add((rpc.peer, rpc.stream[-1]))
            if rpc.value["partial"] is not None:
                require(spec.mode(sender) == spec.mode(receiver) == "partial",
                        "partial RPC reached a global-only/full-message subscriber")
        require(seen == {(peer, direction) for peer in graph[role] for direction in ("read", "write")},
                "extension case lacks actual topic subscriptions in both directions")


def _offmesh_exchange(events, views, peers, exchange, topic):
    for role, remote, end in (("victim", "replacement", max(part["read_sequence"] for part in exchange["parts"])),
                              ("replacement", "victim", max(part["write_sequence"] for part in exchange["parts"]))):
        before = _snapshot(events[role], "extension_before")["sequence"]
        first = exchange["metadata_read" if role == "victim" else "metadata_write"]
        require(before < first <= end, "partial transfer preceded its observed off-mesh setup")
        require(not any(value["topic"] == topic for rpc in views[role]
                        if rpc.peer == peers[remote] and before < rpc.sequence <= end for value in rpc.value["graft"]),
                "partial transfer used a GRAFTed path rather than the required off-mesh edge")
        for event in events[role]:
            if event["kind"] == "snapshot" and before < event["sequence"] <= end:
                _score_observation(events[role], event)
                require(peers[remote] not in event["mesh_peer_ids"], "partial peer entered mesh before transfer finished")


def _capture_identity(artifact):
    require(isinstance(artifact, dict) and type(artifact.get("schema_version")) is int
            and artifact["schema_version"] == 1 and artifact.get("suite") == "pubsub-extensions",
            "invalid extension capture schema")
    spec = next((value for value in case_specs() if artifact.get("case") == asdict(value)), None)
    require(spec is not None and artifact.get("scenario_id") == spec.identifier
            and artifact.get("acceptance_scenario_id") == spec.scenario
            and artifact.get("runner_scenario_id") == spec.runner_id, "extension capture differs from closed matrix")
    token = artifact.get("case_token")
    require(isinstance(token, str) and re.fullmatch(r"[a-f0-9]{32}", token), "foreign extension capture token")
    actors, processes = artifact.get("raw"), artifact.get("processes")
    require(isinstance(actors, dict) and isinstance(processes, dict)
            and set(actors) == set(processes) == {"victim", "offender", "replacement", "sink"},
            "extension capture lacks four independent actors")
    return spec, token, actors, processes


def needs_observer(spec):
    return spec in case_specs() and spec.profile == "native_quic" and "rust" in (spec.source, spec.destination)


def validate_capture(artifact, *, expected_fingerprint=None):
    spec, token, actors, processes = _capture_identity(artifact)
    require(artifact.get("errors") == [] and artifact.get("cleanup_errors") == [], "failed extension capture")
    private = spec.profile == "private_tcp_yamux"
    require((isinstance(expected_fingerprint, str) and bool(expected_fingerprint)) if private
            else expected_fingerprint is None, "extension capture profile/fingerprint mismatch")
    events, framing, peers = {}, {}, {}
    for role, raw in actors.items():
        require(raw.get("extension") == spec.mode(role)
                and raw.get("requests_partial") is (spec.mode(role) == "partial"), "extension actor mode differs")
        framing[role] = {}
        events[role] = _events(raw, spec.implementation(role), token, role, cleanup_framing=framing[role])
        _terminal_owners(raw)
        _extension_closed(raw)
        process = processes[role]
        require(type(process.get("pid")) is int and process["pid"] > 0
                and type(process.get("returncode")) is int and process["returncode"] == 0
                and process.get("forced_termination") is False, "extension process did not actually join")
        peers[role] = raw["local_peer_id"]
    require(len(set(peers.values())) == 4 and len({process["pid"] for process in processes.values()}) == 4,
            "extension capture reuses identity/process")
    _shutdown_barrier(artifact, actors, events)
    return _validate_traffic(artifact, spec, actors, events, framing, expected_fingerprint)


def _validate_traffic(artifact, spec, actors, events, framing, expected_fingerprint):
    token = artifact["case_token"]
    protocol, transport, topic = "/meshsub/" + spec.version + ".0", PROFILES[spec.profile], "forge-pr11:" + token
    peers = {role: raw["local_peer_id"] for role, raw in actors.items()}
    graph = _graph(spec, peers)
    _validate_native_graph(artifact, actors, events, framing, graph, protocol, transport, expected_fingerprint)
    views = {role: native_rpcs(values, spec.implementation(role), protocol, transport, expected_fingerprint)
             for role, values in events.items()}
    if spec.extension == "idontwant":
        return {"idontwant": _idontwant(actors, events, views, token, protocol, transport, expected_fingerprint)}
    first = {role: len(advertisements(rpcs, True, {peer: True for peer in graph[role]}))
             for role, rpcs in views.items()}
    _topic_flags(spec, views, peers, graph, topic)
    for role, remote in (("victim", "offender"), ("offender", "victim"), ("replacement", "sink"), ("sink", "replacement")):
        before = _snapshot(events[role], "extension_before")
        _score_observation(events[role], before)
        require(before["mesh_peer_ids"] == [peers[remote]], "extension case did not start from actual two-pair mesh")
    if spec.extension == "advertisement":
        return {"first_rpcs": first,
                "forward": _full(actors, events, "offender", "victim", "accept:" + token + ":advertisement",
                                 protocol, transport, expected_fingerprint),
                "reverse": _full(actors, events, "sink", "replacement", "accept:" + token + ":advertisement-reverse",
                                 protocol, transport, expected_fingerprint)}
    exchange = partial_exchange(views["replacement"], views["victim"], peers["replacement"], peers["victim"], topic, token)
    _offmesh_exchange(events, views, peers, exchange, topic)
    reconstructed = _reconstruction(events["victim"], exchange, peers["replacement"], topic)
    return {"first_rpcs": first, "partial_exchange": exchange, "reconstructed": reconstructed,
            "full_fallback": _full(actors, events, "offender", "victim", "accept:" + token + ":full-fallback",
                                   protocol, transport, expected_fingerprint)}


def _rust_original_tail(raw, captured):
    """Bound a retained opaque post-ACK failure; never classify it as normal."""
    events, prefix = raw.get("events"), captured["events"]
    require(isinstance(events, list) and len(prefix) < len(events) <= EVENT_LIMIT
            and len(json.dumps(raw).encode()) <= 16 * 1024 * 1024,
            "original Rust diagnostic trace is missing/unbounded")
    failures, previous = [], 0
    for sequence, event in enumerate(events, 1):
        require(isinstance(event, dict) and type(event.get("sequence")) is int and event["sequence"] == sequence
                and type(event.get("mono_ns")) is int and event["mono_ns"] >= previous and event["mono_ns"] > 0
                and len(json.dumps(event).encode()) <= 64 * 1024, "invalid original diagnostic sequence/clock/bound")
        previous = event["mono_ns"]
        kind = event.get("kind")
        require(isinstance(kind, str) and (validate_extension_event(raw, event)
                or kind in SOURCES["rust"] and event.get("source") in SOURCES["rust"][kind]),
                "original diagnostic has foreign event authority")
        if kind != "native_io_error":
            _framing_terminal(event, "rust")
            if kind == "native_multistream_frame":
                _rust_multistream_frame(event)
            if kind == "non_pubsub_negotiation_failure":
                _rust_non_pubsub_failure(raw, event, events)
            if kind == "rpc":
                protocol = "/meshsub/" + raw["version"] + ".0"
                _owner(events, event, _rpc_peer(event), protocol, "quic", None)
                validate_rpc_receipt(event.get("receipt"), protocol, event.get("direction"))
            if kind == "expected_native_close":
                require(event.get("prepared") is True and sequence > len(prefix)
                        and event.get("typed_cause") in {"quinn_application_closed_0", "quinn_locally_closed"}
                        and event.get("raw_os_error") is None
                        and event.get("operation") in {"stream_read", "stream_write", "stream_flush", "stream_close",
                                                        "muxer_inbound", "muxer_outbound", "muxer_poll", "muxer_close"}
                        and event.get("swarm_connection_id") == event.get("connection_id")
                        and event.get("remote_peer_id") == event.get("peer_id")
                        and type(event.get("connection_trace_id")) is int,
                        "original tail has a foreign/nonzero typed close")
                owned = [owner for owner in prefix if owner.get("kind") == "connection"
                         and all(_same_json(owner.get(key), event.get(key)) for key in
                                 ("connection_trace_id", "connection_id", "peer_id", "endpoint", "native_stack"))]
                require(len(owned) == 1, "original typed close borrows another native owner")
                stream = event.get("stream_trace_id")
                require((type(stream) is int and 1 <= stream <= 64
                         and event.get("stream_id") == f'{event["connection_trace_id"]}:{stream}')
                        if event["operation"].startswith("stream_")
                        else stream is None and event.get("stream_id") is None,
                        "original typed close has foreign stream/muxer scope")
            require(kind != "native_terminal_state", "QUIC original cannot borrow Yamux terminal state")
            if sequence > len(prefix) and kind == "command_done":
                require(False, "original admitted a command after completed Prepare")
            continue
        require(set(event) == EVENT_FIELDS | RUST_WIRE_OWNER_FIELDS | {
                    "operation", "io_kind", "raw_os_error", "typed_cause", "prepared", "message"}
                and sequence > len(prefix) and event["prepared"] is True
                and event["operation"] in {"muxer_inbound", "muxer_outbound", "muxer_poll", "muxer_close"}
                and event["typed_cause"] == "quic_connection_cause_unavailable"
                and event["io_kind"] == "Other" and event["raw_os_error"] is None
                and event["stream_trace_id"] is None and event["stream_id"] is None
                and isinstance(event["message"], str) and 0 < len(event["message"]) <= 512,
                "original failure is not a bounded post-ACK opaque QUIC muxer diagnostic")
        owners = [owner for owner in prefix if owner.get("kind") == "connection"
                  and type(event["connection_trace_id"]) is int
                  and owner.get("connection_trace_id") == event["connection_trace_id"]
                  and owner.get("connection_id") == event["connection_id"]
                  and owner.get("peer_id") == event["peer_id"]]
        require(len(owners) == 1 and isinstance(event["connection_id"], str) and bool(event["connection_id"])
                and 1 <= event["connection_trace_id"] <= 16
                and event["connection_id"] == event["swarm_connection_id"]
                and event["remote_peer_id"] == event["peer_id"]
                and owners[0].get("authenticated") is True and owners[0].get("transport") == "quic"
                and _same_json(event["endpoint"], owners[0].get("endpoint"))
                and _same_json(event["native_stack"], owners[0].get("native_stack")),
                "original opaque failure lacks its own authenticated pre-ACK carrier")
        disposed = [owner for owner in raw["native_close"]["connections"]
                    if type(owner.get("connection_trace_id")) is int
                    and owner["connection_trace_id"] == event["connection_trace_id"]
                    and owner.get("connection_id") == event["connection_id"]
                    and owner.get("peer_id") == event["peer_id"]]
        require(len(disposed) == 1 and disposed[0].get("dropped") is True
                and (disposed[0].get("close_returned") is True or disposed[0].get("native_terminal_observed") is True),
                "original opaque owner did not actually terminate/drop/join")
        failures.append(event)
    # Exact preservation of the emitter's first sticky diagnostic, not matching
    # error text to infer a transport cause or to turn an error into success.
    require(failures and raw.get("status") == "error" and raw.get("error") == failures[0]["message"],
            "original first failure is not the retained opaque diagnostic")
    shutdown = _donor_shutdown_event(raw, events)
    require(shutdown["sequence"] > len(prefix), "original host close preceded its active Prepare snapshot")


def validate_active_capture(artifact, snapshots):
    """PR12 original traffic from actual pre-stop snapshots, with no shutdown claim."""
    spec, token, final, processes = _capture_identity(artifact)
    require(needs_observer(spec) and isinstance(snapshots, dict) and set(snapshots) == set(final),
            "active extension proof outside exact six Rust QUIC cases")
    barrier = artifact.get("shutdown_barrier")
    require(isinstance(barrier, dict) and set(barrier) == {"source", "operations"}
            and barrier["source"] == "python.fixture.all_actor_prepare_barrier"
            and isinstance(barrier["operations"], list), "missing original Prepare barrier")
    rows = barrier["operations"][:4]
    require(len(rows) == 4 and all(isinstance(row, dict) for row in rows)
            and {row.get("actor") for row in rows} == set(final), "missing original four distinct Prepare ACKs")
    actors, events, framing, final_events = {}, {}, {}, {}
    failed_role = None
    for sequence, row in enumerate(rows, 1):
        role, raw, process = row["actor"], final[row["actor"]], processes[row["actor"]]
        implementation = spec.implementation(role)
        require(set(row) == {"sequence", "kind", "actor", "case_token", "local_peer_id", "command_sequence",
                             "ack_event_sequence", "evidence_file"}
                and type(row["sequence"]) is int and row["sequence"] == sequence and row["kind"] == "prepare_ack"
                and row["case_token"] == token and row["local_peer_id"] == raw.get("local_peer_id"),
                "original Prepare row has foreign identity/order")
        require(type(process.get("pid")) is int and process["pid"] > 0
                and type(process.get("returncode")) is int
                and process["returncode"] in ((0, 1) if implementation == "rust" else (0,))
                and process.get("forced_termination") is False
                and raw.get("finalized") is True and raw.get("joined") is True and raw.get("overflow") is False,
                "original actor did not actually finish/join")
        captured = prepared_snapshot(snapshots[role], row, raw, process["pid"], terminal_success=False)
        require(all(_same_json(raw.get(field), captured.get(field)) for field in
                    ("schema_version", "implementation", "actor", "case_token", "local_peer_id", "version",
                     "extension", "requests_partial")) and raw.get("extension") == spec.mode(role)
                and raw.get("requests_partial") is (spec.mode(role) == "partial"), "original actor/capture mode differs")
        actors[role], framing[role] = captured, {}
        events[role] = _events(captured, implementation, token, role, cleanup_framing=framing[role], active=True)
        _extension_closed(captured, active=True)
        _extension_closed(raw)
        _terminal_owners(raw)
        if raw.get("error") is not None:
            require(implementation == "rust" and process["returncode"] == 1, "non-Rust original terminal failure")
            _rust_original_tail(raw, captured)
            failed_role = role
        else:
            require(process["returncode"] == 0, "original exit failure without native diagnostic")
            final_events[role] = _events(raw, implementation, token, role)
    require(len({raw["local_peer_id"] for raw in actors.values()}) == 4
            and len({process["pid"] for process in processes.values()}) == 4, "original actors reuse identity/PID")
    if failed_role is None:
        require(artifact.get("errors") == [] and artifact.get("cleanup_errors") == [], "original execution failed")
        _shutdown_barrier(artifact, final, final_events)
    else:
        # A typed process exit receipt explains only runner lifecycle diagnostics,
        # never the unknown native cause. No arbitrary cleanup failure is excused.
        process = processes[failed_role]
        diagnostic = f"pid={process['pid']}; log={process['log_file']}: terminal exit code 1"
        require(artifact.get("cleanup_errors") in ([diagnostic], [diagnostic, diagnostic])
                and artifact.get("errors") == [f"RuntimeError: {failed_role} donor shutdown did not actually close/join successfully: {[diagnostic]}"],
                "original has unrelated execution/cleanup failures")
        stop = {"sequence": 5, "kind": "stop_requested", "actor": failed_role, "case_token": token,
                "local_peer_id": final[failed_role]["local_peer_id"]}
        require(_same_json(barrier["operations"], [*rows, stop]), "original failure is not after its actual Stop")
    return _validate_traffic(artifact, spec, actors, events, framing, None)
