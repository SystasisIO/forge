"""Synthetic checker regressions only; these are NOT interoperability evidence."""

import copy
from copy import deepcopy
import hashlib
import unittest

from pubsub_evidence import (
    AUTHENTICATION, GO_QUIC_SOURCES, PROFILES, _events, _go_quic_operations, _go_quic_owner, _owner, _rpc_peer, _same_json, _shutdown_barrier,
    prepared_snapshot, quiesce_ack, shutdown_ack, validate_case,
)
from rust_upgrade_evidence import BASE58, _peer
from test_pubsub_wire import field, receipt


def peer_id(index):
    data = b"\x12\x20" + bytes([index]) * 32
    number, out = int.from_bytes(data, "big"), ""
    while number:
        number, digit = divmod(number, 58)
        out = BASE58[digit] + out
    return out


def synthetic_yamux_terminal(*, private=False, version012=False):
    """Synthetic verifier input, not a native Yamux run or acceptance artifact."""
    source, token = "rust.libp2p.passive-upgraded-stream-io", "a" * 32
    boundary = "libp2p_yamux_public_into_io"
    basis = ("native_PNET_then_Noise_authenticated_Yamux_upgrade_output" if private
             else "native_Noise_authenticated_Yamux_upgrade_output")
    stack = {"transport": "tcp", "security": "/noise", "muxer": "/yamux/1.0.0",
             "authentication_basis": basis, "muxer_error_boundary": boundary}
    endpoint = {"direction": "outbound", "upgrade_role": "outbound", "remote_address": "/ip4/127.0.0.1/tcp/41000"}
    owner = {"connection_trace_id": 1, "connection_id": "unit-connection", "swarm_connection_id": "unit-connection",
             "peer_id": peer_id(2), "remote_peer_id": peer_id(2), "stream_trace_id": None, "stream_id": None,
             "endpoint": endpoint, "native_stack": stack}
    connection = {**deepcopy(owner), **stack, "sequence": 1, "mono_ns": 1, "kind": "connection", "source": source,
                  "authenticated": True, "remote_address": endpoint["remote_address"],
                  "owner_basis": "exact_native_transport_output_muxer"}
    if private:
        connection.update(pnet_verified=True, pnet_fingerprint="b" * 64)
    ack = {"sequence": 2, "mono_ns": 2, "kind": "shutdown_prepared", "source": "rust.fixture.prepare_shutdown",
           "command_sequence": 1, "actor": "victim", "case_token": token, "local_peer_id": peer_id(1),
           "admission_closed": True, "pending_commands": 0}
    done = {"sequence": 3, "mono_ns": 3, "kind": "command_done",
            "source": "rust.fixture.control-native-operation-completion", "command_sequence": 1,
            "command_kind": "prepare_shutdown", "status": "ok"}
    state = {**deepcopy(owner), "sequence": 4, "mono_ns": 4, "kind": "native_terminal_state", "source": source,
             "operation": "muxer_inbound", "io_kind": "Other", "raw_os_error": None,
             "typed_cause": "yamux012_closed" if version012 else "yamux013_closed", "prepared": True,
             "prepare_ack_sequence": 2, "error_boundary": boundary, "closure_reason": "unknown",
             "message": "connection is closed"}
    return {"schema_version": 1, "implementation": "rust", "case_token": token, "actor": "victim",
            "local_peer_id": peer_id(1), "finalized": True, "joined": True, "overflow": False, "error": None,
            "events": [connection, ack, done, state],
            "native_close": {"live_muxers": 0, "live_streams": 0, "connections": [
                {"connection_trace_id": 1, "connection_id": "unit-connection", "peer_id": peer_id(2),
                 "dropped": True, "close_returned": False, "native_terminal_observed": True}]},
            "task_join": {"fixture_owned_tasks_joined": True, "overflow": False, "errors": []}}


def synthetic_case(*, lower_quic=True):
    token = "a" * 32
    topic = "forge-pr11:" + token
    actors, processes = {}, {}
    names = ("victim", "offender", "replacement", "sink")
    peers = {name: peer_id(index) for index, name in enumerate(names, 1)}
    for index, name in enumerate(names, 1):
        actors[name] = {"schema_version": 1, "implementation": "forge" if name == "victim" else "go",
                        "case_token": token, "actor": name, "local_peer_id": peers[name],
                        "finalized": True, "joined": True, "overflow": False, "error": None, "events": []}
        if name != "victim":
            actors[name].update(pid=index, host_close_returned=True, active_stream_handlers_and_io=0,
                                active_fixture_workers=0, active_callbacks=0)
        processes[name] = {"pid": index, "returncode": 0, "forced_termination": False,
                           "terminal_status": {"exit_code": 0, "termination": "graceful"}}

    def event(name, kind, **fields):
        native = actors[name]["implementation"]
        values = actors[name]["events"]
        source = {"forge": {"connection": "forge.node.authenticated_session", "protocol": "forge.pubsub.native_stream",
                             "rpc": "forge.pubsub.native_stream", "snapshot": "forge.pubsub.peer_score_snapshot",
                             "validation": "forge.pubsub.committed_validation", "delivery": "forge.pubsub.committed_validation",
                             "publish": "forge.node.async_publish"},
                  "go": {"connection": "go.network.Conn.authenticated_output", "protocol": "go.network.Stream.Protocol",
                          "rpc": "go.pubsub.native_stream." + fields.get("direction", "read"),
                          "snapshot": "go.pubsub.RawTracer_mesh_and_score_inspection",
                          "score": "go.pubsub.WithPeerScoreInspect",
                          "validation": "go.pubsub.ValidatorEx", "rejection": "go.pubsub.RawTracer.RejectMessage",
                          "delivery": "go.pubsub.RawTracer.DeliverMessage"}}
        values.append({"sequence": len(values) + 1, "mono_ns": len(values) + 1, "kind": kind,
                       "source": source[native][kind], **fields})
        return values[-1]

    def owners(left, right):
        for name, remote in ((left, right), (right, left)):
            event(name, "connection", peer_id=peers[remote], connection_id=remote,
                  authenticated=True, remote_address="/ip4/127.0.0.1/udp/1/quic-v1",
                  authentication_basis="native_session_upgrade_output" if name == "victim"
                  else "native_quic_TLS_InterceptSecured_and_RemotePublicKey", transport="quic",
                  security="/tls/1.0.0", muxer="quic")
            event(name, "protocol", peer_id=peers[remote], connection_id=remote,
                  stream_id=remote, protocol="/meshsub/1.1.0")

    def rpc(left, right, body):
        for name, remote, direction in ((left, right, "write"), (right, left, "read")):
            event(name, "rpc", peer_id=peers[remote], connection_id=remote, stream_id=remote,
                  protocol="/meshsub/1.1.0", direction=direction, receipt=receipt(body, direction))

    def sample(name, label, mesh, score=0, invalid=0):
        scores = [{"peer_id": peers["offender"], "value": score, "invalid_deliveries": invalid}]
        observation = {}
        if actors[name]["implementation"] == "go":
            native = event(name, "score", peer_scores=deepcopy(scores))
            observation["score_observation_sequence"] = native["sequence"]
        event(name, "snapshot", label=label, mesh_peer_ids=[peers[other] for other in mesh],
              peer_scores=scores, **observation)

    def message(left, right, author, payload, outcome, sequence):
        seqno = sequence.to_bytes(8, "big")
        body = field(2, field(1, _peer(peers[author])) + field(2, payload.encode())
                     + field(3, seqno) + field(4, topic.encode()) + field(5, b"synthetic-signature"))
        rpc(left, right, body)
        detail = {"propagation_peer": peers[left], "author_peer": peers[author], "topic": topic,
                  "message_id": (_peer(peers[author]) + seqno).hex(), "seqno_hex": seqno.hex(),
                  "payload_sha256": hashlib.sha256(payload.encode()).hexdigest()}
        if actors[right]["implementation"] == "go":
            event(right, "delivery" if outcome == "accept" else "rejection", outcome=outcome,
                  committed=True, phase="post_decision", **detail)
        else:
            event(right, "validation", outcome=outcome, **detail)
            if outcome == "accept":
                event(right, "delivery", **detail)

    owners("victim", "offender")
    owners("replacement", "sink")
    owners("victim", "replacement")
    rpc("victim", "replacement", field(1, field(1, 1) + field(2, topic.encode())))
    rpc("replacement", "victim", field(1, field(1, 1) + field(2, topic.encode())))
    sample("victim", "before", ["offender"])
    sample("offender", "before", ["victim"])
    sample("replacement", "before", ["sink"])
    sample("sink", "before", ["replacement"])
    message("offender", "victim", "offender", "accept:" + token + ":gossip", "accept", 1)
    gossip_id = _peer(peers["offender"]) + (1).to_bytes(8, "big")
    rpc("victim", "replacement", field(3, field(1, field(1, topic.encode()) + field(2, gossip_id))))
    rpc("replacement", "victim", field(3, field(2, field(1, gossip_id))))
    message("victim", "replacement", "offender", "accept:" + token + ":gossip", "accept", 1)
    message("replacement", "sink", "offender", "accept:" + token + ":gossip", "accept", 1)
    message("offender", "victim", "offender", "ignore:" + token + ":one", "ignore", 2)
    sample("victim", "ignored", ["offender"])
    message("offender", "victim", "offender", "reject:" + token + ":one", "reject", 3)
    sample("victim", "penalized", ["offender"], -100, 1)
    rpc("victim", "offender", field(3, field(4, field(1, topic.encode()) + field(3, 1))))
    rpc("victim", "replacement", field(3, field(3, field(1, topic.encode()))))
    sample("victim", "repaired", ["replacement"], -100, 1)
    sample("replacement", "repaired", ["victim", "sink"])
    good = "accept:" + token + ":repaired"
    event("victim", "publish", topic=topic, payload_sha256=hashlib.sha256(good.encode()).hexdigest())
    message("victim", "replacement", "victim", good, "accept", 4)
    message("replacement", "sink", "victim", good, "accept", 4)
    barrier = {"source": "python.fixture.all_actor_prepare_barrier", "operations": []}
    for name in names:
        native, values = actors[name]["implementation"], actors[name]["events"]
        ack_sequence = len(values) + 1
        values.append({"sequence": ack_sequence, "mono_ns": ack_sequence, "kind": "shutdown_prepared",
                       "source": native + ".fixture.prepare_shutdown", "command_sequence": 1,
                       "actor": name, "case_token": token, "local_peer_id": peers[name],
                       "admission_closed": True, "pending_commands": 0})
        values.append({"sequence": ack_sequence + 1, "mono_ns": ack_sequence + 1, "kind": "command_done",
                       "source": "forge.fixture.native_operation" if native == "forge" else "go.fixture.append_only_control",
                       "command_sequence": 1, "command_kind": "prepare_shutdown", "status": "ok"})
        barrier["operations"].append({"sequence": len(barrier["operations"]) + 1, "kind": "prepare_ack",
                                      "actor": name, "case_token": token, "local_peer_id": peers[name],
                                      "command_sequence": 1, "ack_event_sequence": ack_sequence,
                                      "evidence_file": f"/synthetic/{name}.prepare-result.json"})
    for name in names[1:]:
        prepared = next(row for row in barrier["operations"][:4] if row["actor"] == name)
        barrier["operations"].append({"sequence": len(barrier["operations"]) + 1, "kind": "quiesce_requested",
                                      "actor": name, "case_token": token, "local_peer_id": peers[name],
                                      "pid": processes[name]["pid"], "command_sequence": 2,
                                      "prepare_ack_sequence": prepared["ack_event_sequence"]})
    for name in names[1:]:
        request = next(row for row in barrier["operations"] if row["kind"] == "quiesce_requested" and row["actor"] == name)
        values = actors[name]["events"]
        sequence = len(values) + 1
        values.append({"sequence": sequence, "mono_ns": sequence, "kind": "shutdown_quiesced",
                       "source": "go.fixture.owned_pubsub_quiesce", "actor": name, "case_token": token,
                       "local_peer_id": peers[name], "pid": processes[name]["pid"], "command_sequence": 2,
                       "prepare_ack_sequence": request["prepare_ack_sequence"], "native_admission_closed": True,
                       "pubsub_callback_admission_closed": True,
                       "pubsub_context_cancelled": True, "subscriber_context_cancelled": True,
                       "active_stream_handlers_and_io": 0, "active_pubsub_streams": 0,
                       "active_fixture_workers": 0, "active_callbacks": 0,
                       "joined_scope": "fixture_subscriber_admitted_stream_IO_framing_pending_terminal_and_observer_callbacks"})
        values.append({"sequence": sequence + 1, "mono_ns": sequence + 1, "kind": "command_done",
                       "source": "go.fixture.append_only_control", "command_sequence": 2,
                       "command_kind": "quiesce_shutdown", "status": "ok"})
        barrier["operations"].append({**request, "sequence": len(barrier["operations"]) + 1, "kind": "quiesce_ack",
                                      "quiesce_event_sequence": sequence})
    for name in names[1:]:
        barrier["operations"].append({"sequence": len(barrier["operations"]) + 1, "kind": "stop_requested",
                                      "actor": name, "case_token": token, "local_peer_id": peers[name]})
    for name in names[1:]:
        values = actors[name]["events"]
        shutdown_sequence = len(values) + 1
        values.append({"sequence": shutdown_sequence, "mono_ns": shutdown_sequence, "kind": "shutdown",
                       "source": "go.fixture.owned_context_cancel_and_drain", "context_cancelled": True,
                       "joined": True, "host_close_returned": True,
                       "active_stream_handlers_and_io": 0, "active_fixture_workers": 0})
        barrier["operations"].append({"sequence": len(barrier["operations"]) + 1, "kind": "donor_joined",
                                      "actor": name, "case_token": token, "local_peer_id": peers[name],
                                      "pid": processes[name]["pid"], "shutdown_event_sequence": shutdown_sequence})
    barrier["operations"].append({"sequence": len(barrier["operations"]) + 1, "kind": "stop_requested",
                                  "actor": "victim", "case_token": token, "local_peer_id": peers["victim"]})
    artifact = {"schema_version": 1, "suite": "pubsub-scoring", "case_token": token,
            "case": {"source": "go", "destination": "forge", "version": "1.1", "profile": "native_quic"},
            "roles": {role: role for role in names[1:]}, "raw": actors, "processes": processes,
            "errors": [], "cleanup_errors": [], "shutdown_barrier": barrier}
    if lower_quic:
        return synthetic_lower_quic(artifact)
    artifact["case"]["profile"] = "native_tcp_yamux"
    for raw in actors.values():
        for value in raw["events"]:
            if value["kind"] == "connection":
                value.update(transport="tcp", security="/noise", muxer="/yamux/1.0.0",
                             remote_address="/ip4/127.0.0.1/tcp/1",
                             authentication_basis=AUTHENTICATION[raw["implementation"]]["tcp"])
    return artifact


def synthetic_lower_quic(artifact):
    """Synthetic lower receipts only, never native crypto or live acceptance evidence."""
    live = {"done": False, "cause_type": None, "error_code": None, "remote": None, "native_stream_id": None, "error": None}
    for raw in artifact["raw"].values():
        if raw["implementation"] != "go":
            continue
        original, raw["events"] = raw["events"], []
        events, owners, original_refs = raw["events"], {}, {}

        def append(kind, fields, source=None):
            value = {"sequence": len(events) + 1, "mono_ns": len(events) + 1, "kind": kind,
                     "source": GO_QUIC_SOURCES[kind] if source is None else source, **deepcopy(fields)}
            events.append(value)
            return value

        for old in original:
            if old["kind"] == "shutdown":
                continue
            fields = {key: deepcopy(value) for key, value in old.items() if key not in {"sequence", "mono_ns", "kind", "source"}}
            if old["kind"] == "connection":
                native_id = "synthetic-native-" + raw["actor"] + "-" + old["connection_id"]
                secured = {"source": "go.quic.transport.InterceptSecured", "native_connection_id": native_id,
                           "native_connection_basis": "network.Conn.As(**quic.Conn)", "observed_mono_ns": 1,
                           "local_peer_id": raw["local_peer_id"], "remote_peer_id": old["peer_id"],
                           "local_address": "/ip4/127.0.0.1/udp/2/quic-v1", "remote_address": old["remote_address"],
                           "direction": "outbound", "security_role": "client"}
                connection = append("native_quic_connection", {
                    **{key: value for key, value in secured.items() if key not in
                       {"source", "native_connection_basis", "observed_mono_ns", "direction", "security_role"}},
                    "remote_public_key_sha256": "b" * 64, "native_connection_basis": "CapableConn.As(**quic.Conn)",
                    "authentication_basis": "same_native_quic_Conn_and_InterceptSecured_capable_output",
                    "secured_callback": secured, "prepare_ack_sequence": 0, "connection_context_at_capable_return": live})
                owners[old["connection_id"]] = {"connection": connection}
                fields.update(local_peer_id=raw["local_peer_id"], remote_peer_id=old["peer_id"], local_address=secured["local_address"],
                              remote_public_key_sha256="b" * 64, quic_security=secured,
                              lower_connection_receipt_sequence=connection["sequence"],
                              lower_stream_binding_basis="same_native_CapableConn_not_Swarm_stream_ID_mapping")
            elif old["kind"] == "protocol":
                owner = owners[old["connection_id"]]
                connection = owner["connection"]
                base = {"native_connection_id": connection["native_connection_id"], "native_stream_id": 0,
                        "connection_receipt_sequence": connection["sequence"], "native_stream_receipt_sequence": 0,
                        "remote_peer_id": old["peer_id"], "stream_direction": "Outbound", "protocol": "",
                        "owner_basis": "same_native_CapableConn_returned_MuxedStream"}
                stream = append("native_quic_stream", {**base, "prepare_ack_sequence": 0, "send_context_at_stream_return": live,
                                "native_call_begin_prepare_ack_sequence": 0,
                                "native_call_begin_observation_basis": "published_Prepare_ACK_before_native_CapableConn_call"})
                base["native_stream_receipt_sequence"] = stream["sequence"]
                references = []
                for side, token in (("write", "/multistream/1.0.0"), ("read", "/multistream/1.0.0"),
                                    ("write", old["protocol"]), ("read", old["protocol"])):
                    body = (token + "\n").encode()
                    frame = bytes([len(body)]) + body
                    observed = {"framed_hex": frame.hex(), side: {"framed_bytes": len(frame), "frames": 1,
                                "framed_sha256": hashlib.sha256(frame).hexdigest(), "complete_frames": True,
                                "invalid_or_over_limit": False}}
                    references.append(append("multistream_frame", {**base, "protocol": token, "direction": side,
                                                                    "receipt": observed})["sequence"])
                base["protocol"] = old["protocol"]
                owner["base"], owner["stream"] = base, stream
                append("protocol", {**base, "negotiation_frame_sequences": references})
            elif old["kind"] == "rpc":
                base = owners[old["connection_id"]]["base"]
                new = append("rpc", {**base, "direction": old["direction"], "receipt": old["receipt"]},
                             "go.quic.native_stream." + old["direction"])
                original_refs[old["sequence"]] = new
                continue
            elif old["kind"] == "shutdown_prepared":
                fields["native_quic_context_snapshots"] = [
                    {"native_connection_id": owner["base"]["native_connection_id"], "native_stream_id": 0,
                     "send_context": deepcopy(live), "connection_context": deepcopy(live)} for owner in owners.values()]
            new = append(old["kind"], fields, old["source"])
            original_refs[old["sequence"]] = new
        for value in events:
            if "score_observation_sequence" in value:
                value["score_observation_sequence"] = original_refs[value["score_observation_sequence"]]["sequence"]
            if value["kind"] == "shutdown_quiesced":
                value["prepare_ack_sequence"] = original_refs[value["prepare_ack_sequence"]]["sequence"]
        ack = next(value for value in events if value["kind"] == "shutdown_prepared")
        row = next(row for row in artifact["shutdown_barrier"]["operations"][:4] if row["actor"] == raw["actor"])
        row["ack_event_sequence"] = ack["sequence"]
        for row in artifact["shutdown_barrier"]["operations"]:
            if row["actor"] == raw["actor"] and row["kind"] in {"quiesce_requested", "quiesce_ack"}:
                row["prepare_ack_sequence"] = ack["sequence"]
                if row["kind"] == "quiesce_ack":
                    row["quiesce_event_sequence"] = original_refs[row["quiesce_event_sequence"]]["sequence"]
        for owner in owners.values():
            base = owner["base"]
            send = {"done": True, "cause_type": "*quic.StreamError", "error_code": 0, "remote": False,
                    "native_stream_id": 0, "error": "synthetic local reset"}
            reset = append("native_stream_operation", {**base, "operation": "stream_reset", "started_order": 20,
                           "returned_order": 21, "prepare_ack_sequence": ack["sequence"], "send_context": send,
                           "error": None, "error_type": None, "outcome": "ok", "typed_cause": "none", "requested_reset_code": None})
            connection = owner["connection"]
            fields = {key: connection[key] for key in ("native_connection_id", "local_peer_id", "remote_peer_id",
                      "local_address", "remote_address", "remote_public_key_sha256", "native_connection_basis")}
            append("native_quic_connection_context", {**fields, "connection_receipt_sequence": connection["sequence"],
                   "context": {"done": True, "cause_type": "*qerr.ApplicationError", "error_code": 0, "remote": False,
                               "native_stream_id": None, "error": "synthetic local application close"},
                   "prepare_ack_sequence": ack["sequence"], "context_at_prepare": live, "prepare_baseline_present": True})
            append("native_quic_framing_finalized", {**base, "framing_clean": True, "io_joined": True,
                   "read_finalized": True, "write_finalized": True, "negotiation_complete": True,
                   "pending_read_frame_bytes": 0, "pending_write_frame_bytes": 0, "native_owner_disposed": True,
                   "owner_disposal_receipt_sequence": reset["sequence"]})
        append("native_quic_join", {"active_native_calls": 0, "observed_connections": len(owners), "observed_streams": len(owners),
               "joined_scope": "fixture_lower_stream_IO_and_operations_not_all_donor_goroutines"})
        shutdown = next(value for value in original if value["kind"] == "shutdown")
        joined = append("shutdown", {key: value for key, value in shutdown.items()
                        if key not in {"sequence", "mono_ns", "kind", "source"}}, shutdown["source"])
        row = next(row for row in artifact["shutdown_barrier"]["operations"]
                   if row["kind"] == "donor_joined" and row["actor"] == raw["actor"])
        row["shutdown_event_sequence"] = joined["sequence"]
    return artifact


class PubSubEvidenceTests(unittest.TestCase):
    def lower_terminal_events(self, artifact, outcome, *, direction="read", late_reset=False):
        """Synthetic immutable operation/finalization receipts, not native outcome fabrication."""
        raw = artifact["raw"]["sink"]
        events = raw["events"]
        ack = next(value for value in events if value["kind"] == "shutdown_prepared")
        original_reset = next(value for value in events if value["kind"] == "native_stream_operation")
        original_context = next(value for value in events if value["kind"] == "native_quic_connection_context")
        original_join = next(value for value in events if value["kind"] == "native_quic_join")
        original_shutdown = next(value for value in events if value["kind"] == "shutdown")
        base = {key: original_reset[key] for key in ("native_connection_id", "native_stream_id", "connection_receipt_sequence",
                "native_stream_receipt_sequence", "remote_peer_id", "stream_direction", "protocol", "owner_basis")}
        events[:] = events[:original_reset["sequence"] - 1]
        live = deepcopy(ack["native_quic_context_snapshots"][0]["send_context"])
        peer = outcome == "peer_zero_reset_pending" or outcome == "remote_close"
        send = {"done": True, "cause_type": "*quic.StreamError", "error_code": 0, "remote": peer,
                "native_stream_id": base["native_stream_id"], "error": "synthetic typed zero reset"}

        def append(kind, fields, source=None):
            value = {**deepcopy(fields), "kind": kind, "source": GO_QUIC_SOURCES[kind] if source is None else source,
                     "sequence": len(events) + 1, "mono_ns": len(events) + 1}
            events.append(value)
            return value

        reset_fields = {**base, "operation": "stream_reset", "started_order": 20, "returned_order": 23,
                        "prepare_ack_sequence": ack["sequence"], "send_context": send, "requested_reset_code": None,
                        "outcome": "ok", "typed_cause": "none", "error": None, "error_type": None}
        opaque_close = outcome in {"remote_close", "repeat_close"}
        if opaque_close:
            reset_fields.update(started_order=20 if outcome == "repeat_close" else 24,
                                returned_order=21 if outcome == "repeat_close" else 25)
            close_context = append("native_quic_send_context", {**base, "started_order": 22, "returned_order": 23,
                                   "prepare_ack_sequence": ack["sequence"], "send_context": send,
                                   "observation_basis": "same_lower_delegate_Context_at_actual_Close_RETURN"})
            fields = {**base, "operation": "stream_close", "started_order": 22, "returned_order": 23,
                      "prepare_ack_sequence": ack["sequence"], "send_context": send, "requested_reset_code": None,
                      "error": "synthetic opaque native Close", "error_type": "*errors.errorString", "typed_cause": "opaque",
                      "outcome": "native_send_reset_close_pending", "native_close_error_classification": "opaque_unwrapped_native_error",
                      "send_context_receipt_sequence": close_context["sequence"], "send_context_at_prepare": live,
                      "terminal_state_cause": "observed_native_send_context_not_graceful_close"}
            if outcome == "repeat_close" and not late_reset:
                reset = append("native_stream_operation", reset_fields)
                observed = append("native_stream_operation", fields)
            else:
                observed = append("native_stream_operation", fields)
                reset = append("native_stream_operation", reset_fields)
        else:
            fields = {**base, "operation": "stream_" + direction, "started_order": 10, "returned_order": 22,
                      "prepare_ack_sequence": 0, "send_context": send if direction == "write" else live,
                      "error": "synthetic direct native I/O cause", "error_type": "*network.StreamError",
                      "outcome": outcome, "typed_cause": "libp2p_quic_stream_error", "direction": direction,
                      "successful_prefix_bytes": 0, "pending_frame_bytes": 0, "terminal_prepare_ack_sequence": ack["sequence"],
                      "prepare_baseline_present": True, "send_context_at_prepare": live, "connection_context_at_prepare": live,
                      "error_code": 0, "remote": peer, "transport_error_type": "*quic.StreamError",
                      "transport_error_code": 0, "transport_error_remote": peer,
                      "transport_native_stream_id": base["native_stream_id"], "same_send_context_cause": direction == "write"}
            if outcome == "peer_zero_reset_pending":
                fields["peer_reset_reason"] = "unknown"
                reset_fields.update(started_order=24, returned_order=25)
            if outcome == "connection_application_close_pending":
                fields.update(error_type="*network.ConnError", typed_cause="libp2p_quic_application_error",
                              transport_error_type="*qerr.ApplicationError")
                fields.pop("transport_native_stream_id")
                fields.pop("same_send_context_cause")
                reset_fields.update(started_order=24, returned_order=25)
            observed = append("stream_io_terminal", fields)
            reset = append("native_stream_operation", reset_fields)
        context = append("native_quic_connection_context", {key: value for key, value in original_context.items()
                         if key not in {"sequence", "mono_ns", "source", "kind"}})
        framing = append("native_quic_framing_finalized", {**base, "framing_clean": True, "io_joined": True,
                         "read_finalized": True, "write_finalized": True, "negotiation_complete": True,
                         "pending_read_frame_bytes": 0, "pending_write_frame_bytes": 0,
                         "native_owner_disposed": True, "owner_disposal_receipt_sequence": reset["sequence"]})
        fields = {**base, "operation_receipt_sequence": observed["sequence"], "framing_receipt_sequence": framing["sequence"],
                  "owner_disposal_receipt_sequence": reset["sequence"], "accepted": True, "terminal_outcome": observed["outcome"],
                  "prepare_ack_sequence": ack["sequence"], "connection_context_receipt_sequence": context["sequence"],
                  "send_context_receipt_sequence": observed.get("send_context_receipt_sequence", 0)}
        if outcome in {"owned_reset_pending", "owned_read_terminal_pending", "repeat_close"}:
            fields["owned_terminal_receipt_sequence"] = reset["sequence"]
        if outcome == "connection_application_close_pending":
            fields["same_native_connection_context_cause"] = True
        final = append("native_quic_terminal_finalized", fields)
        append("native_quic_join", {key: value for key, value in original_join.items()
               if key not in {"sequence", "mono_ns", "kind", "source"}})
        shutdown = append("shutdown", {key: value for key, value in original_shutdown.items()
                          if key not in {"sequence", "mono_ns", "kind", "source"}}, original_shutdown["source"])
        row = next(row for row in artifact["shutdown_barrier"]["operations"]
                   if row["kind"] == "donor_joined" and row["actor"] == raw["actor"])
        row["shutdown_event_sequence"] = shutdown["sequence"]
        return reset, observed, context, framing, final

    def lower_repeat_close_read_events(self, artifact, *, delayed_publication=False):
        read = self.lower_terminal_events(deepcopy(artifact), "owned_read_terminal_pending")[1]
        reset, close, _, framing, close_final = self.lower_terminal_events(
            artifact, "repeat_close", late_reset=delayed_publication)
        read["returned_order"] = 24
        events = artifact["raw"]["sink"]["events"]
        self.insert_sink_events(artifact, events.index(close if delayed_publication else framing), [read])
        read_final = deepcopy(close_final)
        read_final.update(operation_receipt_sequence=read["sequence"], terminal_outcome=read["outcome"],
                          send_context_receipt_sequence=0)
        self.insert_sink_events(artifact, events.index(close_final), [read_final])
        return reset, close, read, framing, close_final, read_final

    def test_lower_quic_read_retains_exact_reset_after_independently_finalized_repeat_close(self):
        for delayed in (False, True):
            artifact = synthetic_case()
            reset, close, read, _, close_final, read_final = self.lower_repeat_close_read_events(
                artifact, delayed_publication=delayed)
            before = deepcopy(artifact)
            with self.subTest(delayed_publication=delayed):
                validate_case(artifact)
                self.assertTrue(_same_json(before, artifact))
                self.assertLess(reset["returned_order"], close["started_order"])
                self.assertLess(close["started_order"], close["returned_order"])
                self.assertLess(close["returned_order"], read["returned_order"])
                self.assertEqual(read["prepare_ack_sequence"], 0)
                self.assertEqual(close_final["owned_terminal_receipt_sequence"], reset["sequence"])
                self.assertEqual(read_final["owned_terminal_receipt_sequence"], reset["sequence"])
                self.assertEqual(read_final["send_context_receipt_sequence"], 0)
                if delayed:
                    self.assertLess(read["sequence"], close["sequence"])
                    self.assertLess(close["sequence"], reset["sequence"])

    def test_lower_quic_repeat_close_read_chain_keeps_independent_error_and_join_guards(self):
        modes = ("other_reset", "foreign_owner", "foreign_protocol", "foreign_ack", "boolean_ack", "missing_close_final",
                 "duplicate_close_final", "missing_close_context", "wrong_final_context", "nonzero_context", "foreign_context",
                 "errno_close", "wrapped_close", "joined_close", "sentinel_close", "reset_pending", "reset_failure",
                 "close_after_read", "new_reset", "new_reset_with_error", "new_terminal", "read_residue", "framing_residue",
                 "failed_disposal", "half_disposal", "unfinished_framing", "unfinished_native_io", "sticky")
        for mode in modes:
            artifact = synthetic_case()
            reset, close, read, framing, close_final, read_final = self.lower_repeat_close_read_events(
                artifact, delayed_publication=True)
            events = artifact["raw"]["sink"]["events"]
            send_context = events[close["send_context_receipt_sequence"] - 1]
            mutations = {
                "foreign_owner": lambda: close_final.update(native_connection_id="foreign-native-connection"),
                "foreign_protocol": lambda: close_final.update(protocol="/meshsub/1.0.0"),
                "foreign_ack": lambda: close_final.update(prepare_ack_sequence=0),
                "boolean_ack": lambda: close_final.update(prepare_ack_sequence=True),
                "wrong_final_context": lambda: close_final.update(send_context_receipt_sequence=0),
                "nonzero_context": lambda: close["send_context"].update(error_code=7),
                "foreign_context": lambda: close["send_context"].update(native_stream_id=4),
                "errno_close": lambda: close.update(error_type="syscall.Errno", native_close_error_classification="public_typed_error"),
                "wrapped_close": lambda: close.update(error_type="*fmt.wrapError", native_close_error_classification="public_typed_error"),
                "joined_close": lambda: close.update(error_type="*errors.joinError", native_close_error_classification="public_typed_error"),
                "sentinel_close": lambda: close.update(native_close_error_classification="known_sentinel_error"),
                "reset_pending": lambda: reset.update(returned_order=0),
                "reset_failure": lambda: reset.update(outcome="error", error="native Reset failed", error_type="syscall.Errno", typed_cause="opaque"),
                "read_residue": lambda: read.update(pending_frame_bytes=1),
                "framing_residue": lambda: framing.update(pending_read_frame_bytes=1),
                "failed_disposal": lambda: framing.update(owner_disposal_receipt_sequence=close["sequence"]),
                "half_disposal": lambda: reset.update(operation="stream_close_read"),
                "unfinished_framing": lambda: framing.update(io_joined=False),
                "unfinished_native_io": lambda: next(event for event in events if event["kind"] == "native_quic_join").update(active_native_calls=1),
                "sticky": lambda: artifact["raw"]["sink"].update(error="earlier unrelated native failure"),
            }
            if mode in mutations:
                mutations[mode]()
            elif mode in {"missing_close_final", "missing_close_context"}:
                # Keep indexing intact: an extra valid connection snapshot cannot replace the missing proof.
                replacement = deepcopy(events[read_final["connection_context_receipt_sequence"] - 1])
                target = close_final if mode == "missing_close_final" else send_context
                replacement.update(sequence=target["sequence"], mono_ns=target["mono_ns"])
                target.clear()
                target.update(replacement)
            elif mode == "duplicate_close_final":
                self.insert_sink_events(artifact, events.index(close_final), [deepcopy(close_final)])
            elif mode == "close_after_read":
                close["returned_order"] = send_context["returned_order"] = 25
            else:
                extra = deepcopy(reset)
                extra.update(started_order=18 if mode == "other_reset" else 24,
                             returned_order=19 if mode == "other_reset" else 25)
                if mode == "new_reset_with_error":
                    extra.update(operation="stream_reset_with_error", requested_reset_code=0)
                elif mode == "new_terminal":
                    extra["operation"] = "stream_close_read"
                self.insert_sink_events(artifact, events.index(framing), [extra])
                if mode == "other_reset":
                    close_final["owned_terminal_receipt_sequence"] = extra["sequence"]
                else:
                    read["returned_order"] = 26
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_lower_quic_real_shaped_owners_and_terminal_links_are_immutable(self):
        validate_case(synthetic_case())
        for outcome, direction in (("owned_reset_pending", "write"), ("owned_read_terminal_pending", "read"),
                                   ("peer_zero_reset_pending", "read"), ("peer_zero_reset_pending", "write"),
                                   ("connection_application_close_pending", "read"), ("remote_close", "read"),
                                   ("repeat_close", "read")):
            for late in (False, True):
                artifact = synthetic_case()
                _, observed, _, _, _ = self.lower_terminal_events(artifact, outcome, direction=direction, late_reset=late)
                before = deepcopy(artifact)
                with self.subTest(outcome=outcome, direction=direction, late=late):
                    validate_case(artifact)
                    self.assertTrue(_same_json(before, artifact))
                    if not outcome.endswith("close"):
                        self.assertEqual(observed["prepare_ack_sequence"], 0)
                    self.assertNotIn("stream_id", observed)
                    self.assertNotIn("connection_id", observed)

    def lower_diagnostic_events(self, artifact, mode="prefix"):
        """Actual-shaped unselected RETURN snapshots; never selected RPC authority."""
        events = artifact["raw"]["sink"]["events"]
        ack = next(value for value in events if value["kind"] == "shutdown_prepared")
        reset = next(value for value in events if value["kind"] == "native_stream_operation")
        selected = next(value for value in events if value["kind"] == "protocol")
        live = deepcopy(ack["native_quic_context_snapshots"][0]["send_context"])
        base = {key: reset[key] for key in ("native_connection_id", "native_stream_id", "connection_receipt_sequence",
                "native_stream_receipt_sequence", "remote_peer_id", "stream_direction", "protocol", "owner_basis")}

        def capture(data):
            return {"bytes": len(data), "hex": data.hex(), "sha256": hashlib.sha256(data).hexdigest(), "capture_complete": True}

        def direction(header, paused, token):
            return {"header_seen": header, "paused": paused, "token": token,
                    "partial_frame": capture(b""), "lazy_tail": capture(b"")}

        late = mode in {"late_close", "late_reset", "late_proposal"}
        refs = selected["negotiation_frame_sequences"][:] if not late else []
        snapshot = {"capture_complete": True, "selected_protocol": base["protocol"] if not late else "",
                    "proposal": base["protocol"] if not late else "", "reply": base["protocol"] if not late else "",
                    "touched_pubsub": not late, "parser_failed": False, "proposals": 0 if late else 1,
                    "frame_sequences": refs, "snapshot_basis": "parser_state_after_successful_prefix_observation",
                    "read": direction(not late, not late, base["protocol"] if not late else ""),
                    "write": direction(not late, not late, base["protocol"] if not late else "")}
        common = {"started_order": 1, "returned_order": 2, "prepare_ack_sequence": ack["sequence"] if late else 0,
                  "send_context": live, "observation_phase": "unselected_at_native_return", "protocol_at_native_return": ""}
        passive = {"terminal_prepare_ack_sequence": ack["sequence"] if late else 0,
                   "prepare_snapshot_ack_sequence": ack["sequence"] if late else 0,
                   "prepare_baseline_present": False, "stream_prepare_baseline_present": False,
                   "connection_prepare_baseline_present": late, "send_context_at_prepare": live,
                   "connection_context_at_prepare": live, "connection_context": live,
                   "context_observation_basis": "send_context_after_seal_and_parent_context_sealed_at_native_return", "negotiation_snapshot": snapshot,
                   "error": None, "error_type": None, "outcome": "ok", "typed_cause": "none"}
        added, stream = [], None
        if late:
            proposed_protocol = base["protocol"]
            base.update(protocol="", native_stream_id=4)
            stream = {"kind": "native_quic_stream", "source": GO_QUIC_SOURCES["native_quic_stream"],
                      **base, "native_stream_receipt_sequence": 0, "prepare_ack_sequence": ack["sequence"],
                      "native_call_begin_prepare_ack_sequence": ack["sequence"],
                      "native_call_begin_observation_basis": "published_Prepare_ACK_before_native_CapableConn_call",
                      "send_context_at_stream_return": deepcopy(live)}
            added.append(stream)
            proposal_frames = []
            if mode == "late_proposal":
                for reference in selected["negotiation_frame_sequences"][:3]:
                    frame = {**deepcopy(events[reference - 1]), **base, "protocol": events[reference - 1]["protocol"]}
                    proposal_frames.append(frame)
                    added.append(frame)
                snapshot.update(proposal=proposed_protocol, proposals=1, touched_pubsub=True,
                                read=direction(True, False, ""), write=direction(True, True, proposed_protocol))
                tail = bytes.fromhex(receipt(field(1, field(1, 1) + field(2, b"synthetic-subscribe")), "write")["framed_hex"])
                snapshot["write"]["lazy_tail"] = capture(tail)
            operation = "stream_close" if mode == "late_close" else "stream_reset"
            if mode == "late_close":
                context = {"kind": "native_quic_send_context", "source": GO_QUIC_SOURCES["native_quic_send_context"],
                           **base, **common, "connection_context": deepcopy(live),
                           "observation_basis": "same_lower_delegate_Context_at_actual_Close_RETURN"}
                added.append(context)
            observed = {"kind": "native_stream_operation", "source": GO_QUIC_SOURCES["native_stream_operation"],
                        **base, **common, **passive, "operation": operation, "requested_reset_code": None}
            if mode == "late_close":
                observed.update(native_close_error_classification="none", send_context_receipt_sequence=0)
            index = events.index(reset)
        else:
            prefix = bytes.fromhex(events[refs[-1] - 1]["receipt"]["framed_hex"])
            snapshot["successful_prefix"] = capture(prefix)
            observed = {"kind": "native_quic_negotiation_io_return", "source": GO_QUIC_SOURCES["native_quic_negotiation_io_return"],
                        **base, **common, **passive, "operation": "stream_read", "direction": "read",
                        "requested_bytes": len(prefix), "successful_prefix_bytes": len(prefix), "successful_prefix_valid": True}
            index = events.index(selected) + 1
        added.append(observed)
        self.insert_sink_events(artifact, index, added)
        if stream is not None:
            for value in added[1:]:
                value["native_stream_receipt_sequence"] = stream["sequence"]
            next(value for value in events if value["kind"] == "native_quic_join")["observed_streams"] += 1
            if mode == "late_proposal":
                snapshot["frame_sequences"] = [value["sequence"] for value in proposal_frames]
        if mode == "late_close":
            observed["send_context_receipt_sequence"] = context["sequence"]
        return observed

    def empty_late_diagnostic_events(self, artifact, *, close=False):
        disposal = self.lower_diagnostic_events(artifact, "late_close" if close else "late_reset")
        raw, events = artifact["raw"]["sink"], artifact["raw"]["sink"]["events"]
        stream = events[disposal["native_stream_receipt_sequence"] - 1]
        parent = next(value for value in events if value["kind"] == "native_quic_connection_context")
        parent["context"]["remote"] = True
        disposal.update(started_order=5, returned_order=6, connection_context=deepcopy(parent["context"]),
                        send_context=deepcopy(parent["context"]))
        if close:
            context = events[disposal["send_context_receipt_sequence"] - 1]
            context.update(started_order=5, returned_order=6, connection_context=deepcopy(parent["context"]),
                           send_context=deepcopy(parent["context"]))
        fields = {key: deepcopy(value) for key, value in disposal.items() if key not in {
            "kind", "source", "requested_reset_code", "native_close_error_classification", "send_context_receipt_sequence"}}
        records = []
        for direction, start in (("read", 1), ("write", 3)):
            original = {**deepcopy(fields), "kind": "native_quic_negotiation_io_return",
                        "source": GO_QUIC_SOURCES["native_quic_negotiation_io_return"], "operation": "stream_" + direction,
                        "direction": direction, "started_order": start, "returned_order": start + 1,
                        "requested_bytes": 1, "successful_prefix_bytes": 0, "successful_prefix_valid": True,
                        "outcome": "error", "error": "synthetic direct native AppClosed0", "error_type": "*network.ConnError",
                        "typed_cause": "libp2p_quic_application_error", "error_code": 0, "remote": True,
                        "transport_error_type": "*qerr.ApplicationError", "transport_error_code": 0,
                        "transport_error_remote": True, "same_native_connection_context_cause": True}
            original["negotiation_snapshot"]["successful_prefix"] = self.cleanup_capture(b"")
            records.append(original)
        self.insert_sink_events(artifact, events.index(parent) + 1, records)
        return stream, parent, *records, disposal, next(value for value in events if value["kind"] == "native_quic_join")

    def test_empty_late_native_errors_require_full_disposal_and_export_no_pubsub_authority(self):
        for close in (False, True):
            artifact = synthetic_case()
            stream, parent, read, write, disposal, joined = self.empty_late_diagnostic_events(artifact, close=close)
            raw = artifact["raw"]["sink"]
            before = deepcopy(artifact)
            with self.subTest(close=close):
                validate_case(artifact)
                self.assertEqual(_go_quic_operations(raw, raw["events"], terminal=True), {})
                self.assertTrue(_same_json(before, artifact))
                self.assertLess(disposal["sequence"], read["sequence"])
                for original in (read, write):
                    self.assertEqual(original["outcome"], "error")
                    self.assertEqual(original["successful_prefix_bytes"], 0)
                    self.assertIs(original["stream_prepare_baseline_present"], False)
                    self.assertIs(original["prepare_baseline_present"], False)
                    self.assertEqual(original["prepare_ack_sequence"], stream["native_call_begin_prepare_ack_sequence"])
                    self.assertLess(parent["sequence"], original["sequence"])
                    self.assertLess(original["returned_order"], disposal["started_order"])
                    self.assertLess(original["sequence"], joined["sequence"])
                    with self.assertRaises(ValueError):
                        _go_quic_owner(raw["events"], original, original["remote_peer_id"], "", "quic")

    def test_empty_late_diagnostic_errors_keep_owner_byte_cause_and_join_guards(self):
        modes = ("zero_begin_ack", "future_ack", "foreign_ack", "boolean_ack", "foreign_owner", "foreign_parent",
                 "missing_parent", "future_parent", "missing_parent_baseline",
                 "parent_cause", "return_cause", "send_cause_only", "missing_cause_link", "wrapped", "opaque", "errno",
                 "nonzero", "mismatched_remote", "prefix", "partial", "lazy", "proposal", "selected", "header",
                 "other_io_bytes", "half_disposal", "early_disposal", "before_write_return", "failed_disposal",
                 "missing_disposal", "foreign_disposal", "missing_join", "active_join", "not_final", "host_live", "sticky", "extra_field")
        for close in (False, True):
            for mode in modes:
                artifact = synthetic_case()
                stream, parent, read, write, disposal, joined = self.empty_late_diagnostic_events(artifact, close=close)
                raw, events = artifact["raw"]["sink"], artifact["raw"]["sink"]["events"]
                mutations = {
                    "zero_begin_ack": lambda: stream.update(native_call_begin_prepare_ack_sequence=0),
                    "future_ack": lambda: stream.update(prepare_ack_sequence=read["sequence"]),
                    "foreign_ack": lambda: read.update(prepare_ack_sequence=stream["sequence"]),
                    "boolean_ack": lambda: read.update(terminal_prepare_ack_sequence=True),
                    "foreign_owner": lambda: read.update(native_stream_id=8),
                    "foreign_parent": lambda: parent.update(native_connection_id="foreign-native-parent"),
                    "missing_parent_baseline": lambda: read.update(connection_prepare_baseline_present=False),
                    "parent_cause": lambda: parent["context"].update(remote=False),
                    "return_cause": lambda: read["connection_context"].update(error_code=7),
                    "send_cause_only": lambda: read.update(connection_context=deepcopy(read["connection_context_at_prepare"])),
                    "missing_cause_link": lambda: read.update(same_native_connection_context_cause=False),
                    "wrapped": lambda: read.update(error_type="*fmt.wrapError"),
                    "opaque": lambda: read.update(transport_error_type="*errors.errorString"),
                    "errno": lambda: read.update(error_type="syscall.Errno"),
                    "nonzero": lambda: read.update(transport_error_code=7),
                    "mismatched_remote": lambda: read.update(transport_error_remote=False),
                    "prefix": lambda: read.update(successful_prefix_bytes=1),
                    "partial": lambda: read["negotiation_snapshot"]["read"].update(partial_frame=self.cleanup_capture(b"\x01")),
                    "lazy": lambda: read["negotiation_snapshot"]["write"].update(lazy_tail=self.cleanup_capture(b"\x01")),
                    "proposal": lambda: read["negotiation_snapshot"].update(proposal="/meshsub/1.1.0"),
                    "selected": lambda: read.update(protocol="/meshsub/1.1.0"),
                    "before_write_return": lambda: write.update(returned_order=8),
                    "failed_disposal": lambda: disposal.update(outcome="error", error="native disposal failed", error_type="syscall.Errno", typed_cause="opaque"),
                    "foreign_disposal": lambda: disposal.update(native_stream_id=8),
                    "active_join": lambda: joined.update(active_native_calls=1),
                    "not_final": lambda: raw.update(finalized=False),
                    "host_live": lambda: raw.update(host_close_returned=False),
                    "sticky": lambda: raw.update(error="earlier unrelated sticky failure"),
                    "extra_field": lambda: read.update(accepted=True),
                }
                if mode in mutations:
                    mutations[mode]()
                elif mode in {"missing_disposal", "missing_join", "missing_parent"}:
                    # Preserve event indexing; unrelated telemetry cannot replace a native operation/join.
                    target = disposal if mode == "missing_disposal" else parent if mode == "missing_parent" else joined
                    replacement = {"sequence": target["sequence"], "mono_ns": target["mono_ns"], "kind": "score",
                                   "source": "go.pubsub.WithPeerScoreInspect", "peer_scores": []}
                    target.clear()
                    target.update(replacement)
                elif mode == "future_parent":
                    left, right = events.index(parent), events.index(read)
                    events[left], events[right] = events[right], events[left]
                    for index in (left, right):
                        events[index].update(sequence=index + 1, mono_ns=index + 1)
                elif mode == "early_disposal":
                    disposal.update(started_order=1, returned_order=2)
                    read.update(started_order=3, returned_order=4)
                    write.update(started_order=5, returned_order=6)
                    if close:
                        events[disposal["send_context_receipt_sequence"] - 1].update(started_order=1, returned_order=2)
                elif mode == "half_disposal":
                    disposal["operation"] = "stream_close_read"
                    if close:
                        disposal.pop("native_close_error_classification")
                        disposal.pop("send_context_receipt_sequence")
                else:
                    if mode == "header":
                        selected = next(value for value in events if value["kind"] == "protocol")
                        added = deepcopy(events[selected["negotiation_frame_sequences"][0] - 1])
                        added.update({key: read[key] for key in ("native_connection_id", "native_stream_id", "connection_receipt_sequence",
                                     "native_stream_receipt_sequence", "remote_peer_id", "stream_direction", "owner_basis")})
                    else:
                        added = deepcopy(write)
                        for key in ("error_code", "remote", "transport_error_type", "transport_error_code",
                                    "transport_error_remote", "same_native_connection_context_cause"):
                            added.pop(key)
                        added.update(started_order=7, returned_order=8, outcome="ok", error=None, error_type=None,
                                     typed_cause="none", successful_prefix_bytes=1)
                        added["negotiation_snapshot"]["successful_prefix"] = self.cleanup_capture(b"\x01")
                        added["negotiation_snapshot"]["write"]["partial_frame"] = self.cleanup_capture(b"\x01")
                    self.insert_sink_events(artifact, events.index(joined), [added])
                with self.subTest(close=close, mode=mode), self.assertRaises(ValueError):
                    validate_case(artifact)

    def test_lower_unselected_diagnostics_are_not_selected_proof_and_do_not_mutate_receipts(self):
        for mode in ("prefix", "late_reset", "late_close"):
            artifact = synthetic_case()
            observed = self.lower_diagnostic_events(artifact, mode)
            before = deepcopy(artifact)
            with self.subTest(mode=mode):
                validate_case(artifact)
                self.assertTrue(_same_json(before, artifact))
                self.assertEqual(observed["protocol_at_native_return"], "")
                self.assertFalse(observed["prepare_baseline_present"])
                raw = artifact["raw"]["sink"]
                ack = next(event for event in raw["events"] if event["kind"] == "shutdown_prepared")
                self.assertIs(shutdown_ack(raw, "go", "sink", raw["case_token"], raw["local_peer_id"], ack["command_sequence"]), ack)

    def test_lower_diagnostic_owner_phase_ack_capture_and_tail_forgeries_are_fatal(self):
        mutations = (
            lambda a, e: e.update(native_stream_id=8),
            lambda a, e: e.update(connection_receipt_sequence=True),
            lambda a, e: e.update(remote_peer_id=peer_id(99)),
            lambda a, e: e.update(source="go.network.Stream.read"),
            lambda a, e: e.update(observation_phase="selected_at_native_return"),
            lambda a, e: e.update(protocol_at_native_return="/meshsub/1.1.0"),
            lambda a, e: e.update(started_order=True),
            lambda a, e: e.update(returned_order=2**64),
            lambda a, e: e.update(prepare_ack_sequence=True),
            lambda a, e: e.update(terminal_prepare_ack_sequence=True),
            lambda a, e: e.update(prepare_snapshot_ack_sequence=e["sequence"]),
            lambda a, e: e.update(stream_prepare_baseline_present=True),
            lambda a, e: e.update(typed_cause="libp2p_quic_application_error"),
            lambda a, e: e.update(error="synthetic original error", error_type="*network.ConnError", outcome="error"),
            lambda a, e: e["negotiation_snapshot"].update(parser_failed=True),
            lambda a, e: e["negotiation_snapshot"].update(capture_complete=False),
            lambda a, e: e["negotiation_snapshot"].update(proposals=True),
            lambda a, e: e["negotiation_snapshot"]["frame_sequences"].pop(),
            lambda a, e: e["negotiation_snapshot"]["successful_prefix"].update(sha256="0" * 64),
            lambda a, e: e["negotiation_snapshot"]["successful_prefix"].update(bytes=4 * 1024 * 1024 + 1),
            lambda a, e: e["negotiation_snapshot"]["read"]["lazy_tail"].update(hex="01", bytes=1, sha256=hashlib.sha256(b"\x01").hexdigest()),
            lambda a, e: e["negotiation_snapshot"]["write"]["partial_frame"].update(hex="00", bytes=1, sha256=hashlib.sha256(b"\x00").hexdigest()),
            lambda a, e: next(v for v in a["raw"]["sink"]["events"] if v["kind"] == "rpc").update(protocol=""),
        )
        for mutation in mutations:
            artifact = synthetic_case()
            observed = self.lower_diagnostic_events(artifact)
            mutation(artifact, observed)
            with self.subTest(mutation=mutation):
                with self.assertRaises(ValueError):
                    validate_case(artifact)

    def test_unselected_pubsub_proposal_with_complete_subscribe_tail_is_not_terminal_acceptance(self):
        artifact = synthetic_case()
        observed = self.lower_diagnostic_events(artifact, "late_proposal")
        raw = artifact["raw"]["sink"]
        _go_quic_operations(raw, raw["events"], terminal=False)
        with self.assertRaisesRegex(ValueError, "unselected PubSub proposal/tail"):
            validate_case(artifact)
        observed.update(error="synthetic original application close", error_type="*network.ConnError", outcome="error")
        with self.assertRaises(ValueError):
            shutdown_ack(raw, "go", "sink", raw["case_token"], raw["local_peer_id"],
                         next(event for event in raw["events"] if event["kind"] == "shutdown_prepared")["command_sequence"])

    def test_late_diagnostics_require_real_parent_baseline_and_close_context(self):
        for mutation in (
            lambda a, e: e.update(prepare_baseline_present=True),
            lambda a, e: e.update(prepare_ack_sequence=0),
            lambda a, e: e.update(connection_prepare_baseline_present=False),
            lambda a, e: next(v for v in a["raw"]["sink"]["events"] if v["kind"] == "shutdown_prepared").update(case_token="foreign"),
            lambda a, e: e.update(native_close_error_classification="opaque_unwrapped_native_error"),
            lambda a, e: e.update(send_context_receipt_sequence=0),
        ):
            artifact = synthetic_case()
            observed = self.lower_diagnostic_events(artifact, "late_close")
            mutation(artifact, observed)
            with self.subTest(late_mutation=mutation):
                with self.assertRaises(ValueError):
                    validate_case(artifact)

    @staticmethod
    def cleanup_capture(data):
        return {"bytes": len(data), "hex": data.hex(), "sha256": hashlib.sha256(data).hexdigest(), "capture_complete": True}

    def lower_cleanup_events(self, artifact, *, close=False, peer_header=False, prior_disposal=False, early_publication=False):
        """Exact frozen Go cleanup schema; the candidate never becomes a selected RPC."""
        write = self.lower_diagnostic_events(artifact, "late_reset")
        raw, events = artifact["raw"]["sink"], artifact["raw"]["sink"]["events"]
        stream = events[write["native_stream_receipt_sequence"] - 1]
        parent = next(value for value in events if value["kind"] == "native_quic_connection_context")
        selected = next(value for value in events if value["kind"] == "protocol")
        base = {key: write[key] for key in ("native_connection_id", "native_stream_id", "connection_receipt_sequence",
                "native_stream_receipt_sequence", "remote_peer_id", "stream_direction", "protocol", "owner_basis")}
        frames = [{**deepcopy(events[ref - 1]), **base, "protocol": events[ref - 1]["protocol"]}
                  for ref in (selected["negotiation_frame_sequences"][0], selected["negotiation_frame_sequences"][2])]
        self.insert_sink_events(artifact, events.index(write), frames)
        tail = bytes.fromhex(receipt(field(1, field(1, 1) + field(2, ("forge-pr11:" + raw["case_token"]).encode())
                                          + field(3, 0) + field(4, 0)), "write")["framed_hex"])
        wire = b"".join(bytes.fromhex(value["receipt"]["framed_hex"]) for value in frames) + tail
        write.pop("requested_reset_code")
        write.update(kind="native_quic_negotiation_io_return", source=GO_QUIC_SOURCES["native_quic_negotiation_io_return"],
                     operation="stream_write", direction="write", requested_bytes=len(wire), successful_prefix_bytes=len(wire),
                     successful_prefix_valid=True)
        snapshot = write["negotiation_snapshot"]
        snapshot.update(proposal=selected["protocol"], proposals=1, touched_pubsub=True,
                        frame_sequences=[value["sequence"] for value in frames], successful_prefix=self.cleanup_capture(wire))
        snapshot["write"].update(header_seen=True, paused=True, token=selected["protocol"], lazy_tail=self.cleanup_capture(tail))
        parent["context"].update(remote=True, error="synthetic same-parent application close")

        def event(kind, fields):
            return {"sequence": 0, "mono_ns": 0, "kind": kind, "source": GO_QUIC_SOURCES[kind], **base, **fields}

        header = header_return = None
        if peer_header:
            header = deepcopy(frames[0])
            header.update(direction="read", receipt={"framed_hex": header["receipt"]["framed_hex"],
                                                      "read": deepcopy(header["receipt"]["write"])})
            self.insert_sink_events(artifact, events.index(parent), [header])
            header_return = deepcopy(write)
            header_bytes = bytes.fromhex(header["receipt"]["framed_hex"])
            header_return.update(operation="stream_read", direction="read", started_order=3, returned_order=4,
                                 requested_bytes=len(header_bytes), successful_prefix_bytes=len(header_bytes))
            header_return["negotiation_snapshot"]["frame_sequences"].append(header["sequence"])
            header_return["negotiation_snapshot"]["read"]["header_seen"] = True
            header_return["negotiation_snapshot"]["successful_prefix"] = self.cleanup_capture(header_bytes)
            self.insert_sink_events(artifact, events.index(parent), [header_return])
        read_start = 5 if peer_header else 3
        read_return = read_start + (3 if prior_disposal else 1)
        disposal_start = read_return + 1
        original = deepcopy(write)
        original.update(operation="stream_read", direction="read", started_order=read_start, returned_order=read_return, requested_bytes=1,
                        successful_prefix_bytes=0, connection_context=deepcopy(parent["context"]), outcome="error",
                        error="synthetic sealed native Read error", error_type="*network.ConnError", typed_cause="libp2p_quic_application_error",
                        error_code=0, remote=True, transport_error_type="*qerr.ApplicationError", transport_error_code=0,
                        transport_error_remote=True, same_native_connection_context_cause=True)
        original["negotiation_snapshot"]["successful_prefix"] = self.cleanup_capture(b"")
        if peer_header:
            original["negotiation_snapshot"]["frame_sequences"].append(header["sequence"])
            original["negotiation_snapshot"]["read"]["header_seen"] = True
        common = {"operation_receipt_sequence": 0, "candidate_write_receipt_sequence": write["sequence"],
                  "connection_context_receipt_sequence": parent["sequence"], "prepare_ack_sequence": write["prepare_ack_sequence"],
                  "candidate_protocol": selected["protocol"], "candidate_protocol_frame_sequence": frames[1]["sequence"],
                  "peer_header_frame_sequence": header["sequence"] if peer_header else 0,
                  "candidate_bytes_complete": True, "selected_rpc_authority": False, "negotiation_complete": False,
                  "framing_clean": False, "terminal_outcome": "negotiation_aborted_cleanup_pending",
                  "same_native_connection_context_cause": True}
        pending = event("native_quic_negotiation_cleanup_pending", common)
        disposal = deepcopy(write)
        for field_name in ("direction", "requested_bytes", "successful_prefix_bytes", "successful_prefix_valid"):
            disposal.pop(field_name)
        disposal["negotiation_snapshot"] = {key: deepcopy(value) for key, value in original["negotiation_snapshot"].items()
                                            if key != "successful_prefix"}
        send = {"done": True, "cause_type": "*quic.StreamError", "error_code": 0, "remote": False,
                "native_stream_id": stream["native_stream_id"], "error": "synthetic local send reset"}
        disposal.update(kind="native_stream_operation", source=GO_QUIC_SOURCES["native_stream_operation"],
                        operation="stream_close" if close else "stream_reset", requested_reset_code=None,
                        started_order=disposal_start, returned_order=disposal_start + 1,
                        send_context=send, connection_context=deepcopy(parent["context"]))
        first = None
        if prior_disposal:
            first = deepcopy(disposal)
            first.update(operation="stream_reset", started_order=read_start + 1, returned_order=read_start + 2)
        added = ([first] if first else []) + [original, pending]
        if close:
            context = event("native_quic_send_context", {
                "started_order": disposal_start, "returned_order": disposal_start + 1, "prepare_ack_sequence": write["prepare_ack_sequence"],
                "send_context": deepcopy(send), "connection_context": deepcopy(parent["context"]),
                "observation_phase": "unselected_at_native_return", "protocol_at_native_return": "",
                "observation_basis": "same_lower_delegate_Context_at_actual_Close_RETURN"})
            added.append(context)
            disposal.update(native_close_error_classification="none", send_context_receipt_sequence=0)
        framing = event("native_quic_framing_finalized", {
            "framing_clean": False, "io_joined": True, "read_finalized": True, "write_finalized": True,
            "negotiation_complete": False, "pending_read_frame_bytes": 0, "pending_write_frame_bytes": 0,
            "native_owner_disposed": True, "owner_disposal_receipt_sequence": 0,
            "cleanup_owner_disposal_receipt_sequence": 0, "peer_header_frame_sequence": common["peer_header_frame_sequence"],
            "negotiation_snapshot": deepcopy(disposal["negotiation_snapshot"]), "candidate_bytes_complete": True,
            "selected_rpc_authority": False, "negotiation_cleanup_pending_receipt_sequence": 0})
        if early_publication:
            added = ([first] if first else []) + ([context] if close else []) + [disposal, original, pending, framing]
        else:
            added += [disposal, framing]
        self.insert_sink_events(artifact, events.index(parent) + 1, added)
        pending["operation_receipt_sequence"] = original["sequence"]
        first = first if first is not None else disposal
        framing.update(owner_disposal_receipt_sequence=first["sequence"], cleanup_owner_disposal_receipt_sequence=disposal["sequence"],
                       negotiation_cleanup_pending_receipt_sequence=pending["sequence"])
        if close:
            disposal["send_context_receipt_sequence"] = context["sequence"]
        joined = next(value for value in events if value["kind"] == "native_quic_join")
        final = event("native_quic_negotiation_cleanup_finalized", {
            **{key: value for key, value in pending.items() if key not in {"sequence", "mono_ns", "kind", "source"}},
            "terminal_outcome": "negotiation_aborted_cleanup", "pending_receipt_sequence": pending["sequence"],
            "framing_receipt_sequence": framing["sequence"], "owner_disposal_receipt_sequence": disposal["sequence"],
            "first_owner_disposal_receipt_sequence": first["sequence"],
            "native_join_receipt_sequence": joined["sequence"], "accepted": True})
        self.insert_sink_events(artifact, len(events), [final])
        return {"write": write, "original": original, "pending": pending, "disposal": disposal,
                "framing": framing, "final": final, "stream": stream, "parent": parent, "join": joined,
                "header": header, "header_return": header_return, "first": first}

    def test_cleanup_only_complete_chain_preserves_raw_error_and_no_selected_authority(self):
        for close in (False, True):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, close=close)
            before = deepcopy(artifact)
            with self.subTest(close=close):
                validate_case(artifact)
                self.assertTrue(_same_json(before, artifact))
                self.assertEqual(records["original"]["outcome"], "error")
                self.assertEqual(records["original"]["successful_prefix_bytes"], 0)
                self.assertFalse(records["final"]["selected_rpc_authority"])
                with self.assertRaises(ValueError):
                    _go_quic_owner(artifact["raw"]["sink"]["events"], records["original"], records["original"]["remote_peer_id"],
                                   records["pending"]["candidate_protocol"], "quic")

    def test_cleanup_peer_header_and_post_read_pin_preserve_first_disposal(self):
        for close in (False, True):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, close=close, peer_header=True, prior_disposal=True)
            before = deepcopy(artifact)
            with self.subTest(close=close):
                validate_case(artifact)
                self.assertTrue(_same_json(before, artifact))
                self.assertEqual(len(records["write"]["negotiation_snapshot"]["frame_sequences"]), 2)
                self.assertEqual(len(records["original"]["negotiation_snapshot"]["frame_sequences"]), 3)
                self.assertEqual(records["pending"]["peer_header_frame_sequence"], records["header"]["sequence"])
                self.assertLess(records["write"]["sequence"], records["header"]["sequence"])
                self.assertLess(records["first"]["returned_order"], records["original"]["returned_order"])
                self.assertLess(records["original"]["returned_order"], records["disposal"]["started_order"])
                self.assertEqual(records["framing"]["owner_disposal_receipt_sequence"], records["first"]["sequence"])
                self.assertEqual(records["framing"]["cleanup_owner_disposal_receipt_sequence"], records["disposal"]["sequence"])
                self.assertEqual(records["final"]["first_owner_disposal_receipt_sequence"], records["first"]["sequence"])
                self.assertEqual(records["final"]["owner_disposal_receipt_sequence"], records["disposal"]["sequence"])
                self.assertEqual(records["original"]["outcome"], "error")
                self.assertEqual(records["original"]["successful_prefix_bytes"], 0)
                self.assertEqual(records["final"]["protocol"], "")
                self.assertFalse(records["final"]["selected_rpc_authority"])

    def test_cleanup_post_read_disposal_may_publish_before_read_and_pending(self):
        for close in (False, True):
            for peer_header in (False, True):
                artifact = synthetic_case()
                records = self.lower_cleanup_events(artifact, close=close, peer_header=peer_header,
                                                    prior_disposal=True, early_publication=True)
                before = deepcopy(artifact)
                with self.subTest(close=close, peer_header=peer_header):
                    validate_case(artifact)
                    self.assertTrue(_same_json(before, artifact))
                    self.assertLess(records["disposal"]["sequence"], records["original"]["sequence"])
                    self.assertLess(records["original"]["sequence"], records["pending"]["sequence"])
                    self.assertLess(records["original"]["returned_order"], records["disposal"]["started_order"])
                    self.assertLess(records["disposal"]["started_order"], records["disposal"]["returned_order"])
                    self.assertTrue(_same_json(records["disposal"]["connection_context"], records["original"]["connection_context"]))
                    self.assertEqual(records["final"]["first_owner_disposal_receipt_sequence"], records["first"]["sequence"])
                    self.assertEqual(records["final"]["owner_disposal_receipt_sequence"], records["disposal"]["sequence"])
                    self.assertEqual(records["original"]["outcome"], "error")

    def test_cleanup_native_return_pin_is_not_receipt_publication_order(self):
        artifact = synthetic_case()
        records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True, early_publication=True)
        later = deepcopy(records["disposal"])
        later.update(started_order=11, returned_order=12)
        self.insert_sink_events(artifact, records["disposal"]["sequence"] - 1, [later])
        self.assertLess(later["sequence"], records["disposal"]["sequence"])
        self.assertLess(records["disposal"]["returned_order"], later["returned_order"])
        validate_case(artifact)
        records["framing"]["cleanup_owner_disposal_receipt_sequence"] = later["sequence"]
        records["final"]["owner_disposal_receipt_sequence"] = later["sequence"]
        with self.assertRaisesRegex(ValueError, "post-Read disposal pin"):
            validate_case(artifact)

    def test_cleanup_early_publication_cannot_replace_native_counters_owner_or_parent(self):
        mutations = (
            ("original", "returned_order", 12), ("disposal", "started_order", True),
            ("disposal", "returned_order", 9), ("disposal", "native_stream_id", 8),
            ("disposal", "remote_peer_id", peer_id(99)), ("disposal", "prepare_ack_sequence", 0),
            ("disposal", "operation", "stream_close_read"), ("disposal", "error", "synthetic Reset failure"),
            ("framing", "cleanup_owner_disposal_receipt_sequence", True), ("framing", "io_joined", False),
        )
        for name, field_name, value in mutations:
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True, early_publication=True)
            records[name][field_name] = value
            with self.subTest(record=name, field=field_name, value=value), self.assertRaises(ValueError):
                validate_case(artifact)
        for close in (False, True):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, close=close, peer_header=True, prior_disposal=True, early_publication=True)
            records["disposal"]["connection_context"]["error_code"] = 1
            if close:
                reference = records["disposal"]["send_context_receipt_sequence"]
                artifact["raw"]["sink"]["events"][reference - 1]["connection_context"] = deepcopy(records["disposal"]["connection_context"])
            with self.subTest(close_parent=close), self.assertRaisesRegex(ValueError, "sealed original parent context"):
                validate_case(artifact)
        artifact = synthetic_case()
        records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True, early_publication=True)
        records["framing"]["cleanup_owner_disposal_receipt_sequence"] = records["first"]["sequence"]
        records["final"]["owner_disposal_receipt_sequence"] = records["first"]["sequence"]
        with self.assertRaisesRegex(ValueError, "post-Read disposal pin"):
            validate_case(artifact)

    def test_cleanup_peer_header_and_disposal_pin_forgeries_are_fatal(self):
        mutations = (
            ("header", "native_stream_id", 8), ("header", "remote_peer_id", peer_id(99)),
            ("header", "direction", "write"), ("header", "protocol", "/multistream/2.0.0"),
            ("pending", "peer_header_frame_sequence", 0), ("pending", "peer_header_frame_sequence", True),
            ("pending", "peer_header_frame_sequence", 2**63), ("framing", "peer_header_frame_sequence", 0),
            ("final", "peer_header_frame_sequence", 0), ("final", "peer_header_frame_sequence", True),
            ("framing", "cleanup_owner_disposal_receipt_sequence", 0),
            ("framing", "cleanup_owner_disposal_receipt_sequence", True),
            ("final", "first_owner_disposal_receipt_sequence", True),
            ("final", "first_owner_disposal_receipt_sequence", 0),
            ("disposal", "native_connection_id", "foreign-pin"), ("disposal", "native_stream_id", 8),
            ("disposal", "prepare_ack_sequence", 0), ("disposal", "started_order", 8),
            ("disposal", "operation", "stream_close_read"), ("disposal", "error", "synthetic native Reset failure"),
            ("framing", "io_joined", False), ("framing", "pending_read_frame_bytes", 1),
        )
        for name, field_name, value in mutations:
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True)
            records[name][field_name] = value
            with self.subTest(record=name, field=field_name, value=value), self.assertRaises(ValueError):
                validate_case(artifact)
        for name, field_name in (("pending", "peer_header_frame_sequence"), ("final", "peer_header_frame_sequence"),
                                 ("framing", "peer_header_frame_sequence"), ("framing", "cleanup_owner_disposal_receipt_sequence"),
                                 ("final", "first_owner_disposal_receipt_sequence")):
            artifact = synthetic_case()
            self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True)[name].pop(field_name)
            with self.subTest(missing=name, field=field_name), self.assertRaises(ValueError):
                validate_case(artifact)
        for mutation in (
            lambda r: r["header"]["receipt"]["read"].update(framed_sha256="0" * 64),
            lambda r: r["header"]["receipt"]["read"].update(complete_frames=False),
            lambda r: r["original"]["negotiation_snapshot"]["read"].update(header_seen=False),
            lambda r: r["original"]["negotiation_snapshot"]["read"].update(paused=True, token="na"),
            lambda r: r["original"]["negotiation_snapshot"].update(reply="/meshsub/1.1.0", selected_protocol="/meshsub/1.1.0"),
            lambda r: r["original"]["negotiation_snapshot"]["read"].update(partial_frame=self.cleanup_capture(b"\x80")),
            lambda r: r["original"]["negotiation_snapshot"]["read"].update(lazy_tail=self.cleanup_capture(b"\x80")),
            lambda r: r["framing"]["negotiation_snapshot"]["read"].update(header_seen=False),
            lambda r: r["disposal"]["connection_context"].update(error_code=1),
        ):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True)
            mutation(records)
            with self.subTest(snapshot=mutation), self.assertRaises(ValueError):
                validate_case(artifact)
        for token in ("/multistream/2.0.0", "na", "/meshsub/1.1.0"):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True)
            body = (token + "\n").encode()
            wire = bytes([len(body)]) + body
            records["header"].update(protocol=token, receipt={"framed_hex": wire.hex(), "read": {
                "framed_bytes": len(wire), "frames": 1, "framed_sha256": hashlib.sha256(wire).hexdigest(),
                "complete_frames": True, "invalid_or_over_limit": False}})
            records["header_return"].update(requested_bytes=len(wire), successful_prefix_bytes=len(wire))
            records["header_return"]["negotiation_snapshot"]["successful_prefix"] = self.cleanup_capture(wire)
            with self.subTest(canonical_wrong_header=token), self.assertRaises(ValueError):
                validate_case(artifact)
        for token in ("/multistream/1.0.0", "na", "/meshsub/1.1.0"):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True)
            extra = deepcopy(records["header"])
            body = (token + "\n").encode()
            wire = bytes([len(body)]) + body
            extra.update(protocol=token, receipt={"framed_hex": wire.hex(), "read": {
                "framed_bytes": len(wire), "frames": 1, "framed_sha256": hashlib.sha256(wire).hexdigest(),
                "complete_frames": True, "invalid_or_over_limit": False}})
            self.insert_sink_events(artifact, records["header_return"]["sequence"] - 1, [extra])
            header_bytes = bytes.fromhex(records["header"]["receipt"]["framed_hex"]) + wire
            records["header_return"].update(requested_bytes=len(header_bytes), successful_prefix_bytes=len(header_bytes))
            records["header_return"]["negotiation_snapshot"]["successful_prefix"] = self.cleanup_capture(header_bytes)
            for name in ("header_return", "first", "original", "disposal", "framing"):
                records[name]["negotiation_snapshot"]["frame_sequences"].append(extra["sequence"])
            with self.subTest(extra_peer_reply=token), self.assertRaises(ValueError):
                validate_case(artifact)
        for name in ("header", "first", "disposal"):
            artifact = synthetic_case()
            record = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True)[name]
            replacement = {key: record[key] for key in ("sequence", "mono_ns")}
            replacement.update(kind="join", source="go.pubsub.RawTracer.Join")
            artifact["raw"]["sink"]["events"][record["sequence"] - 1] = replacement
            with self.subTest(absent=name), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_cleanup_disposal_pins_cannot_retarget_prior_or_later_success(self):
        for mode in ("prior_as_pin", "replace_first", "later_as_pin"):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact, peer_header=True, prior_disposal=True)
            if mode == "prior_as_pin":
                records["framing"]["cleanup_owner_disposal_receipt_sequence"] = records["first"]["sequence"]
                records["final"]["owner_disposal_receipt_sequence"] = records["first"]["sequence"]
            elif mode == "replace_first":
                records["framing"]["owner_disposal_receipt_sequence"] = records["disposal"]["sequence"]
                records["final"]["first_owner_disposal_receipt_sequence"] = records["disposal"]["sequence"]
            else:
                later = deepcopy(records["disposal"])
                later.update(started_order=11, returned_order=12)
                self.insert_sink_events(artifact, records["framing"]["sequence"] - 1, [later])
                records["framing"]["cleanup_owner_disposal_receipt_sequence"] = later["sequence"]
                records["final"]["owner_disposal_receipt_sequence"] = later["sequence"]
            message = "original first owner disposal" if mode == "replace_first" else "post-Read disposal pin"
            with self.subTest(mode=mode), self.assertRaisesRegex(ValueError, message):
                validate_case(artifact)

    def test_cleanup_disposal_parent_cannot_replace_sealed_read_context(self):
        for close in (False, True):
            for cause in ("nonzero", "remote", "opaque"):
                artifact = synthetic_case()
                records = self.lower_cleanup_events(artifact, close=close)
                context = records["disposal"]["connection_context"]
                if cause == "nonzero":
                    context["error_code"] = 1
                elif cause == "remote":
                    context["remote"] = not context["remote"]
                else:
                    context.update(cause_type="*errors.errorString", error_code=None, remote=None)
                if close:
                    reference = records["disposal"]["send_context_receipt_sequence"]
                    artifact["raw"]["sink"]["events"][reference - 1]["connection_context"] = deepcopy(context)
                with self.subTest(close=close, cause=cause), self.assertRaisesRegex(
                        ValueError, "cleanup disposal changed its sealed original parent context"):
                    validate_case(artifact)

    def test_cleanup_owner_begin_ack_phase_typed_cause_disposal_and_join_forgeries_are_fatal(self):
        mutations = (
            ("stream", "native_call_begin_prepare_ack_sequence", 0),
            ("stream", "native_call_begin_prepare_ack_sequence", True),
            ("stream", "native_call_begin_prepare_ack_sequence", 2**63),
            ("stream", "native_call_begin_prepare_ack_sequence", 1),
            ("stream", "native_call_begin_observation_basis", "registration_after_native_return"),
            ("write", "requested_bytes", 0), ("write", "returned_order", 7),
            ("original", "native_stream_id", 8), ("original", "remote_peer_id", peer_id(99)),
            ("original", "prepare_ack_sequence", 0), ("original", "terminal_prepare_ack_sequence", True),
            ("original", "context_observation_basis", "public_context_samples_after_sealed_native_return"),
            ("original", "error_type", "*fmt.wrapError"), ("original", "transport_error_type", "*errors.errorString"),
            ("original", "error_code", 1), ("original", "transport_error_code", 1),
            ("original", "remote", 1), ("original", "transport_error_remote", False),
            ("original", "same_native_connection_context_cause", False), ("original", "successful_prefix_bytes", 1),
            ("original", "successful_prefix_valid", False), ("original", "stream_prepare_baseline_present", True),
            ("parent", "prepare_baseline_present", False), ("parent", "native_connection_id", "foreign-parent"),
            ("pending", "protocol", "/meshsub/1.1.0"), ("pending", "candidate_bytes_complete", False),
            ("pending", "candidate_protocol_frame_sequence", True), ("pending", "selected_rpc_authority", True),
            ("pending", "negotiation_complete", True), ("pending", "framing_clean", True),
            ("pending", "operation_receipt_sequence", True), ("pending", "candidate_write_receipt_sequence", 1),
            ("pending", "connection_context_receipt_sequence", 1), ("pending", "prepare_ack_sequence", 0),
            ("disposal", "operation", "stream_close_read"), ("disposal", "operation", "stream_reset_with_error"),
            ("disposal", "outcome", "native_send_reset_close_pending"), ("disposal", "error", "synthetic primary error"),
            ("disposal", "started_order", 4), ("disposal", "returned_order", 2**64),
            ("framing", "io_joined", False), ("framing", "native_owner_disposed", False),
            ("framing", "pending_write_frame_bytes", 1), ("framing", "candidate_bytes_complete", False),
            ("framing", "framing_clean", True), ("framing", "negotiation_complete", True),
            ("join", "active_native_calls", 1), ("join", "observed_streams", 1),
            ("final", "accepted", False), ("final", "pending_receipt_sequence", True),
            ("final", "native_join_receipt_sequence", 1), ("final", "selected_rpc_authority", True),
        )
        for name, field_name, value in mutations:
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact)
            records[name][field_name] = value
            with self.subTest(record=name, field=field_name, value=value), self.assertRaises(ValueError):
                validate_case(artifact)
        for name in ("stream", "original", "pending", "disposal", "framing", "final"):
            artifact = synthetic_case()
            self.lower_cleanup_events(artifact)[name]["unknown_field"] = False
            with self.subTest(unknown=name), self.assertRaises(ValueError):
                validate_case(artifact)
        for name, field_name in (("stream", "native_call_begin_prepare_ack_sequence"),
                                 ("stream", "native_call_begin_observation_basis"), ("original", "prepare_snapshot_ack_sequence")):
            artifact = synthetic_case()
            self.lower_cleanup_events(artifact)[name].pop(field_name)
            with self.subTest(missing=name, field=field_name), self.assertRaises(ValueError):
                validate_case(artifact)
        for mutation in (
            lambda r, a: r["original"]["connection_context"].update(error_code=1),
            lambda r, a: r["original"]["connection_context"].update(remote=False),
            lambda r, a: r["disposal"]["send_context"].update(error_code=1),
            lambda r, a: r["disposal"]["send_context"].update(native_stream_id=8),
            lambda r, a: r["framing"]["negotiation_snapshot"].update(parser_failed=True),
            lambda r, a: next(e for e in a["raw"]["sink"]["events"] if e["kind"] == "shutdown_prepared").update(case_token="foreign-ack"),
            lambda r, a: next(e for e in a["raw"]["sink"]["events"] if e["kind"] == "shutdown_prepared")["native_quic_context_snapshots"][0].update(
                connection_context=deepcopy(r["parent"]["context"])),
        ):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact)
            mutation(records, artifact)
            with self.subTest(nested=mutation), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_cleanup_canonical_subscribe_full_consumption_hash_and_partial_negatives(self):
        token = synthetic_case()["raw"]["sink"]["case_token"]
        topic = ("forge-pr11:" + token).encode()
        sub = field(1, 1) + field(2, topic) + field(3, 0) + field(4, 0)
        good = bytes.fromhex(receipt(field(1, sub), "write")["framed_hex"])
        bodies = (field(1, sub) * 2, field(1, sub) + field(2, b""), field(1, sub) + field(3, b""),
                  field(1, sub) + field(42, b""), field(1, sub + field(1, 1)),
                  field(1, sub + field(5, 0)), field(1, sub + field(3, 0)),
                  field(1, field(1, 2) + field(2, topic)), field(1, field(1, 0) + field(2, topic)),
                  field(1, field(1, 1) + field(2, b"foreign-topic")), field(1, field(2, topic) + field(1, 1)),
                  field(1, field(1, 1) + field(2, topic) + field(3, 1)),
                  field(1, field(1, 1) + field(2, topic) + field(4, 1)))
        tails = [bytes.fromhex(receipt(body, "write")["framed_hex"]) for body in bodies]
        tails += [good[:-1], good + b"\x00", good + good, b"\x00", bytes([good[0] | 128, 0]) + good[1:]]
        for tail in tails:
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact)
            refs = records["write"]["negotiation_snapshot"]["frame_sequences"]
            prefix = b"".join(bytes.fromhex(artifact["raw"]["sink"]["events"][ref - 1]["receipt"]["framed_hex"]) for ref in refs) + tail
            records["write"].update(requested_bytes=len(prefix), successful_prefix_bytes=len(prefix))
            records["write"]["negotiation_snapshot"]["successful_prefix"] = self.cleanup_capture(prefix)
            for name in ("write", "original", "disposal", "framing"):
                records[name]["negotiation_snapshot"]["write"]["lazy_tail"] = self.cleanup_capture(tail)
            with self.subTest(tail=tail.hex()), self.assertRaises(ValueError):
                validate_case(artifact)
        for field_name, value in (("sha256", "0" * 64), ("capture_complete", False), ("bytes", 16 * 1024 + 11), ("hex", "00")):
            artifact = synthetic_case()
            self.lower_cleanup_events(artifact)["write"]["negotiation_snapshot"]["write"]["lazy_tail"][field_name] = value
            with self.subTest(capture=field_name), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_cleanup_missing_duplicate_orphan_chain_and_prior_failure_cannot_be_hidden(self):
        for name in ("write", "original", "pending", "disposal", "framing", "join", "final"):
            artifact = synthetic_case()
            record = self.lower_cleanup_events(artifact)[name]
            replacement = {key: record[key] for key in ("sequence", "mono_ns")}
            replacement.update(kind="join", source="go.pubsub.RawTracer.Join")
            artifact["raw"]["sink"]["events"][record["sequence"] - 1] = replacement
            with self.subTest(absent=name), self.assertRaises(ValueError):
                validate_case(artifact)
        for name in ("pending", "framing", "final"):
            artifact = synthetic_case()
            records = self.lower_cleanup_events(artifact)
            self.insert_sink_events(artifact, records[name]["sequence"], [deepcopy(records[name])])
            with self.subTest(duplicate=name), self.assertRaises(ValueError):
                validate_case(artifact)
        for field_name, value in (("error", "synthetic earlier sticky failure"), ("overflow", True)):
            artifact = synthetic_case()
            self.lower_cleanup_events(artifact)
            artifact["raw"]["sink"][field_name] = value
            with self.subTest(sticky=field_name), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_cleanup_active_snapshot_defers_only_indexed_pending_not_completed_or_unrelated_error(self):
        artifact = synthetic_case()
        records = self.lower_cleanup_events(artifact)
        raw = artifact["raw"]["sink"]
        ack = next(event for event in raw["events"] if event["kind"] == "shutdown_prepared")
        prefix = deepcopy(raw)
        prefix.update(finalized=False, joined=False, events=prefix["events"][:records["pending"]["sequence"]])
        self.assertIsNotNone(shutdown_ack(prefix, "go", "sink", raw["case_token"], raw["local_peer_id"], ack["command_sequence"], active=True))
        completed = deepcopy(raw)
        completed.update(finalized=False, joined=False)
        with self.assertRaises(ValueError):
            shutdown_ack(completed, "go", "sink", raw["case_token"], raw["local_peer_id"], ack["command_sequence"], active=True)
        unrelated = deepcopy(records["original"])
        for field_name in ("error_code", "remote", "transport_error_type", "transport_error_code", "transport_error_remote",
                           "same_native_connection_context_cause"):
            unrelated.pop(field_name)
        unrelated.update(error_type="*os.SyscallError", typed_cause="opaque", error="synthetic unrelated native errno 38",
                         started_order=7, returned_order=8)
        self.insert_sink_events(artifact, records["original"]["sequence"] - 1, [unrelated])
        with self.assertRaises(ValueError):
            validate_case(artifact)

    def test_lower_quic_owner_rpc_and_negotiation_forgeries_are_rejected(self):
        mutations = (
            lambda a, r, c, s, p: r.update(peer_id=peer_id(99)),
            lambda a, r, c, s, p: r.update(source="go.pubsub.native_stream.read"),
            lambda a, r, c, s, p: r.update(native_stream_id=True),
            lambda a, r, c, s, p: r.update(native_stream_id=4),
            lambda a, r, c, s, p: r.update(native_stream_receipt_sequence=True),
            lambda a, r, c, s, p: r.update(connection_receipt_sequence=r["sequence"]),
            lambda a, r, c, s, p: c["secured_callback"].update(remote_peer_id=peer_id(99)),
            lambda a, r, c, s, p: c.update(remote_public_key_sha256="c" * 64),
            lambda a, r, c, s, p: c.update(authentication_basis="configured_TLS"),
            lambda a, r, c, s, p: c.update(remote_address="/ip4/127.0.0.1/tcp/1"),
            lambda a, r, c, s, p: s.update(prepare_ack_sequence=True),
            lambda a, r, c, s, p: p.update(protocol="/meshsub/1.0.0"),
            lambda a, r, c, s, p: p["negotiation_frame_sequences"].pop(),
            lambda a, r, c, s, p: p["negotiation_frame_sequences"].append(True),
            lambda a, r, c, s, p: a["raw"]["sink"].update(local_peer_id=peer_id(99)),
        )
        for mutate in mutations:
            artifact = synthetic_case()
            events = artifact["raw"]["sink"]["events"]
            rpc = next(value for value in events if value["kind"] == "rpc")
            connection = events[rpc["connection_receipt_sequence"] - 1]
            stream = events[rpc["native_stream_receipt_sequence"] - 1]
            selected = next(value for value in events if value["source"] == GO_QUIC_SOURCES["protocol"])
            mutate(artifact, rpc, connection, stream, selected)
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                validate_case(artifact)
        for kind in ("native_quic_connection", "native_quic_stream", "protocol"):
            artifact = synthetic_case()
            events = artifact["raw"]["sink"]["events"]
            duplicate = deepcopy(next(value for value in events if value["kind"] == kind and value["source"] == GO_QUIC_SOURCES[kind]))
            duplicate.update(sequence=len(events) + 1, mono_ns=len(events) + 1)
            events.append(duplicate)
            with self.subTest(duplicate=kind), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_lower_quic_pending_close_cannot_normalize_public_errors_or_borrow_read(self):
        for outcome in ("remote_close", "repeat_close"):
            for mode in ("errno", "stream_nonzero", "application", "wrapped", "joined", "sentinel", "foreign_stream",
                         "nonzero_context", "remote_read_without_send_context", "future_reset", "failed_reset", "sticky"):
                artifact = synthetic_case()
                reset, observed, _, _, _ = self.lower_terminal_events(artifact, outcome, late_reset=True)
                if mode in {"errno", "stream_nonzero", "application", "wrapped", "joined", "sentinel"}:
                    observed["error_type"] = {"errno": "syscall.Errno", "stream_nonzero": "*quic.StreamError",
                                              "application": "*qerr.ApplicationError", "wrapped": "*fmt.wrapError",
                                              "joined": "*errors.joinError", "sentinel": "*errors.errorString"}[mode]
                    observed["native_close_error_classification"] = "known_sentinel_error" if mode == "sentinel" else "public_typed_error"
                elif mode == "foreign_stream":
                    observed["send_context"]["native_stream_id"] = 4
                elif mode == "nonzero_context":
                    observed["send_context"]["error_code"] = 7
                elif mode == "remote_read_without_send_context":
                    observed["send_context"] = deepcopy(observed["send_context_at_prepare"])
                elif mode == "future_reset":
                    reset.update(started_order=24, returned_order=25)
                    if outcome == "remote_close":
                        # A later Reset is disposal only, never a cause for Close; remove the actual send witness instead.
                        observed["send_context"]["cause_type"] = "*fmt.wrapError"
                elif mode == "failed_reset":
                    reset.update(outcome="error", error="synthetic Reset failure", error_type="syscall.Errno", typed_cause="opaque")
                else:
                    artifact["raw"]["sink"]["error"] = "sticky earlier native I/O error"
                with self.subTest(outcome=outcome, mode=mode), self.assertRaises(ValueError):
                    validate_case(artifact)

    def test_lower_quic_terminal_requires_exact_phase_cause_disposal_and_join(self):
        mutations = (
            lambda a, r, o, c, f, v: o.update(error_type="*fmt.wrapError"),
            lambda a, r, o, c, f, v: o.update(error_code=True),
            lambda a, r, o, c, f, v: o.update(transport_error_code=5),
            lambda a, r, o, c, f, v: o.update(transport_error_remote=not o["remote"]),
            lambda a, r, o, c, f, v: o.update(transport_native_stream_id=4),
            lambda a, r, o, c, f, v: o.update(pending_frame_bytes=1),
            lambda a, r, o, c, f, v: o.update(terminal_prepare_ack_sequence=0),
            lambda a, r, o, c, f, v: o.update(terminal_prepare_ack_sequence=True),
            lambda a, r, o, c, f, v: o.update(prepare_baseline_present=False),
            lambda a, r, o, c, f, v: o["connection_context_at_prepare"].update(done=True),
            lambda a, r, o, c, f, v: r.update(prepare_ack_sequence=0),
            lambda a, r, o, c, f, v: r.update(operation="stream_close_read"),
            lambda a, r, o, c, f, v: r.update(native_stream_id=4),
            lambda a, r, o, c, f, v: r.update(started_order=o["returned_order"] + 1, returned_order=o["returned_order"] + 2),
            lambda a, r, o, c, f, v: f.update(owner_disposal_receipt_sequence=o["sequence"]),
            lambda a, r, o, c, f, v: f.update(io_joined=False),
            lambda a, r, o, c, f, v: f.update(pending_read_frame_bytes=1),
            lambda a, r, o, c, f, v: v.update(operation_receipt_sequence=True),
            lambda a, r, o, c, f, v: v.update(operation_receipt_sequence=[]),
            lambda a, r, o, c, f, v: v.update(accepted=False),
            lambda a, r, o, c, f, v: v.update(framing_receipt_sequence=o["sequence"]),
            lambda a, r, o, c, f, v: v.update(owner_disposal_receipt_sequence=0),
            lambda a, r, o, c, f, v: v.update(owner_disposal_receipt_sequence=float(r["sequence"])),
            lambda a, r, o, c, f, v: a["raw"]["sink"].update(joined=False),
            lambda a, r, o, c, f, v: next(event for event in a["raw"]["sink"]["events"] if event["kind"] == "native_quic_join").update(active_native_calls=1),
        )
        for mutate in mutations:
            artifact = synthetic_case()
            records = self.lower_terminal_events(artifact, "owned_reset_pending", direction="write")
            mutate(artifact, *records)
            with self.subTest(mutation=mutate), self.assertRaises(ValueError):
                validate_case(artifact)
        for missing in ("native_stream_operation", "native_quic_framing_finalized", "native_quic_terminal_finalized", "native_quic_join"):
            artifact = synthetic_case()
            self.lower_terminal_events(artifact, "owned_read_terminal_pending")
            events = artifact["raw"]["sink"]["events"]
            events[:] = [value for value in events if value["kind"] != missing]
            with self.subTest(missing=missing), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_lower_peer_zero_read_requires_live_baseline_and_other_actor_prepare(self):
        for mode in ("early_send", "early_connection", "missing_row", "foreign_row", "duplicate_row", "boolean_id",
                     "remote_false", "invented_reason", "missing_peer_ack", "foreign_peer_ack", "duplicate_final"):
            artifact = synthetic_case()
            _, observed, _, _, final = self.lower_terminal_events(artifact, "peer_zero_reset_pending")
            raw = artifact["raw"]["sink"]
            ack = next(value for value in raw["events"] if value["kind"] == "shutdown_prepared")
            rows = ack["native_quic_context_snapshots"]
            if mode == "early_send":
                rows[0]["send_context"].update(done=True, cause_type="*quic.StreamError", error_code=0, remote=True,
                                               native_stream_id=0, error="synthetic early send terminal")
                observed["send_context_at_prepare"] = deepcopy(rows[0]["send_context"])
            elif mode == "early_connection":
                rows[0]["connection_context"].update(done=True)
            elif mode == "missing_row":
                rows.clear()
            elif mode == "foreign_row":
                rows[0]["native_connection_id"] = "foreign"
            elif mode == "duplicate_row":
                rows.append(deepcopy(rows[0]))
            elif mode == "boolean_id":
                rows[0]["native_stream_id"] = False
            elif mode == "remote_false":
                observed.update(remote=False, transport_error_remote=False)
            elif mode == "invented_reason":
                observed["peer_reset_reason"] = "prepared_remote_cleanup"
            elif mode in {"missing_peer_ack", "foreign_peer_ack"}:
                peer = artifact["raw"]["replacement"]
                peer_ack = next(value for value in peer["events"] if value["kind"] == "shutdown_prepared")
                if mode == "missing_peer_ack":
                    peer["events"].remove(peer_ack)
                else:
                    peer_ack["local_peer_id"] = peer_id(99)
            else:
                raw["events"].append({**deepcopy(final), "sequence": len(raw["events"]) + 1, "mono_ns": len(raw["events"]) + 1})
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                validate_case(artifact)

    def terminal_event(self, artifact, *, pending=0, kind="stream_io_terminal"):
        raw = artifact["raw"]["sink"]
        event = {"sequence": len(raw["events"]) + 1, "mono_ns": len(raw["events"]) + 1,
                 "kind": kind, "source": "go.network.Stream.read" if kind == "stream_io_terminal"
                 else "go.pubsub.native_stream.finalize", "direction": "read", "pending_frame_bytes": pending,
                 "successful_prefix_bytes": 0, "error": "EOF", "connection_id": "replacement",
                 "stream_id": "replacement", "remote_peer_id": artifact["raw"]["replacement"]["local_peer_id"],
                 "protocol": "/meshsub/1.1.0"}
        if kind == "incomplete_rpc_frame":
            event.update(operation="stream_reset", typed_cause="incomplete_rpc_frame", prepared=True)
        else:
            event.update(operation="stream_read", started_order=1, returned_order=2, prepared=True,
                         native_yamux=True, reset_receipt_sequence=0, reset_returned_order=0,
                         prepare_ack_sequence=next(value["sequence"] for value in raw["events"] if value["kind"] == "shutdown_prepared"),
                         reset_started_order=0, transport_error_type=None, transport_error_code=None, transport_error_remote=None,
                         requested_reset_code=None, outcome="eof", typed_cause="io_eof",
                         error_type="*errors.errorString", error_code=None, remote=None)
        raw["events"].append(event)
        return event

    def owned_reset_events(self, artifact, *, direction="read", io_first=True, repeat_close=False, remote=False,
                           late_reset_publication=False):
        """Synthetic causal receipts only; never exported as native acceptance."""
        artifact["case"]["profile"] = "native_tcp_yamux"
        for actor in artifact["raw"].values():
            for event in actor["events"]:
                if event["kind"] == "connection":
                    event.update(transport="tcp", security="/noise", muxer="/yamux/1.0.0", remote_address="/ip4/127.0.0.1/tcp/1")
                    if actor["implementation"] == "go":
                        event["authentication_basis"] = "native_TCP_ConnState_and_authenticated_RemotePublicKey"
        raw = artifact["raw"]["sink"]
        ack = next(event["sequence"] for event in raw["events"] if event["kind"] == "shutdown_prepared")
        base = {"connection_id": "replacement", "stream_id": "replacement",
                "remote_peer_id": artifact["raw"]["replacement"]["local_peer_id"], "protocol": "/meshsub/1.1.0",
                "native_yamux": True, "prepared": True, "prepare_ack_sequence": ack,
                "reset_receipt_sequence": 0, "reset_returned_order": 0, "reset_started_order": 0,
                "requested_reset_code": None, "outcome": "ok", "typed_cause": "none", "error": None,
                "error_type": None, "error_code": None, "remote": None,
                "transport_error_type": None, "transport_error_code": None, "transport_error_remote": None}

        def append(kind, fields):
            source = "go.network.Stream." + direction if kind == "stream_io_terminal" else "go.pubsub.native_stream.native_operation"
            event = {**fields, "sequence": len(raw["events"]) + 1, "mono_ns": len(raw["events"]) + 1,
                     "kind": kind, "source": source}
            raw["events"].append(event)
            return event

        reset_fields = {**base, "operation": "stream_reset", "started_order": 2, "returned_order": 4 if io_first else 3}
        terminal = {**base, "operation": "stream_" + direction, "started_order": 1,
                    "returned_order": 3 if io_first else 4, "prepared": False, "prepare_ack_sequence": 0,
                    "reset_started_order": 2, "outcome": "owned_reset_pending", "typed_cause": "libp2p_stream_error",
                    "error_type": "*network.StreamError", "error_code": 0, "remote": False,
                    "transport_error_type": "*yamux.StreamError", "transport_error_code": 0, "transport_error_remote": False,
                    "direction": direction, "successful_prefix_bytes": 0, "pending_frame_bytes": 0, "error": "native local reset"}
        if repeat_close:
            reset_fields.update(started_order=1, returned_order=2)
            terminal = {**base, "operation": "stream_close", "started_order": 3, "returned_order": 4,
                        "outcome": "repeat_close_pending", "typed_cause": "yamux_stream_error", "error_type": "*yamux.StreamError",
                        "error_code": 0, "remote": remote, "error": "native cached reset",
                        "reset_started_order": 1, "reset_returned_order": 2, "reset_receipt_sequence": 0}
            if late_reset_publication:
                observed = append("native_stream_operation", terminal)
                reset = append("native_stream_operation", reset_fields)
            else:
                reset = append("native_stream_operation", reset_fields)
                terminal["reset_receipt_sequence"] = reset["sequence"]
                observed = append("native_stream_operation", terminal)
        elif io_first:
            observed = append("stream_io_terminal", terminal)
            reset = append("native_stream_operation", reset_fields)
        else:
            reset = append("native_stream_operation", reset_fields)
            observed = append("stream_io_terminal", terminal)
        resolution = append("native_stream_close_finalized" if repeat_close else "native_stream_io_finalized",
                            {**terminal, "operation_receipt_sequence": observed["sequence"], "accepted": True,
                             "causal_reset_receipt_sequence": reset["sequence"], "causal_reset_returned_order": reset["returned_order"],
                             "owner_disposal_receipt_sequence": reset["sequence"],
                             "io_joined": True, "native_owner_disposed": True, "read_finalized": True, "write_finalized": True,
                             "pending_read_frame_bytes": 0, "pending_write_frame_bytes": 0, "framing_clean": True})
        return reset, observed, resolution

    def peer_zero_reset_events(self, artifact, *, direction="read"):
        """Synthetic unknown peer cancellation plus independently observed full disposal."""
        reset, observed, resolution = self.owned_reset_events(artifact, direction=direction)
        raw = artifact["raw"]["sink"]
        ack = next(event["sequence"] for event in raw["events"] if event["kind"] == "shutdown_prepared")
        changes = {"outcome": "peer_zero_reset_pending", "remote": True, "transport_error_remote": True,
                   "reset_started_order": 0, "terminal_prepare_ack_sequence": ack, "peer_reset_reason": "unknown",
                   "error": "native peer reset"}
        observed.update(changes)
        resolution.update(changes, operation_receipt_sequence=observed["sequence"])
        resolution.pop("causal_reset_receipt_sequence")
        resolution.pop("causal_reset_returned_order")
        return observed, resolution

    def concurrent_reset_close_events(self, artifact, *, late_publication=False, reset_return_first=False):
        """Synthetic overlap/disposal observation, never a causal or native acceptance result."""
        reset, observed, final = self.owned_reset_events(artifact, repeat_close=True, late_reset_publication=late_publication)
        reset["returned_order"] = 3 if reset_return_first else 4
        changes = {"outcome": "concurrent_reset_close_pending", "started_order": 2,
                   "returned_order": 4 if reset_return_first else 3, "reset_receipt_sequence": 0,
                   "reset_returned_order": 0, "terminal_state_cause": "unknown", "native_close_succeeded": False}
        observed.update(changes)
        final.update(changes, observed_reset_receipt_sequence=reset["sequence"],
                     observed_reset_returned_order=reset["returned_order"])
        final.pop("causal_reset_receipt_sequence")
        final.pop("causal_reset_returned_order")
        return reset, observed, final

    def owned_read_terminal_events(self, artifact, *, late_publication=False):
        """Synthetic sentinel observation; Closed/Reset is not a physical-cause claim."""
        close, observed, resolution = self.owned_reset_events(artifact, io_first=late_publication)
        raw = artifact["raw"]["sink"]
        close.update(operation="stream_close_read", returned_order=3)
        changes = {"outcome": "owned_read_terminal_pending", "typed_cause": "yamux_reset_sentinel_pair",
                   "returned_order": 4, "reset_started_order": 0, "error_type": "*fmt.wrapErrors",
                   "error_code": None, "remote": None, "transport_error_type": None,
                   "transport_error_code": None, "transport_error_remote": None,
                   "terminal_state_cause": "unknown", "terminal_started_order": 2, "terminal_returned_order": 3,
                   "terminal_receipt_sequence": 0 if late_publication else close["sequence"],
                   "error": "stream reset: stream reset"}
        observed.update(changes)
        resolution.update(changes, observed_terminal_receipt_sequence=close["sequence"])
        resolution.pop("causal_reset_receipt_sequence")
        resolution.pop("causal_reset_returned_order")
        disposal = {**close, "operation": "stream_reset", "started_order": 5, "returned_order": 6}
        raw["events"].insert(raw["events"].index(resolution), disposal)
        for sequence, event in enumerate(raw["events"], 1):
            event.update(sequence=sequence, mono_ns=sequence)
        resolution["owner_disposal_receipt_sequence"] = disposal["sequence"]
        return close, observed, disposal, resolution

    @staticmethod
    def insert_sink_events(artifact, index, added):
        """Resequence synthetic facts while retaining exact indexed native references."""
        events = artifact["raw"]["sink"]["events"]
        fields = {"prepare_ack_sequence", "terminal_prepare_ack_sequence", "reset_receipt_sequence",
                  "operation_receipt_sequence", "causal_reset_receipt_sequence", "owner_disposal_receipt_sequence",
                  "terminal_receipt_sequence", "observed_terminal_receipt_sequence", "observed_reset_receipt_sequence", "connection_receipt_sequence",
                  "native_stream_receipt_sequence", "framing_receipt_sequence", "connection_context_receipt_sequence",
                  "send_context_receipt_sequence", "owned_terminal_receipt_sequence", "prepare_snapshot_ack_sequence",
                  "lower_connection_receipt_sequence", "score_observation_sequence", "candidate_write_receipt_sequence",
                  "candidate_protocol_frame_sequence", "pending_receipt_sequence", "native_join_receipt_sequence",
                  "negotiation_cleanup_pending_receipt_sequence", "native_call_begin_prepare_ack_sequence",
                  "peer_header_frame_sequence", "cleanup_owner_disposal_receipt_sequence", "first_owner_disposal_receipt_sequence"}
        references = [(event, key, events[event[key] - 1]) for event in events + added for key in fields
                      if type(event.get(key)) is int and event[key] > 0]
        lists = [(value, key, [events[ref - 1] for ref in value[key]]) for event in events + added
                 for value in (event, event.get("negotiation_snapshot", {})) for key in ("negotiation_frame_sequences", "frame_sequences")
                 if key in value]
        joins = [(row, events[row["shutdown_event_sequence"] - 1])
                 for row in artifact["shutdown_barrier"]["operations"] if row["kind"] == "donor_joined" and row["actor"] == "sink"]
        quiesced = [(row, events[row["quiesce_event_sequence"] - 1])
                    for row in artifact["shutdown_barrier"]["operations"] if row["kind"] == "quiesce_ack" and row["actor"] == "sink"]
        events[index:index] = added
        for sequence, event in enumerate(events, 1):
            event.update(sequence=sequence, mono_ns=sequence)
        for event, key, target in references:
            event[key] = target["sequence"]
        for value, key, targets in lists:
            value[key] = [target["sequence"] for target in targets]
        for row, target in joins:
            row["shutdown_event_sequence"] = target["sequence"]
        ack = next(event for event in events if event["kind"] == "shutdown_prepared")
        row = next(row for row in artifact["shutdown_barrier"]["operations"] if row["kind"] == "prepare_ack" and row["actor"] == "sink")
        row["ack_event_sequence"] = ack["sequence"]
        for row in artifact["shutdown_barrier"]["operations"]:
            if row["actor"] == "sink" and row["kind"] in {"quiesce_requested", "quiesce_ack"}:
                row["prepare_ack_sequence"] = ack["sequence"]
        for row, target in quiesced:
            row["quiesce_event_sequence"] = target["sequence"]

    def terminal_only_events(self, artifact, outcome, *, private=False):
        """Keep all RPC carriers intact; use a separate real-shaped terminal/disposal owner."""
        if outcome == "owned_read":
            records = self.owned_read_terminal_events(artifact, late_publication=True)
        elif outcome == "peer_zero":
            self.peer_zero_reset_events(artifact)
            records = tuple(event for event in artifact["raw"]["sink"]["events"]
                            if event["kind"] in {"stream_io_terminal", "native_stream_operation", "native_stream_io_finalized"})
        elif outcome == "concurrent_close":
            records = self.concurrent_reset_close_events(artifact, late_publication=True)
        else:
            records = self.owned_reset_events(artifact, repeat_close=outcome == "repeat_close", late_reset_publication=True)
        events = artifact["raw"]["sink"]["events"]
        connection = deepcopy(next(event for event in events if event["kind"] == "connection"))
        selected = deepcopy(next(event for event in events if event["kind"] == "protocol"))
        connection["connection_id"] = "terminal-only-carrier"
        selected.update(connection_id=connection["connection_id"], stream_id="terminal-only-stream")
        for event in records:
            event.update(connection_id=connection["connection_id"], stream_id=selected["stream_id"])
        index = next(event["sequence"] for event in events if event["kind"] == "shutdown_prepared") - 1
        self.insert_sink_events(artifact, index, [connection, selected])
        if private:
            artifact["case"]["profile"] = "private_tcp_yamux"
            for raw in artifact["raw"].values():
                for event in raw["events"]:
                    if event["kind"] == "connection":
                        event.update(pnet_verified=True, pnet_fingerprint="b" * 64)
        return connection, selected, records

    def assert_rpc_owners_valid(self, artifact, fingerprint=None):
        protocol = "/meshsub/" + artifact["case"]["version"] + ".0"
        transport = PROFILES[artifact["case"]["profile"]]
        for raw in artifact["raw"].values():
            for event in raw["events"]:
                if event["kind"] == "rpc":
                    _owner(raw["events"], event, event["peer_id"], protocol, transport, fingerprint)

    def test_terminal_only_carriers_use_canonical_owner_without_requiring_rpc(self):
        for outcome in ("owned_reset", "peer_zero", "owned_read", "repeat_close", "concurrent_close"):
            for private in (False, True):
                artifact = synthetic_case(lower_quic=False)
                connection, _, _ = self.terminal_only_events(artifact, outcome, private=private)
                original = deepcopy(artifact)
                with self.subTest(outcome=outcome, private=private):
                    self.assertFalse(any(event["kind"] == "rpc" and event["connection_id"] == connection["connection_id"]
                                         for event in artifact["raw"]["sink"]["events"]))
                    validate_case(artifact, expected_fingerprint="b" * 64 if private else None)
                    self.assertTrue(_same_json(original, artifact))

    def test_terminal_only_carrier_cannot_borrow_valid_rpc_authentication_or_profile(self):
        mutations = ("profile", "authentication_basis", "missing_address", "address", "address_peer", "protocol",
                     "fingerprint", "missing_fingerprint", "unverified_pnet", "boolean_pnet")
        for outcome in ("owned_reset", "peer_zero", "owned_read", "repeat_close", "concurrent_close"):
            for mutation in mutations:
                private = mutation in {"fingerprint", "missing_fingerprint", "unverified_pnet", "boolean_pnet"}
                artifact = synthetic_case(lower_quic=False)
                connection, selected, records = self.terminal_only_events(artifact, outcome, private=private)
                fingerprint = "b" * 64 if private else None
                validate_case(artifact, expected_fingerprint=fingerprint)
                if mutation == "profile":
                    artifact["case"]["profile"] = "native_quic"
                    for raw in artifact["raw"].values():
                        for event in raw["events"]:
                            if event["kind"] == "connection" and event is not connection:
                                event.update(transport="quic", security="/tls/1.0.0", muxer="quic",
                                             remote_address="/ip4/127.0.0.1/udp/1/quic-v1",
                                             authentication_basis=AUTHENTICATION[raw["implementation"]]["quic"])
                elif mutation == "authentication_basis":
                    connection["authentication_basis"] = "configured_Noise"
                elif mutation == "missing_address":
                    connection.pop("remote_address")
                elif mutation == "address":
                    connection["remote_address"] = "/ip4/127.0.0.1/udp/1/quic-v1"
                elif mutation == "address_peer":
                    connection["remote_address"] += "/p2p/" + artifact["raw"]["offender"]["local_peer_id"]
                elif mutation == "protocol":
                    selected["protocol"] = "/meshsub/1.0.0"
                    for event in records:
                        event["protocol"] = selected["protocol"]
                elif mutation == "fingerprint":
                    connection["pnet_fingerprint"] = "c" * 64
                elif mutation == "missing_fingerprint":
                    connection.pop("pnet_fingerprint")
                elif mutation == "unverified_pnet":
                    connection["pnet_verified"] = False
                else:
                    connection["pnet_verified"] = 1
                with self.subTest(outcome=outcome, mutation=mutation):
                    self.assert_rpc_owners_valid(artifact, fingerprint)
                    with self.assertRaises(ValueError):
                        validate_case(artifact, expected_fingerprint=fingerprint)

    def test_terminal_only_owner_indexes_reject_identical_and_conflicting_duplicates(self):
        for outcome in ("owned_reset", "peer_zero", "owned_read", "repeat_close", "concurrent_close"):
            for kind in ("connection", "protocol"):
                for conflicting in (False, True):
                    for late in (False, True):
                        artifact = synthetic_case(lower_quic=False)
                        connection, selected, records = self.terminal_only_events(artifact, outcome)
                        duplicate = deepcopy(connection if kind == "connection" else selected)
                        if conflicting:
                            duplicate["peer_id"] = artifact["raw"]["offender"]["local_peer_id"]
                            if kind == "connection":
                                duplicate["authenticated"] = False
                            else:
                                duplicate["protocol"] = "/meshsub/1.0.0"
                        events = artifact["raw"]["sink"]["events"]
                        index = len(events) if late else min(event["sequence"] for event in records) - 1
                        self.insert_sink_events(artifact, index, [duplicate])
                        with self.subTest(outcome=outcome, kind=kind, conflicting=conflicting, late=late):
                            self.assert_rpc_owners_valid(artifact)
                            with self.assertRaisesRegex(ValueError, "ambiguous native " + ("connection" if kind == "connection" else "stream") + " owner index"):
                                validate_case(artifact)

    def test_owned_read_terminal_requires_native_return_not_publication_order(self):
        for late in (False, True):
            with self.subTest(late_publication=late):
                artifact = synthetic_case(lower_quic=False)
                close, observed, disposal, resolution = self.owned_read_terminal_events(artifact, late_publication=late)
                original = deepcopy(artifact)
                validate_case(artifact)
                self.assertTrue(_same_json(original, artifact))
                self.assertIs(observed["prepared"], False)
                self.assertLess(close["returned_order"], observed["returned_order"])
                self.assertLess(observed["returned_order"], disposal["started_order"])
                self.assertEqual(resolution["terminal_state_cause"], "unknown")
                self.assertNotIn("causal_reset_receipt_sequence", resolution)

    def test_owned_read_terminal_rejects_unowned_unprepared_or_superseded_observation(self):
        mutations = [
            lambda a, c, o, d, f: o.update(error_type="*errors.joinError"),
            lambda a, c, o, d, f: o.update(typed_cause="opaque"),
            lambda a, c, o, d, f: o.update(error_code=0),
            lambda a, c, o, d, f: o.update(remote=False),
            lambda a, c, o, d, f: o.update(direction="write", operation="stream_write"),
            lambda a, c, o, d, f: o.update(terminal_state_cause="graceful"),
            lambda a, c, o, d, f: o.update(terminal_started_order=True),
            lambda a, c, o, d, f: o.update(terminal_returned_order=o["returned_order"]),
            lambda a, c, o, d, f: o.update(terminal_receipt_sequence=True),
            lambda a, c, o, d, f: o.update(terminal_receipt_sequence=d["sequence"]),
            lambda a, c, o, d, f: c.update(operation="stream_close_write"),
            lambda a, c, o, d, f: c.update(prepared=False, prepare_ack_sequence=0),
            lambda a, c, o, d, f: c.update(stream_id="foreign"),
            lambda a, c, o, d, f: c.update(returned_order=8),
            lambda a, c, o, d, f: f.update(observed_terminal_receipt_sequence=True),
            lambda a, c, o, d, f: f.update(observed_terminal_receipt_sequence=d["sequence"]),
            lambda a, c, o, d, f: f.update(terminal_state_cause="local close caused it"),
            lambda a, c, o, d, f: f.update(causal_reset_receipt_sequence=d["sequence"]),
            lambda a, c, o, d, f: d.update(started_order=3, returned_order=5),
            lambda a, c, o, d, f: a["raw"]["sink"].update(error="earlier sticky native error"),
            lambda a, c, o, d, f: a["raw"]["sink"].update(overflow=True),
        ]
        for mutation in mutations:
            artifact = synthetic_case(lower_quic=False)
            records = self.owned_read_terminal_events(artifact)
            mutation(artifact, *records)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_every_pending_terminal_requires_indexed_full_owner_disposal(self):
        for outcome in ("owned_reset", "peer_zero", "owned_read"):
            for mutation in ("missing", "boolean", "future", "half_close", "foreign", "failed"):
                artifact = synthetic_case(lower_quic=False)
                if outcome == "peer_zero":
                    observed, resolution = self.peer_zero_reset_events(artifact)
                elif outcome == "owned_read":
                    _, observed, _, resolution = self.owned_read_terminal_events(artifact)
                else:
                    _, observed, resolution = self.owned_reset_events(artifact)
                raw = artifact["raw"]["sink"]
                disposal = raw["events"][resolution["owner_disposal_receipt_sequence"] - 1]
                if mutation == "missing":
                    resolution.pop("owner_disposal_receipt_sequence")
                elif mutation == "boolean":
                    resolution["owner_disposal_receipt_sequence"] = True
                elif mutation == "future":
                    resolution["owner_disposal_receipt_sequence"] = resolution["sequence"]
                elif mutation == "half_close":
                    disposal["operation"] = "stream_close_read"
                elif mutation == "foreign":
                    disposal["remote_peer_id"] = artifact["raw"]["offender"]["local_peer_id"]
                else:
                    disposal.update(outcome="error", error="native disposal failed", typed_cause="opaque")
                with self.subTest(outcome=outcome, mutation=mutation), self.assertRaises(ValueError):
                    validate_case(artifact)

    def test_peer_zero_reset_is_unknown_cancellation_after_return_ack_not_begin(self):
        for direction in ("read", "write"):
            with self.subTest(direction=direction):
                artifact = synthetic_case(lower_quic=False)
                observed, resolution = self.peer_zero_reset_events(artifact, direction=direction)
                original = deepcopy(artifact)
                validate_case(artifact)
                self.assertTrue(_same_json(original, artifact))
                self.assertIs(observed["prepared"], False)
                self.assertEqual(observed["prepare_ack_sequence"], 0)
                self.assertLess(observed["terminal_prepare_ack_sequence"], observed["sequence"])
                self.assertEqual(resolution["peer_reset_reason"], "unknown")
                self.assertNotIn("causal_reset_receipt_sequence", resolution)

    def test_peer_zero_reset_requires_exact_type_owner_return_ack_and_clean_finalizer(self):
        mutations = [
            lambda a, o, f: o.update(remote=False),
            lambda a, o, f: o.update(remote=1),
            lambda a, o, f: o.update(error_code=7),
            lambda a, o, f: o.update(error_code=False),
            lambda a, o, f: o.update(error_type="*errors.joinError"),
            lambda a, o, f: o.update(transport_error_remote=False),
            lambda a, o, f: o.update(transport_error_remote=1),
            lambda a, o, f: o.update(transport_error_code=7),
            lambda a, o, f: o.update(transport_error_type="*quic.StreamError"),
            lambda a, o, f: o.update(transport_error_type="*net.OpError", error="TCP ECONNRESET"),
            lambda a, o, f: o.update(transport_error_type="*fmt.wrapError"),
            lambda a, o, f: o.update(terminal_prepare_ack_sequence=0),
            lambda a, o, f: o.update(terminal_prepare_ack_sequence=True),
            lambda a, o, f: o.update(terminal_prepare_ack_sequence=float(o["terminal_prepare_ack_sequence"])),
            lambda a, o, f: o.update(peer_reset_reason="remote cleanup"),
            lambda a, o, f: o.update(reset_started_order=2),
            lambda a, o, f: o.update(reset_receipt_sequence=1),
            lambda a, o, f: o.update(remote_peer_id=a["raw"]["offender"]["local_peer_id"]),
            lambda a, o, f: o.update(stream_id="foreign"),
            lambda a, o, f: o.update(native_yamux=False),
            lambda a, o, f: o.update(pending_frame_bytes=1),
            lambda a, o, f: f.update(causal_reset_receipt_sequence=1),
            lambda a, o, f: f.update(operation_receipt_sequence=True),
            lambda a, o, f: f.update(io_joined=False),
            lambda a, o, f: f.update(native_owner_disposed=False),
            lambda a, o, f: f.update(write_finalized=False),
            lambda a, o, f: f.update(framing_clean=False),
            lambda a, o, f: f.update(pending_write_frame_bytes=1),
            lambda a, o, f: f.update(terminal_prepare_ack_sequence=0),
            lambda a, o, f: a["raw"]["sink"].update(error="sticky earlier native failure"),
            lambda a, o, f: a["raw"]["sink"].update(overflow=True),
            lambda a, o, f: a["raw"]["sink"]["events"].remove(f),
        ]
        for mutation in mutations:
            artifact = synthetic_case(lower_quic=False)
            records = self.peer_zero_reset_events(artifact)
            mutation(artifact, *records)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_case(artifact)
        artifact = synthetic_case(lower_quic=False)
        observed, resolution = self.peer_zero_reset_events(artifact)
        values = artifact["raw"]["sink"]["events"]
        values.append({**resolution, "sequence": len(values) + 1, "mono_ns": len(values) + 1})
        with self.assertRaisesRegex(ValueError, "duplicate"):
            validate_case(artifact)

    def test_peer_zero_reset_cannot_borrow_future_ack_or_missing_foreign_four_actor_barrier(self):
        artifact = synthetic_case(lower_quic=False)
        observed, resolution = self.peer_zero_reset_events(artifact)
        raw = artifact["raw"]["sink"]
        values = raw["events"]
        ack = next(event for event in values if event["kind"] == "shutdown_prepared")
        values.remove(observed)
        values.insert(values.index(ack), observed)
        for sequence, event in enumerate(values, 1):
            event.update(sequence=sequence, mono_ns=sequence)
        observed["terminal_prepare_ack_sequence"] = ack["sequence"]
        resolution.update(terminal_prepare_ack_sequence=ack["sequence"], operation_receipt_sequence=observed["sequence"])
        row = next(row for row in artifact["shutdown_barrier"]["operations"][:4] if row["actor"] == "sink")
        row["ack_event_sequence"] = ack["sequence"]
        with self.assertRaisesRegex(ValueError, "preparation-at-return"):
            validate_case(artifact)

        for mode in ("missing", "foreign", "early_stop"):
            artifact = synthetic_case(lower_quic=False)
            self.peer_zero_reset_events(artifact)
            if mode == "early_stop":
                rows = artifact["shutdown_barrier"]["operations"]
                rows[3], rows[4] = rows[4], rows[3]
            else:
                values = artifact["raw"]["replacement"]["events"]
                ack = next(event for event in values if event["kind"] == "shutdown_prepared")
                if mode == "foreign":
                    ack["actor"] = "foreign"
                else:
                    values.remove(ack)
                    for sequence, event in enumerate(values, 1):
                        event.update(sequence=sequence, mono_ns=sequence)
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                validate_case(artifact)

        artifact = synthetic_case(lower_quic=False)
        self.peer_zero_reset_events(artifact)
        raw = artifact["raw"]["sink"]
        row = next(row for row in artifact["shutdown_barrier"]["operations"][:4] if row["actor"] == "sink")
        captured = deepcopy(raw)
        captured.update(finalized=False, joined=False)
        snapshot = {"schema_version": 1, "source": "python.fixture.native_prepare_snapshot", "actor": "sink",
                    "case_token": artifact["case_token"], "pid": 4, "command_sequence": row["command_sequence"],
                    "ack_event_sequence": row["ack_event_sequence"], "result": captured}
        with self.assertRaisesRegex(ValueError, "peer cancellation"):
            prepared_snapshot(snapshot, row, raw, 4)

    def test_owned_reset_io_allows_preprepare_begin_and_either_return_order(self):
        for direction in ("read", "write"):
            for io_first in (False, True):
                with self.subTest(direction=direction, io_first=io_first):
                    artifact = synthetic_case(lower_quic=False)
                    reset, observed, resolution = self.owned_reset_events(artifact, direction=direction, io_first=io_first)
                    original = deepcopy(artifact)
                    validate_case(artifact)
                    self.assertTrue(_same_json(original, artifact))
                    self.assertIs(observed["prepared"], False)
                    self.assertEqual(observed["prepare_ack_sequence"], 0)
                    self.assertEqual(reset["returned_order"] > observed["returned_order"], io_first)
                    raw = artifact["raw"]["sink"]
                    row = next(row for row in artifact["shutdown_barrier"]["operations"][:4] if row["actor"] == "sink")
                    captured = deepcopy(raw)
                    captured.update(finalized=False, joined=False, events=deepcopy(raw["events"][:row["ack_event_sequence"] + 1]))
                    snapshot = {"schema_version": 1, "source": "python.fixture.native_prepare_snapshot", "actor": "sink",
                                "case_token": artifact["case_token"], "pid": 4, "command_sequence": 1,
                                "ack_event_sequence": row["ack_event_sequence"], "result": captured}
                    prepared_snapshot(snapshot, row, raw, 4)

    def test_owned_reset_io_rejects_cause_phase_owner_order_and_framing_forgery(self):
        mutations = [
            lambda a, r, o, f: o.update(outcome="error"),
            lambda a, r, o, f: o.update(error_type="*errors.errorString", typed_cause="opaque"),
            lambda a, r, o, f: o.update(error_type="*errors.joinError"),
            lambda a, r, o, f: o.update(error_code=7),
            lambda a, r, o, f: o.update(error_code=False),
            lambda a, r, o, f: o.update(remote=True),
            lambda a, r, o, f: o.update(transport_error_type=None, transport_error_code=None, transport_error_remote=None),
            lambda a, r, o, f: o.update(transport_error_type="*net.OpError", error="native TCP reset by peer"),
            lambda a, r, o, f: o.update(transport_error_type="*fmt.wrapError"),
            lambda a, r, o, f: o.update(transport_error_type="*errors.joinError"),
            lambda a, r, o, f: o.update(transport_error_code=7),
            lambda a, r, o, f: o.update(transport_error_remote=True),
            lambda a, r, o, f: o.update(reset_started_order=5),
            lambda a, r, o, f: o.update(pending_frame_bytes=1),
            lambda a, r, o, f: r.update(operation="stream_reset_with_error", requested_reset_code=0),
            lambda a, r, o, f: r.update(outcome="error", error="native Reset failure", typed_cause="opaque", error_type="*errors.errorString"),
            lambda a, r, o, f: r.update(prepared=False, prepare_ack_sequence=0),
            lambda a, r, o, f: r.update(prepare_ack_sequence=True),
            lambda a, r, o, f: r.update(prepare_ack_sequence=1),
            lambda a, r, o, f: r.update(started_order=5, returned_order=6),
            lambda a, r, o, f: r.update(stream_id="foreign"),
            lambda a, r, o, f: f.update(causal_reset_receipt_sequence=o["sequence"]),
            lambda a, r, o, f: f.update(causal_reset_receipt_sequence=True),
            lambda a, r, o, f: f.update(causal_reset_returned_order=4.0),
            lambda a, r, o, f: f.update(accepted=False),
            lambda a, r, o, f: f.update(io_joined=False),
            lambda a, r, o, f: f.update(native_owner_disposed=False),
            lambda a, r, o, f: f.update(read_finalized=False),
            lambda a, r, o, f: f.update(pending_write_frame_bytes=1),
            lambda a, r, o, f: f.update(framing_clean=False),
            lambda a, r, o, f: f.update(error="rewritten native outcome"),
            lambda a, r, o, f: a["raw"]["sink"].update(error="sticky earlier native failure"),
            lambda a, r, o, f: a["raw"]["sink"].update(joined=False),
            lambda a, r, o, f: a["raw"]["sink"]["events"].pop(),
        ]
        for mutation in mutations:
            artifact = synthetic_case(lower_quic=False)
            records = self.owned_reset_events(artifact)
            mutation(artifact, *records)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_case(artifact)
        artifact = synthetic_case(lower_quic=False)
        reset, observed, resolution = self.owned_reset_events(artifact)
        values = artifact["raw"]["sink"]["events"]
        values.append({**resolution, "sequence": len(values) + 1, "mono_ns": len(values) + 1})
        with self.assertRaises(ValueError):
            validate_case(artifact)

        artifact = synthetic_case(lower_quic=False)
        reset, observed, resolution = self.owned_reset_events(artifact)
        values = artifact["raw"]["sink"]["events"]
        protocol = next(event for event in values if event["kind"] == "protocol" and event["stream_id"] == "replacement")
        values.insert(observed["sequence"] - 1, {**protocol, "stream_id": "foreign"})
        for sequence, event in enumerate(values, 1):
            event.update(sequence=sequence, mono_ns=sequence)
        reset["stream_id"] = "foreign"
        resolution.update(operation_receipt_sequence=observed["sequence"], causal_reset_receipt_sequence=reset["sequence"],
                          owner_disposal_receipt_sequence=reset["sequence"])
        with self.assertRaisesRegex(ValueError, "exact wrapper"):
            validate_case(artifact)

    def test_repeat_close_is_distinct_from_remote_io_and_requires_returned_own_reset(self):
        for remote in (False, True):
            artifact = synthetic_case(lower_quic=False)
            self.owned_reset_events(artifact, repeat_close=True, remote=remote)
            validate_case(artifact)
        for field, value in (("reset_receipt_sequence", True), ("reset_returned_order", 3),
                             ("error_type", "*network.StreamError"), ("error_type", "*fmt.wrapError"),
                             ("error_code", 7), ("prepared", False)):
            artifact = synthetic_case(lower_quic=False)
            reset, observed, resolution = self.owned_reset_events(artifact, repeat_close=True)
            observed[field] = value
            resolution[field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                validate_case(artifact)

        artifact = synthetic_case(lower_quic=False)
        reset, observed, resolution = self.owned_reset_events(artifact, repeat_close=True)
        values = artifact["raw"]["sink"]["events"]
        values.insert(observed["sequence"] - 1, {**reset, "operation": "stream_reset_with_error", "requested_reset_code": 0,
                                                 "started_order": 3, "returned_order": 4})
        for sequence, event in enumerate(values, 1):
            event.update(sequence=sequence, mono_ns=sequence)
        observed.update(started_order=5, returned_order=6)
        resolution.update(started_order=5, returned_order=6, operation_receipt_sequence=observed["sequence"])
        with self.assertRaisesRegex(ValueError, "superseded"):
            validate_case(artifact)

    def test_concurrent_reset_close_is_unknown_and_requires_exact_joined_disposal(self):
        for late in (False, True):
            for reset_first in (False, True):
                artifact = synthetic_case(lower_quic=False)
                reset, observed, final = self.concurrent_reset_close_events(
                    artifact, late_publication=late, reset_return_first=reset_first)
                original = deepcopy(artifact)
                with self.subTest(late_publication=late, reset_return_first=reset_first):
                    validate_case(artifact)
                    self.assertTrue(_same_json(original, artifact))
                    self.assertEqual(observed["reset_returned_order"], 0)
                    self.assertEqual(observed["reset_receipt_sequence"], 0)
                    self.assertLess(reset["started_order"], observed["started_order"])
                    self.assertLess(observed["started_order"], reset["returned_order"])
                    self.assertEqual(final["observed_reset_receipt_sequence"], final["owner_disposal_receipt_sequence"])
                    self.assertIs(final["native_close_succeeded"], False)
                    self.assertEqual(final["terminal_state_cause"], "unknown")
                    self.assertNotIn("causal_reset_receipt_sequence", final)
                    self.assertNotIn("causal_reset_returned_order", final)

    def test_concurrent_reset_close_rejects_nonlocal_wrapped_or_unprepared_observations(self):
        changes = [("remote", True), ("remote", 0), ("error_code", 7), ("error_code", False),
                   ("error_type", "*network.StreamError"), ("error_type", "*fmt.wrapError"),
                   ("error_type", "*errors.joinError"), ("error_type", "syscall.Errno"),
                   ("typed_cause", "opaque"), ("operation", "stream_close_read"),
                   ("native_yamux", False), ("prepared", False), ("prepare_ack_sequence", 0),
                   ("prepare_ack_sequence", True), ("prepare_ack_sequence", 999),
                   ("reset_started_order", 0), ("reset_started_order", True), ("reset_started_order", 5),
                   ("reset_returned_order", 4), ("reset_receipt_sequence", 1),
                   ("native_close_succeeded", True), ("native_close_succeeded", 0),
                   ("terminal_state_cause", "graceful"), ("protocol", "/meshsub/1.0.0"),
                   ("stream_id", "foreign"), ("transport_error_type", "*net.OpError")]
        for key, value in changes:
            artifact = synthetic_case(lower_quic=False)
            _, observed, final = self.concurrent_reset_close_events(artifact, late_publication=True)
            observed[key] = final[key] = value
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_concurrent_reset_close_rejects_foreign_failed_missing_or_unjoined_disposal(self):
        mutations = [
            lambda a, r, o, f: r.update(stream_id="foreign"),
            lambda a, r, o, f: r.update(remote_peer_id=a["raw"]["offender"]["local_peer_id"]),
            lambda a, r, o, f: r.update(protocol="/meshsub/1.0.0"),
            lambda a, r, o, f: r.update(prepared=False, prepare_ack_sequence=0),
            lambda a, r, o, f: r.update(prepare_ack_sequence=True),
            lambda a, r, o, f: r.update(prepare_ack_sequence=999),
            lambda a, r, o, f: r.update(operation="stream_close_read"),
            lambda a, r, o, f: r.update(operation="stream_reset_with_error", requested_reset_code=0),
            lambda a, r, o, f: r.update(outcome="error", error="native reset failure", typed_cause="opaque", error_type="syscall.Errno"),
            lambda a, r, o, f: r.update(returned_order=7),
            lambda a, r, o, f: f.update(observed_reset_receipt_sequence=0),
            lambda a, r, o, f: f.update(observed_reset_receipt_sequence=True),
            lambda a, r, o, f: f.update(observed_reset_receipt_sequence=o["sequence"]),
            lambda a, r, o, f: f.update(observed_reset_returned_order=4.0),
            lambda a, r, o, f: f.update(owner_disposal_receipt_sequence=o["sequence"]),
            lambda a, r, o, f: f.update(causal_reset_receipt_sequence=r["sequence"]),
            lambda a, r, o, f: f.update(io_joined=False),
            lambda a, r, o, f: f.update(native_owner_disposed=False),
            lambda a, r, o, f: f.update(read_finalized=False),
            lambda a, r, o, f: f.update(write_finalized=False),
            lambda a, r, o, f: f.update(pending_read_frame_bytes=1),
            lambda a, r, o, f: f.update(pending_write_frame_bytes=1),
            lambda a, r, o, f: f.update(framing_clean=False),
            lambda a, r, o, f: a["raw"]["sink"].update(error="earlier sticky TCP failure"),
            lambda a, r, o, f: a["raw"]["sink"].update(overflow=True),
            lambda a, r, o, f: a["raw"]["sink"]["events"].remove(f),
            lambda a, r, o, f: a["raw"]["sink"]["events"].remove(r),
        ]
        for mutation in mutations:
            artifact = synthetic_case(lower_quic=False)
            records = self.concurrent_reset_close_events(artifact, late_publication=True)
            mutation(artifact, *records)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_case(artifact)
        artifact = synthetic_case(lower_quic=False)
        _, _, final = self.concurrent_reset_close_events(artifact)
        events = artifact["raw"]["sink"]["events"]
        events.append({**final, "sequence": len(events) + 1, "mono_ns": len(events) + 1})
        with self.assertRaisesRegex(ValueError, "duplicate"):
            validate_case(artifact)

    def test_concurrent_reset_close_never_rebinds_a_completed_future_or_superseding_reset(self):
        artifact = synthetic_case(lower_quic=False)
        reset, observed, final = self.concurrent_reset_close_events(artifact)
        reset["returned_order"] = 3
        observed.update(started_order=4, returned_order=5)
        final.update(started_order=4, returned_order=5, observed_reset_returned_order=3)
        with self.assertRaisesRegex(ValueError, "overlapping"):
            validate_case(artifact)
        for explicit in (False, True):
            artifact = synthetic_case(lower_quic=False)
            reset, observed, final = self.concurrent_reset_close_events(artifact, late_publication=True)
            newer = {**reset, "started_order": 5, "returned_order": 6}
            if explicit:
                newer.update(operation="stream_reset_with_error", requested_reset_code=0)
            self.insert_sink_events(artifact, final["sequence"] - 1, [newer])
            with self.subTest(explicit=explicit), self.assertRaisesRegex(ValueError, "superseded"):
                validate_case(artifact)
        artifact = synthetic_case(lower_quic=False)
        reset, observed, final = self.concurrent_reset_close_events(artifact)
        reset.update(started_order=4, returned_order=5)
        observed["reset_started_order"] = final["reset_started_order"] = 4
        final["observed_reset_returned_order"] = 5
        with self.assertRaises(ValueError):
            validate_case(artifact)

    def test_repeat_close_accepts_late_publication_only_with_exact_native_return_and_finalizer(self):
        for publish_before_close_receipt in (False, True):
            artifact = synthetic_case(lower_quic=False)
            reset, observed, resolution = self.owned_reset_events(artifact, repeat_close=True, late_reset_publication=True)
            values = artifact["raw"]["sink"]["events"]
            if publish_before_close_receipt:
                values[observed["sequence"] - 1], values[reset["sequence"] - 1] = reset, observed
                for sequence, event in enumerate(values, 1):
                    event.update(sequence=sequence, mono_ns=sequence)
                resolution.update(operation_receipt_sequence=observed["sequence"], causal_reset_receipt_sequence=reset["sequence"],
                                  owner_disposal_receipt_sequence=reset["sequence"])
            original = deepcopy(artifact)
            validate_case(artifact)
            self.assertTrue(_same_json(original, artifact))
            self.assertEqual(observed["reset_receipt_sequence"], 0)
            self.assertLess(reset["returned_order"], observed["started_order"])

        mutations = [
            lambda a, r, o, f: o.update(reset_started_order=0),
            lambda a, r, o, f: o.update(reset_returned_order=0),
            lambda a, r, o, f: o.update(reset_receipt_sequence=True),
            lambda a, r, o, f: o.update(reset_returned_order=2.0),
            lambda a, r, o, f: r.update(returned_order=4),
            lambda a, r, o, f: r.update(operation="stream_reset_with_error", requested_reset_code=0),
            lambda a, r, o, f: r.update(stream_id="foreign"),
            lambda a, r, o, f: r.update(outcome="error", error="native Reset failure", typed_cause="opaque", error_type="*errors.errorString"),
            lambda a, r, o, f: f.update(causal_reset_receipt_sequence=0),
            lambda a, r, o, f: f.update(causal_reset_receipt_sequence=True),
            lambda a, r, o, f: f.update(causal_reset_returned_order=2.0),
            lambda a, r, o, f: f.update(operation_receipt_sequence=r["sequence"]),
            lambda a, r, o, f: f.update(io_joined=False),
            lambda a, r, o, f: f.update(pending_read_frame_bytes=1),
            lambda a, r, o, f: a["raw"]["sink"].update(error="sticky earlier native failure"),
            lambda a, r, o, f: a["raw"]["sink"]["events"].remove(r),
            lambda a, r, o, f: a["raw"]["sink"]["events"].remove(f),
        ]
        for mutation in mutations:
            artifact = synthetic_case(lower_quic=False)
            records = self.owned_reset_events(artifact, repeat_close=True, late_reset_publication=True)
            mutation(artifact, *records)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_case(artifact)
        artifact = synthetic_case(lower_quic=False)
        reset, observed, resolution = self.owned_reset_events(artifact, repeat_close=True, late_reset_publication=True)
        values = artifact["raw"]["sink"]["events"]
        values.append({**resolution, "sequence": len(values) + 1, "mono_ns": len(values) + 1})
        with self.assertRaisesRegex(ValueError, "duplicate"):
            validate_case(artifact)

    def test_repeat_close_latest_reset_identity_uses_begin_counter_not_publication_sequence(self):
        for before_begin in (False, True):
            for operation in ("stream_reset", "stream_reset_with_error"):
                with self.subTest(before_begin=before_begin, operation=operation):
                    artifact = synthetic_case(lower_quic=False)
                    reset, observed, resolution = self.owned_reset_events(artifact, repeat_close=True, late_reset_publication=True)
                    values = artifact["raw"]["sink"]["events"]
                    newer = {**reset, "operation": operation, "requested_reset_code": 0 if operation == "stream_reset_with_error" else None,
                             "started_order": 3 if before_begin else 4, "returned_order": 4 if before_begin else 5}
                    values.insert(observed["sequence"] - 1, newer)
                    for sequence, event in enumerate(values, 1):
                        event.update(sequence=sequence, mono_ns=sequence)
                    observed.update(started_order=5 if before_begin else 3, returned_order=6)
                    resolution.update(started_order=observed["started_order"], returned_order=observed["returned_order"],
                                      operation_receipt_sequence=observed["sequence"], causal_reset_receipt_sequence=reset["sequence"],
                                      owner_disposal_receipt_sequence=reset["sequence"])
                    if before_begin:
                        with self.assertRaisesRegex(ValueError, "superseded"):
                            validate_case(artifact)
                    else:
                        validate_case(artifact)

    def test_json_prefix_equality_preserves_scalar_types_and_bounds(self):
        original = {"events": [{"sequence": 1, "mono_ns": 1, "ready": True, "score": 0.0}]}
        self.assertTrue(_same_json(original, deepcopy(original)))
        for field, replacement in (("sequence", True), ("mono_ns", 1.0),
                                   ("ready", 1), ("score", 0)):
            changed = deepcopy(original)
            changed["events"][0][field] = replacement
            with self.subTest(field=field):
                self.assertFalse(_same_json(original, changed))
        self.assertFalse(_same_json(float("nan"), float("nan")))
        nested = 0
        for _ in range(65):
            nested = [nested]
        self.assertFalse(_same_json(nested, deepcopy(nested)))

    def test_empty_terminal_is_neutral_but_rpc_residue_is_always_fatal(self):
        artifact = synthetic_case(lower_quic=False)
        self.terminal_event(artifact)
        validate_case(artifact)
        for pending in (None, True, False, -1, 1, 16394, "0"):
            for prepared in (False, True):
                with self.subTest(pending=pending, prepared=prepared):
                    artifact = synthetic_case(lower_quic=False)
                    event = self.terminal_event(artifact, pending=pending)
                    event["prepared"] = prepared
                    with self.assertRaisesRegex(ValueError, "RPC residue"):
                        validate_case(artifact)
        artifact = synthetic_case(lower_quic=False)
        del self.terminal_event(artifact)["pending_frame_bytes"]
        with self.assertRaises(ValueError):
            validate_case(artifact)
        for prepared in (False, True):
            artifact = synthetic_case(lower_quic=False)
            self.terminal_event(artifact, pending=1, kind="incomplete_rpc_frame")["prepared"] = prepared
            with self.assertRaisesRegex(ValueError, "remains fatal"):
                validate_case(artifact)

    def test_prepare_cannot_ack_a_hidden_framing_terminal_failure(self):
        for kind in ("stream_io_terminal", "incomplete_rpc_frame"):
            artifact = synthetic_case(lower_quic=False)
            self.terminal_event(artifact, pending=1, kind=kind)
            raw = artifact["raw"]["sink"]
            raw.update(finalized=False, joined=False)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                shutdown_ack(raw, "go", "sink", artifact["case_token"], raw["local_peer_id"], 1, active=True)

    def test_shutdown_requires_all_exact_acks_before_any_stop_and_real_terminal_success(self):
        mutations = [
            lambda a: a.pop("shutdown_barrier"),
            lambda a: a["shutdown_barrier"]["operations"].pop(0),
            lambda a: a["shutdown_barrier"]["operations"][0].update(actor="foreign"),
            lambda a: a["shutdown_barrier"]["operations"][0].update(command_sequence=2),
            lambda a: a["shutdown_barrier"]["operations"][0].update(ack_event_sequence=1),
            lambda a: a["shutdown_barrier"]["operations"][0].update(kind="stop_requested"),
            lambda a: self.first(a, "victim", "shutdown_prepared").update(case_token="b" * 32),
            lambda a: self.first(a, "victim", "shutdown_prepared").update(local_peer_id=a["raw"]["sink"]["local_peer_id"]),
            lambda a: self.first(a, "victim", "shutdown_prepared").update(actor="sink"),
            lambda a: self.first(a, "victim", "shutdown_prepared").update(admission_closed=1),
            lambda a: self.first(a, "victim", "shutdown_prepared").update(pending_commands=True),
            lambda a: self.first(a, "victim", "shutdown_prepared").update(pending_commands=1),
            lambda a: self.first(a, "victim", "command_done").update(status="error"),
            lambda a: a["raw"]["sink"].update(error="real teardown failure"),
            lambda a: a["raw"]["sink"].update(joined=False),
            lambda a: a["processes"]["sink"].update(returncode=1),
            lambda a: a["raw"]["sink"].update(host_close_returned=False),
            lambda a: a["raw"]["sink"].update(active_stream_handlers_and_io=1),
            lambda a: a["raw"]["sink"].update(active_callbacks=1),
        ]
        for mutation in mutations:
            artifact = synthetic_case(lower_quic=False)
            mutation(artifact)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_late_fixture_command_after_preparation_fails(self):
        artifact = synthetic_case(lower_quic=False)
        event = self.first(artifact, "victim", "command_done")
        event.update(command_kind="sample")
        with self.assertRaises(ValueError):
            validate_case(artifact)

    def test_expected_native_close_requires_typed_cause_preparation_and_exact_source(self):
        token = "a" * 32
        raw = {"schema_version": 1, "implementation": "rust", "case_token": token, "actor": "victim",
               "local_peer_id": peer_id(1), "finalized": True, "joined": True, "overflow": False, "error": None,
               "events": [{"sequence": 1, "mono_ns": 1, "kind": "expected_native_close",
                           "source": "rust.libp2p.passive-upgraded-stream-io", "operation": "stream_read",
                           "io_kind": "NotConnected", "raw_os_error": None, "message": "connection lost",
                           "typed_cause": "quinn_application_closed_0", "prepared": True,
                           "connection_trace_id": 1, "connection_id": "unit-connection", "swarm_connection_id": "unit-connection",
                           "peer_id": peer_id(2), "remote_peer_id": peer_id(2), "stream_trace_id": 1, "stream_id": "1:1"}]}
        _events(raw, "rust", token, "victim")
        for field, value in (("typed_cause", "closed by peer: 0"), ("typed_cause", "quinn_application_closed_nonzero"),
                             ("typed_cause", "quic_connection_cause_unavailable"), ("prepared", False),
                             ("connection_trace_id", None), ("stream_id", "foreign-owner"),
                             ("raw_os_error", 38),
                             ("source", "rust.fixture.expected_close"), ("kind", "native_io_error")):
            changed = deepcopy(raw)
            changed["events"][0][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                _events(changed, "rust", token, "victim")

    def test_public_yamux_terminal_state_requires_exact_type_owner_ack_and_disposal(self):
        for private in (False, True):
            for version012 in (False, True):
                raw = synthetic_yamux_terminal(private=private, version012=version012)
                _events(raw, "rust", raw["case_token"], "victim")
        raw = synthetic_yamux_terminal()
        mutations = (
            lambda r: r["events"][-1].update(typed_cause="yamux013_io"),
            lambda r: r["events"][-1].update(typed_cause="yamux013_decode"),
            lambda r: r["events"][-1].update(typed_cause="connection is closed"),
            lambda r: r["events"][-1].update(closure_reason="graceful"),
            lambda r: r["events"][-1].update(graceful=True),
            lambda r: r["events"][-1].update(prepared=False),
            lambda r: r["events"][-1].update(prepared=1),
            lambda r: r["events"][-1].update(prepare_ack_sequence=True),
            lambda r: r["events"][-1].update(prepare_ack_sequence=2.0),
            lambda r: r["events"][-1].update(prepare_ack_sequence=3),
            lambda r: r["events"][1].update(kind="missing_ack"),
            lambda r: r["events"][1].update(actor="sink"),
            lambda r: r["events"][1].update(case_token="c" * 32),
            lambda r: r["events"][2].update(status="error"),
            lambda r: r["events"][-1].update(connection_trace_id=True),
            lambda r: r["events"][-1].update(connection_id="foreign"),
            lambda r: r["events"][-1].update(remote_peer_id=peer_id(3)),
            lambda r: r["events"][-1].update(stream_trace_id=1, stream_id="1:1"),
            lambda r: r["events"][-1].update(operation="stream_read"),
            lambda r: r["events"][-1].update(raw_os_error=38),
            lambda r: r["events"][-1].update(error_boundary="generic_source_chain"),
            lambda r: r["events"][-1].update(source="rust.fixture.expected_close"),
            lambda r: r["events"][-1]["endpoint"].update(remote_address="foreign"),
            lambda r: r["events"][0].update(authenticated=1),
            lambda r: r["events"][0].update(owner_basis="manifest_claim"),
            lambda r: r["events"][0].update(transport="quic"),
            lambda r: r["events"][0].update(muxer_error_boundary="generic_boxing"),
            lambda r: r["events"][-1]["native_stack"].update(muxer="quic"),
            lambda r: r["native_close"].update(live_streams=1),
            lambda r: r["native_close"]["connections"][0].update(dropped=False),
            lambda r: r["native_close"]["connections"][0].update(native_terminal_observed=False),
            lambda r: r["native_close"]["connections"][0].update(connection_trace_id=True),
            lambda r: r["task_join"].update(fixture_owned_tasks_joined=False),
            lambda r: r.update(error="earlier actual failure"),
            lambda r: r.update(overflow=True),
            lambda r: r.update(joined=False),
        )
        for index, mutate in enumerate(mutations):
            changed = deepcopy(raw)
            mutate(changed)
            with self.subTest(index=index), self.assertRaises(ValueError):
                _events(changed, "rust", changed["case_token"], "victim")

    def test_yamux_closed_cannot_borrow_future_ack_or_hide_framing_error(self):
        raw = synthetic_yamux_terminal()
        changed = deepcopy(raw)
        connection, ack, done, state = changed["events"]
        changed["events"] = [connection, state, ack, done]
        for sequence, event in enumerate(changed["events"], 1):
            event.update(sequence=sequence, mono_ns=sequence)
        state["prepare_ack_sequence"] = ack["sequence"]
        with self.assertRaises(ValueError):
            _events(changed, "rust", changed["case_token"], "victim")
        for position in (1, 4):
            changed = deepcopy(raw)
            changed["events"].insert(position, {"kind": "native_io_error", "source": "rust.libp2p.passive-upgraded-stream-io",
                                               "typed_cause": "incomplete_rpc_frame"})
            for sequence, event in enumerate(changed["events"], 1):
                event.update(sequence=sequence, mono_ns=sequence)
            with self.subTest(position=position), self.assertRaises(ValueError):
                _events(changed, "rust", changed["case_token"], "victim")

    def test_go_quiesce_requires_exact_owner_resources_and_pre_stop_order(self):
        baseline = synthetic_case(lower_quic=False)
        validate_case(baseline)
        modes = ("missing_request", "missing_ack", "duplicate_ack", "wrong_pid", "boolean_pid", "raw_pid",
                 "wrong_prepare", "boolean_prepare", "future_prepare", "foreign_token", "foreign_actor", "wrong_source",
                 "open_admission", "open_callbacks", "live_worker", "live_io", "live_stream", "callback", "boolean_zero",
                 "live_context", "missing_done", "failed_done", "host_claim", "sticky", "stop_before_ack")
        for mode in modes:
            artifact = deepcopy(baseline)
            rows = artifact["shutdown_barrier"]["operations"]
            request = next(row for row in rows if row["kind"] == "quiesce_requested")
            observed = next(row for row in rows if row["kind"] == "quiesce_ack" and row["actor"] == request["actor"])
            raw = artifact["raw"][request["actor"]]
            ack = raw["events"][observed["quiesce_event_sequence"] - 1]
            done = next(event for event in raw["events"] if event.get("command_kind") == "quiesce_shutdown")
            changes = {
                "missing_request": lambda: rows.remove(request), "missing_ack": lambda: raw["events"].remove(ack),
                "duplicate_ack": lambda: raw["events"].append({**ack, "sequence": len(raw["events"]) + 1, "mono_ns": len(raw["events"]) + 1}),
                "wrong_pid": lambda: ack.update(pid=99), "boolean_pid": lambda: ack.update(pid=True),
                "raw_pid": lambda: raw.update(pid=99), "wrong_prepare": lambda: ack.update(prepare_ack_sequence=1),
                "boolean_prepare": lambda: ack.update(prepare_ack_sequence=True),
                "future_prepare": lambda: ack.update(prepare_ack_sequence=ack["sequence"] + 1),
                "foreign_token": lambda: ack.update(case_token="b" * 32), "foreign_actor": lambda: ack.update(actor="sink"),
                "wrong_source": lambda: ack.update(source="go.pubsub.RawTracer.Join"),
                "open_admission": lambda: ack.update(native_admission_closed=False),
                "open_callbacks": lambda: ack.update(pubsub_callback_admission_closed=False),
                "live_worker": lambda: ack.update(active_fixture_workers=1), "live_io": lambda: ack.update(active_stream_handlers_and_io=1),
                "live_stream": lambda: ack.update(active_pubsub_streams=1), "callback": lambda: ack.update(active_callbacks=1),
                "boolean_zero": lambda: ack.update(active_callbacks=False), "live_context": lambda: ack.update(pubsub_context_cancelled=False),
                "missing_done": lambda: raw["events"].remove(done), "failed_done": lambda: done.update(status="error"),
                "host_claim": lambda: ack.update(host_close_returned=True), "sticky": lambda: raw.update(error="native TCP ECONNRESET"),
            }
            if mode == "stop_before_ack":
                left, right = rows.index(observed), next(index for index, row in enumerate(rows) if row["kind"] == "stop_requested")
                rows[left], rows[right] = rows[right], rows[left]
                for sequence, row in enumerate(rows, 1): row["sequence"] = sequence
            else:
                changes[mode]()
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                _shutdown_barrier(artifact, artifact["raw"], {name: value["events"] for name, value in artifact["raw"].items()})

    def test_go_quiesce_rejects_late_native_subscription_and_selected_owner_not_logical_submission(self):
        for kind in ("rpc", "protocol", "rpc_send"):
            artifact = synthetic_case(lower_quic=False)
            raw = artifact["raw"]["sink"]
            if kind == "rpc_send":
                event = {"kind": kind, "source": "go.pubsub.RawTracer.rpc_send", "wire_receipt": False,
                         "remote_peer_id": artifact["raw"]["replacement"]["local_peer_id"], "submission_sha256": "c" * 64}
            else:
                event = deepcopy(next(value for value in raw["events"] if value["kind"] == kind))
                if kind == "rpc":
                    event.update(direction="write", source="go.pubsub.native_stream.write",
                                 receipt=receipt(field(1, field(1, 1) + field(2, ("forge-pr11:" + artifact["case_token"]).encode())), "write"))
                else:
                    event["stream_id"] = "new-native-owner-after-ACK"
            self.insert_sink_events(artifact, len(raw["events"]) - 1, [event])
            with self.subTest(kind=kind):
                if kind == "rpc_send":
                    _shutdown_barrier(artifact, artifact["raw"], {name: value["events"] for name, value in artifact["raw"].items()})
                else:
                    with self.assertRaisesRegex(ValueError, "after quiesce"):
                        _shutdown_barrier(artifact, artifact["raw"], {name: value["events"] for name, value in artifact["raw"].items()})

    def test_active_go_quiesce_is_not_host_or_process_join(self):
        artifact = synthetic_case(lower_quic=False)
        raw = artifact["raw"]["sink"]
        raw["events"].pop()
        raw.update(finalized=False, joined=False, host_close_returned=False)
        row = next(row for row in artifact["shutdown_barrier"]["operations"] if row["kind"] == "quiesce_ack" and row["actor"] == "sink")
        args = (raw, "sink", artifact["case_token"], raw["local_peer_id"], raw["pid"], row["command_sequence"], row["prepare_ack_sequence"])
        self.assertEqual(quiesce_ack(*args, active=True)["sequence"], row["quiesce_event_sequence"])
        for field in ("host_close_returned", "finalized", "joined", "active_fixture_workers", "active_stream_handlers_and_io"):
            changed = deepcopy(raw)
            changed[field] = True if field in {"host_close_returned", "finalized", "joined"} else 1
            with self.subTest(field=field), self.assertRaises(ValueError):
                quiesce_ack(changed, *args[1:], active=True)

    def test_donor_join_barrier_rejects_missing_foreign_forged_and_reordered_receipts(self):
        modes = ("missing_join", "missing_prepare", "duplicate_stop", "duplicate_join", "early_join", "early_forge_stop",
                 "foreign_actor", "foreign_peer", "foreign_token", "foreign_pid", "boolean_pid", "foreign_shutdown",
                 "boolean_shutdown", "extra_claim", "nonzero_exit", "pending_exit", "forced_exit", "boolean_exit",
                 "missing_shutdown", "duplicate_shutdown", "wrong_source", "shutdown_io", "live_io")
        for mode in modes:
            artifact = synthetic_case()
            rows = artifact["shutdown_barrier"]["operations"]
            join = next(row for row in rows if row["kind"] == "donor_joined")
            stops = [row for row in rows if row["kind"] == "stop_requested"]
            joins = [row for row in rows if row["kind"] == "donor_joined"]
            raw, process = artifact["raw"][join["actor"]], artifact["processes"][join["actor"]]
            shutdown = raw["events"][join["shutdown_event_sequence"] - 1]
            mutations = {
                "missing_join": lambda: rows.remove(join),
                "missing_prepare": lambda: rows.pop(0),
                "duplicate_stop": lambda: stops[1].update(actor=stops[0]["actor"]),
                "duplicate_join": lambda: joins[1].update(actor=join["actor"]),
                "foreign_actor": lambda: join.update(actor="foreign"),
                "foreign_peer": lambda: join.update(local_peer_id=artifact["raw"]["victim"]["local_peer_id"]),
                "foreign_token": lambda: join.update(case_token="b" * 32),
                "foreign_pid": lambda: join.update(pid=process["pid"] + 10),
                "boolean_pid": lambda: join.update(pid=True),
                "foreign_shutdown": lambda: join.update(shutdown_event_sequence=rows[1]["ack_event_sequence"]),
                "boolean_shutdown": lambda: join.update(shutdown_event_sequence=True),
                "extra_claim": lambda: join.update(joined=True),
                "nonzero_exit": lambda: process.update(returncode=2),
                "pending_exit": lambda: process.update(returncode=None),
                "forced_exit": lambda: process.update(forced_termination=True),
                "boolean_exit": lambda: process["terminal_status"].update(exit_code=False),
                "missing_shutdown": lambda: raw["events"].remove(shutdown),
                "duplicate_shutdown": lambda: raw["events"].append({**deepcopy(shutdown), "sequence": len(raw["events"]) + 1,
                                                                    "mono_ns": len(raw["events"]) + 1}),
                "wrong_source": lambda: shutdown.update(source="go.pubsub.RawTracer.DeliverMessage"),
                "shutdown_io": lambda: shutdown.update(active_stream_handlers_and_io=1),
                "live_io": lambda: raw.update(active_stream_handlers_and_io=1),
            }
            if mode in mutations:
                mutations[mode]()
            else:
                left, right = (rows.index(stops[0]), rows.index(joins[0])) if mode == "early_join" \
                    else (rows.index(joins[-1]), rows.index(stops[-1]))
                rows[left], rows[right] = rows[right], rows[left]
            for sequence, row in enumerate(rows, 1):
                row["sequence"] = sequence
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                _shutdown_barrier(artifact, artifact["raw"], {name: value["events"] for name, value in artifact["raw"].items()})

    def test_yamux_terminal_requires_all_actor_barrier_and_correct_profile(self):
        artifact = synthetic_case(lower_quic=False)
        artifact["raw"]["victim"] = synthetic_yamux_terminal()
        artifact["case"].update(destination="rust", profile="native_tcp_yamux")
        artifact["shutdown_barrier"]["operations"][0]["ack_event_sequence"] = 2
        raw = artifact["raw"]["victim"]
        shutdown_sequence = len(raw["events"]) + 1
        raw["events"].append({"sequence": shutdown_sequence, "mono_ns": shutdown_sequence,
                              "kind": "shutdown_requested", "source": "rust.fixture.native_host_close", "listeners": 1})
        operations = artifact["shutdown_barrier"]["operations"]
        operations[:] = operations[:4] + [row for row in operations[4:] if row["kind"] in {"quiesce_requested", "quiesce_ack"}] \
            + [row for row in operations[4:] if row["kind"] == "stop_requested"] \
            + [row for row in operations[4:] if row["kind"] == "donor_joined"]
        operations.append({"kind": "donor_joined", "actor": "victim", "case_token": artifact["case_token"],
                           "local_peer_id": raw["local_peer_id"], "pid": artifact["processes"]["victim"]["pid"],
                           "shutdown_event_sequence": shutdown_sequence})
        for sequence, row in enumerate(operations, 1):
            row["sequence"] = sequence
        events = {name: raw["events"] for name, raw in artifact["raw"].items()}
        _shutdown_barrier(artifact, artifact["raw"], events)
        for mutate in (
            lambda a: a["shutdown_barrier"]["operations"].pop(1),
            lambda a: a["shutdown_barrier"]["operations"][1].update(actor="foreign"),
            lambda a: a["case"].update(profile="native_quic"),
            lambda a: a["case"].update(profile="private_tcp_yamux"),
        ):
            changed = deepcopy(artifact)
            mutate(changed)
            with self.assertRaises(ValueError):
                _shutdown_barrier(changed, changed["raw"], {name: raw["events"] for name, raw in changed["raw"].items()})

    def test_cached_go_inspection_before_ignore_cannot_prove_neutrality(self):
        from pubsub_evidence import _score_observation

        values = [{"peer_id": "unit", "value": 0}]
        observation = {"sequence": 1, "kind": "score", "source": "go.pubsub.WithPeerScoreInspect", "peer_scores": values}
        snapshot = {"sequence": 5, "source": "go.pubsub.RawTracer_mesh_and_score_inspection",
                    "score_observation_sequence": 1, "peer_scores": deepcopy(values)}
        _score_observation([observation], snapshot)
        with self.assertRaises(ValueError):
            _score_observation([observation], snapshot, after=2)
        snapshot["peer_scores"][0]["value"] = -100
        with self.assertRaises(ValueError):
            _score_observation([observation], snapshot)

    def test_send_pair_with_another_author_cannot_replace_exact_native_input(self):
        artifact = synthetic_case(lower_quic=False)
        victim, offender = (artifact["raw"][name] for name in ("victim", "offender"))
        payload = "ignore:" + artifact["case_token"] + ":one"
        body = field(2, field(1, _peer(artifact["raw"]["sink"]["local_peer_id"]))
                     + field(2, payload.encode()) + field(3, (2).to_bytes(8, "big"))
                     + field(4, ("forge-pr11:" + artifact["case_token"]).encode()) + field(5, b"synthetic-signature"))
        from pubsub_wire import validate_rpc_receipt

        sent = next(event for event in offender["events"] if event.get("kind") == "rpc" and event["direction"] == "write"
                    and any(message["payload_sha256"] == hashlib.sha256(payload.encode()).hexdigest()
                            for message in validate_rpc_receipt(event["receipt"], event["protocol"], "write")["messages"]))
        sent["receipt"] = receipt(body, "write")
        read = next(event for event in victim["events"] if event.get("kind") == "rpc" and event["direction"] == "read"
                    and any(message["payload_sha256"] == hashlib.sha256(payload.encode()).hexdigest()
                            for message in validate_rpc_receipt(event["receipt"], event["protocol"], "read")["messages"]))
        wrong = deepcopy(read)
        wrong["receipt"] = receipt(body, "read")
        victim["events"].insert(victim["events"].index(read), wrong)
        for index, event in enumerate(victim["events"], 1):
            event.update(sequence=index, mono_ns=index)
        with self.assertRaises(ValueError):
            validate_case(artifact)

    def test_initial_gossip_moved_after_rejection_is_not_phase_evidence(self):
        artifact = synthetic_case(lower_quic=False)
        events = artifact["raw"]["victim"]["events"]
        first = next(index for index, event in enumerate(events) if event.get("kind") == "snapshot" and event["label"] == "before") + 1
        last = next(index for index, event in enumerate(events) if event.get("kind") == "validation" and event.get("outcome") == "ignore") - 1
        gossip = events[first:last]
        del events[first:last]
        index = next(index for index, event in enumerate(events) if event.get("kind") == "validation" and event.get("outcome") == "reject") + 1
        events[index:index] = gossip
        for index, event in enumerate(events, 1):
            event.update(sequence=index, mono_ns=index)
        with self.assertRaises(ValueError):
            validate_case(artifact)

    def test_rust_lifecycle_sources_are_exact_and_unknown_events_still_fail(self):
        token = "a" * 32
        kinds = ("stream_opened", "connection_established", "native_close", "host_muxer_dropped")
        raw = {"schema_version": 1, "implementation": "rust", "case_token": token,
               "actor": "victim", "local_peer_id": peer_id(1), "finalized": True,
               "joined": True, "overflow": False, "error": None,
               "events": [{"sequence": index, "mono_ns": index, "kind": kind,
                           "source": "rust.libp2p.passive-upgraded-stream-io"}
                          for index, kind in enumerate(kinds, 1)]}
        self.assertEqual(len(_events(raw, "rust", token, "victim")), len(kinds))
        for kind in kinds:
            changed = deepcopy(raw)
            next(event for event in changed["events"] if event["kind"] == kind).update(
                source="rust.fixture.asserted-lifecycle")
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                _events(changed, "rust", token, "victim")
        for kind in ("unexpected_native_event", "application_error"):
            changed = deepcopy(raw)
            changed["events"][0]["kind"] = kind
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                _events(changed, "rust", token, "victim")

    def test_checker_accepts_only_coherent_synthetic_causal_path(self):
        self.assertEqual(validate_case(synthetic_case(lower_quic=False))["repaired_path"], ["victim", "replacement", "sink"])

    def test_each_missing_native_fact_fails_closed(self):
        mutations = [
            lambda a: a["raw"]["victim"].update(schema_version=True),
            lambda a: a["raw"]["victim"].update(schema_version="1"),
            lambda a: a["raw"]["victim"].update(joined=False),
            lambda a: a["raw"]["victim"].update(overflow=True),
            lambda a: a["raw"]["victim"].update(case_token="b" * 32),
            lambda a: a["processes"]["victim"].update(forced_termination=True),
            lambda a: a["processes"]["sink"].update(pid=a["processes"]["replacement"]["pid"]),
            lambda a: a.update(cleanup_errors=["native worker not joined"]),
            lambda a: self.first(a, "victim", "connection").update(authenticated=False),
            lambda a: self.first(a, "victim", "connection").update(authentication_basis="configured_TLS"),
            lambda a: self.first(a, "victim", "connection").update(remote_address="/fake/ip4/127.0.0.1/udp/1/quic-v1"),
            lambda a: self.first(a, "victim", "validation").update(source="forge.fixture.asserted_validation"),
            lambda a: self.first(a, "victim", "validation").update(message_id="asserted"),
            lambda a: self.first(a, "victim", "rpc").update(direction="send"),
            lambda a: self.first(a, "sink", "rpc").update(source="go.pubsub.native_stream.write"),
            lambda a: self.first(a, "victim", "rpc").update(protocol="/meshsub/1.0.0"),
            lambda a: self.first(a, "victim", "validation").update(seqno_hex="0" * 16),
            lambda a: self.first(a, "victim", "validation").update(author_peer=a["raw"]["sink"]["local_peer_id"]),
            lambda a: self.sample(a, "ignored")["peer_scores"][0].update(value=-1),
            lambda a: self.sample(a, "penalized")["peer_scores"][0].update(value=-10),
            lambda a: self.sample(a, "repaired").update(mesh_peer_ids=[a["raw"]["offender"]["local_peer_id"]]),
            lambda a: self.first(a, "sink", "delivery").update(propagation_peer=a["raw"]["victim"]["local_peer_id"]),
            lambda a: self.first(a, "victim", "rpc")["receipt"]["write"].update(framed_sha256="0" * 64),
            lambda a: a["raw"]["sink"]["events"][0].update(peer_id=a["raw"]["victim"]["local_peer_id"]),
        ]
        for mutation in mutations:
            artifact = synthetic_case(lower_quic=False)
            mutation(artifact)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_unknown_error_event_and_delivery_of_ignored_input_fail(self):
        for kind in ("application_error", "delivery"):
            artifact = synthetic_case(lower_quic=False)
            events = artifact["raw"]["victim"]["events"]
            event = deepcopy(next(e for e in events if e.get("outcome") == "ignore"))
            event.update(kind=kind, sequence=len(events) + 1, mono_ns=len(events) + 1)
            events.append(event)
            with self.subTest(kind=kind), self.assertRaises(ValueError):
                validate_case(artifact)

    def test_late_identical_native_duplicate_does_not_invalidate_original_receipt(self):
        artifact = synthetic_case(lower_quic=False)
        events = artifact["raw"]["victim"]["events"]
        original = next(e for e in events if e.get("kind") == "rpc" and e.get("direction") == "read"
                        and e["peer_id"] == artifact["raw"]["offender"]["local_peer_id"])
        duplicate = deepcopy(original)
        duplicate.update(sequence=len(events) + 1, mono_ns=len(events) + 1)
        events.append(duplicate)
        validate_case(artifact)

    def test_coherent_different_signed_identity_cannot_be_stitched_into_repaired_path(self):
        artifact = synthetic_case(lower_quic=False)
        token, sink = artifact["case_token"], artifact["raw"]["sink"]
        author = artifact["raw"]["victim"]["local_peer_id"]
        payload = "accept:" + token + ":repaired"
        digest = hashlib.sha256(payload.encode()).hexdigest()
        sequence = (999).to_bytes(8, "big")
        body = field(2, field(1, _peer(author)) + field(2, payload.encode()) + field(3, sequence)
                     + field(4, ("forge-pr11:" + token).encode()) + field(5, b"synthetic-signature"))
        # Make both adjacent receipts internally coherent; only end-to-end identity differs.
        replacement = artifact["raw"]["replacement"]
        target = next(e for e in sink["events"] if e.get("kind") == "delivery" and e["payload_sha256"] == digest)
        old = (4).to_bytes(8, "big").hex()
        for name, events, direction, peer in (("sink", sink["events"], "read", replacement["local_peer_id"]),
                                             ("replacement", replacement["events"], "write", sink["local_peer_id"])):
            for event in events:
                if event.get("kind") == "rpc" and event.get("direction") == direction and event["peer_id"] == peer:
                    from pubsub_wire import validate_rpc_receipt
                    if any(m["payload_sha256"] == digest for m in
                           validate_rpc_receipt(event["receipt"], "/meshsub/1.1.0", direction)["messages"]):
                        event["receipt"] = receipt(body, direction)
                if name == "sink" and event.get("payload_sha256") == digest and event.get("seqno_hex") == old:
                    event.update(seqno_hex=sequence.hex(), message_id=(_peer(author) + sequence).hex())
        self.assertEqual(target["seqno_hex"], sequence.hex())
        with self.assertRaises(ValueError):
            validate_case(artifact)

    @staticmethod
    def first(artifact, name, kind):
        return next(event for event in artifact["raw"][name]["events"] if event["kind"] == kind)

    @staticmethod
    def sample(artifact, label):
        return next(event for event in artifact["raw"]["victim"]["events"] if event.get("label") == label)


if __name__ == "__main__":
    unittest.main()
