"""PR11 causal native scoring evidence. Configuration/status are not proof."""

import hashlib
import json
import math
import re

from autorelay_wire import _fields, varint
from pubsub_wire import validate_rpc_receipt
from rust_upgrade_evidence import _peer, _varint
from upgrade_evidence import HEADER, wire_token

DIRECTIONS = (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge"))
PROFILES = {"native_quic": "quic", "native_tcp_yamux": "tcp", "private_tcp_yamux": "tcp-pnet-noise"}
EVENT_LIMIT = 2048
SOURCES = {
    "forge": {"connection": {"forge.node.authenticated_session"}, "protocol": {"forge.pubsub.native_stream"},
              "rpc": {"forge.pubsub.native_stream"}, "validation": {"forge.pubsub.committed_validation"},
              "delivery": {"forge.pubsub.committed_validation"}, "snapshot": {"forge.pubsub.peer_score_snapshot"},
              "publish": {"forge.node.async_publish"}, "command_done": {"forge.fixture.native_operation"}},
    "go": {"connection": {"go.network.Conn.authenticated_output"}, "protocol": {"go.network.Stream.Protocol"},
           "rpc": {"go.pubsub.native_stream.read", "go.pubsub.native_stream.write"},
           "validation": {"go.pubsub.ValidatorEx"},
           "delivery": {"go.pubsub.RawTracer.DeliverMessage"},
           "snapshot": {"go.pubsub.RawTracer_mesh_and_score_inspection"}, "publish": {"go.pubsub.Topic.Publish"},
           "command_done": {"go.fixture.append_only_control"}},
    "rust": {kind: {"rust.libp2p.passive-upgraded-stream-io"} for kind in ("connection", "protocol", "rpc")}
            | {kind: {"rust.libp2p.gossipsub.public-Behaviour"}
               for kind in ("validation", "delivery", "snapshot", "publish")}
            | {"command_done": {"rust.fixture.control-native-operation-completion"}},
}
SOURCES["go"].update({
    **{kind: {"go.pubsub.RawTracer." + method} for kind, method in
       (("validation_begin", "ValidateMessage"), ("rejection", "RejectMessage"), ("duplicate", "DuplicateMessage"),
        ("undeliverable", "UndeliverableMessage"), ("throttle", "ThrottlePeer"), ("graft", "graft"),
        ("prune", "prune"), ("rpc_receive", "rpc_receive"), ("rpc_send", "rpc_send"), ("rpc_drop", "rpc_drop"),
        ("peer_protocol", "OnNewOutboundStream"), ("peer_down", "OnClosedOutboundStream"), ("join", "Join"),
        ("leave", "Leave"))},
    "score": {"go.pubsub.WithPeerScoreInspect"}, "subscription_delivery": {"go.pubsub.Subscription.Next"},
    "stream_io_terminal": {"go.network.Stream.read", "go.network.Stream.write"},
    "incomplete_rpc_frame": {"go.pubsub.native_stream.finalize"},
    "native_stream_operation": {"go.pubsub.native_stream.native_operation"},
    "native_stream_close_finalized": {"go.pubsub.native_stream.native_operation"},
    "native_stream_io_finalized": {"go.pubsub.native_stream.native_operation"},
    "shutdown": {"go.fixture.owned_context_cancel_and_drain"},
    "shutdown_quiesced": {"go.fixture.owned_pubsub_quiesce"},
    "pre_cancel_retained_reset_return": {"go.fixture.owned_pre_cancel_retained_resets"},
    "pre_cancel_retained_resets_returned": {"go.fixture.owned_pre_cancel_retained_resets"},
    "native_rejected_stream_disposal": {"go.fixture.rejected_native_stream_reset"},
    "connection_closed": {"go.network.NotifyBundle.Disconnected"},
})
GO_QUIC_SOURCES = {
    "native_quic_connection": "go.quic.transport.capable_output",
    "native_quic_stream": "go.quic.CapableConn.stream_return",
    "multistream_frame": "go.quic.native_stream.multistream",
    "protocol": "go.quic.native_stream.selected",
    "native_quic_connection_context": "go.quic.Conn.Context",
    "native_quic_send_context": "go.quic.Stream.Context.at_Close_return",
    "native_stream_operation": "go.quic.native_stream.operation_return",
    "stream_io_terminal": "go.quic.native_stream.io_return",
    "native_quic_negotiation_io_return": "go.quic.native_stream.io_return",
    "native_quic_negotiation_cleanup_pending": "go.quic.native_stream.negotiation_cleanup",
    "native_quic_negotiation_cleanup_finalized": "go.quic.native_stream.negotiation_cleanup",
    "native_quic_negotiation_abort_pending": "go.quic.native_stream.negotiation_cleanup",
    "native_quic_negotiation_abort_finalized": "go.quic.native_stream.negotiation_cleanup",
    "native_quic_negotiation_abort_close_pending": "go.quic.native_stream.negotiation_cleanup",
    "native_quic_framing_finalized": "go.quic.native_stream.finalize",
    "native_quic_terminal_finalized": "go.quic.native_stream.finalize",
    "native_quic_join": "go.quic.fixture.owned_observation_join",
}
for _kind, _source in GO_QUIC_SOURCES.items():
    SOURCES["go"].setdefault(_kind, set()).add(_source)
SOURCES["go"]["rpc"].update({"go.quic.native_stream.read", "go.quic.native_stream.write"})
QUIC_OWNER_FIELDS = {"native_connection_id", "native_stream_id", "connection_receipt_sequence",
                     "native_stream_receipt_sequence", "remote_peer_id", "stream_direction", "protocol", "owner_basis"}
EVENT_FIELDS = {"sequence", "mono_ns", "kind", "source"}
QUIC_CONTEXT_FIELDS = {"done", "cause_type", "error_code", "remote", "native_stream_id", "error"}
SOURCES["rust"].update({
    **{kind: {"rust.libp2p.passive-upgraded-stream-io"}
       for kind in ("wire_message", "graft", "prune", "stream_dropped", "stream_opened",
                    "connection_established", "native_close", "host_muxer_dropped")},
    "subscription": {"rust.libp2p.gossipsub.public-Behaviour"}, "listen": {"rust.libp2p.Swarm.NewListenAddr"},
    "shutdown_requested": {"rust.fixture.native_host_close"},
    "connection_closed": {"rust.libp2p.Swarm.ConnectionClosed"},
})
for _implementation in SOURCES:
    SOURCES[_implementation]["shutdown_prepared"] = {_implementation + ".fixture.prepare_shutdown"}
SOURCES["rust"].update({kind: {"rust.libp2p.passive-upgraded-stream-io"}
                        for kind in ("native_io_error", "expected_native_close", "native_terminal_state")})
AUTHENTICATION = {
    "forge": {profile: "native_session_upgrade_output" for profile in PROFILES.values()},
    "go": {"quic": "native_quic_TLS_InterceptSecured_and_RemotePublicKey",
           "tcp": "native_TCP_ConnState_and_authenticated_RemotePublicKey",
           "tcp-pnet-noise": "native_TCP_ConnState_and_authenticated_RemotePublicKey"},
    "rust": {"quic": "native_QUIC_authenticated_transport_output",
             "tcp": "native_Noise_authenticated_Yamux_upgrade_output",
             "tcp-pnet-noise": "native_PNET_then_Noise_authenticated_Yamux_upgrade_output"},
}


def require(value, message):
    if not value:
        raise ValueError(message)


def _id(value):
    return isinstance(value, str) and 0 < len(value) <= 256


def _number(value):
    return type(value) in (int, float) and math.isfinite(value)


def _same_json(left, right):
    """Compare bounded JSON trees without bool/int or float/int coercion."""
    pending, visited = [(left, right, 0)], 0
    while pending:
        a, b, depth = pending.pop()
        visited += 1
        if visited > 1024 * 1024 or depth > 64 or type(a) is not type(b):
            return False
        if type(a) is dict:
            if any(type(key) is not str for key in a) or set(a) != set(b):
                return False
            pending.extend((a[key], b[key], depth + 1) for key in a)
        elif type(a) is list:
            if len(a) != len(b):
                return False
            pending.extend((x, y, depth + 1) for x, y in zip(a, b))
        elif type(a) not in (str, int, float, bool, type(None)) or a != b:
            return False
        elif type(a) is float and not math.isfinite(a):
            return False
        if len(pending) > 1024 * 1024:
            return False
    return True


def shutdown_ack(raw, implementation, actor, token, local, command_sequence, *, active=False, _check_operations=True):
    """Validate actual actor acknowledgement; absence can wait, foreign evidence cannot."""
    require(isinstance(raw, dict) and type(raw.get("schema_version")) is int and raw["schema_version"] == 1
            and raw.get("implementation") == implementation and raw.get("actor") == actor
            and raw.get("case_token") == token and raw.get("local_peer_id") == local,
            "prepare_shutdown result identity mismatch")
    require(raw.get("overflow") is False and raw.get("error") is None, "prepare_shutdown actor failed")
    if active:
        require(raw.get("finalized") is False and raw.get("joined") is False, "prepare_shutdown actor not active")
    require(type(command_sequence) is int and 1 <= command_sequence <= 64, "invalid prepare command sequence")
    events = raw.get("events")
    require(isinstance(events, list) and len(events) <= EVENT_LIMIT
            and len(json.dumps(raw).encode()) <= 16 * 1024 * 1024, "unbounded prepare_shutdown result")
    for event in events:
        require(isinstance(event, dict), "invalid prepare_shutdown event")
        _framing_terminal(event, implementation)
        if active:
            require(event.get("kind") not in {"native_quic_negotiation_cleanup_finalized", "native_quic_negotiation_abort_finalized"},
                    "active Prepare snapshot claims completed native cleanup")
    if implementation == "go" and _check_operations:
        _go_native_operations(events)
        _go_quic_operations(raw, events, terminal=False)
    acks = [event for event in events if isinstance(event, dict) and event.get("kind") == "shutdown_prepared"]
    require(len(acks) <= 1, "ambiguous prepare_shutdown acknowledgement")
    if not acks:
        return None
    ack = acks[0]
    require(ack.get("source") == implementation + ".fixture.prepare_shutdown"
            and ack.get("actor") == actor and ack.get("case_token") == token and ack.get("local_peer_id") == local
            and type(ack.get("command_sequence")) is int and ack["command_sequence"] == command_sequence
            and ack.get("admission_closed") is True and type(ack.get("pending_commands")) is int
            and ack["pending_commands"] == 0 and type(ack.get("sequence")) is int
            and 1 <= ack["sequence"] <= len(events) and events[ack["sequence"] - 1] is ack,
            "foreign/invalid prepare_shutdown acknowledgement")
    done = [event for event in events if isinstance(event, dict) and event.get("kind") == "command_done"
            and event.get("command_sequence") == command_sequence]
    require(len(done) <= 1, "ambiguous prepare_shutdown completion")
    if not done:
        return None
    require(isinstance(done[0].get("source"), str) and done[0]["source"] in SOURCES[implementation]["command_done"]
            and done[0].get("command_kind") == "prepare_shutdown" and done[0].get("status") == "ok"
            and type(done[0].get("command_sequence")) is int and type(done[0].get("sequence")) is int
            and ack["sequence"] < done[0]["sequence"] <= len(events)
            and events[done[0]["sequence"] - 1] is done[0], "prepare_shutdown completion failed/mismatched")
    return ack


def quiesce_ack(raw, actor, token, local, pid, command_sequence, prepare_ref, *, active=False):
    """Go-owned PubSub drain only; no host, process, or donor-router join authority."""
    require(type(pid) is int and pid > 0 and isinstance(raw, dict) and raw.get("pid") == pid
            and type(raw.get("pid")) is int, "quiesce_shutdown process owner mismatch")
    require(type(command_sequence) is int and 2 <= command_sequence <= 64
            and type(prepare_ref) is int and prepare_ref > 0, "invalid quiesce command/prepare reference")
    # A quiesced owner is still a live process, but may already have actual
    # framing finalizers. The earlier Prepare snapshot remains unchanged.
    prepared = shutdown_ack(raw, "go", actor, token, local, command_sequence - 1)
    require(prepared is not None and prepared["sequence"] == prepare_ref, "quiesce lacks exact preceding Prepare ACK")
    events = raw["events"]
    matches = [event for event in events if event.get("kind") == "shutdown_quiesced"]
    require(len(matches) <= 1, "duplicate quiesce acknowledgement")
    if not matches:
        return None
    ack = matches[0]
    fields = EVENT_FIELDS | {"actor", "case_token", "local_peer_id", "pid", "command_sequence", "prepare_ack_sequence",
                             "native_admission_closed", "pubsub_callback_admission_closed", "pubsub_context_cancelled", "subscriber_context_cancelled",
                             "active_stream_handlers_and_io", "active_pubsub_streams", "active_fixture_workers",
                             "active_callbacks", "joined_scope"}
    require(set(ack) == fields and ack.get("source") == "go.fixture.owned_pubsub_quiesce"
            and ack.get("actor") == actor and ack.get("case_token") == token and ack.get("local_peer_id") == local
            and type(ack.get("pid")) is int and ack["pid"] == pid
            and type(ack.get("command_sequence")) is int and ack["command_sequence"] == command_sequence
            and type(ack.get("prepare_ack_sequence")) is int and ack["prepare_ack_sequence"] == prepare_ref
            and ack.get("native_admission_closed") is True and ack.get("pubsub_context_cancelled") is True
            and ack.get("pubsub_callback_admission_closed") is True
            and ack.get("subscriber_context_cancelled") is True
            and all(type(ack.get(key)) is int and ack[key] == 0 for key in
                    ("active_stream_handlers_and_io", "active_pubsub_streams", "active_fixture_workers", "active_callbacks"))
            and ack.get("joined_scope") == "fixture_subscriber_admitted_stream_IO_framing_pending_terminal_and_observer_callbacks"
            and type(ack.get("sequence")) is int and prepare_ref < ack["sequence"] <= len(events)
            and events[ack["sequence"] - 1] is ack, "invalid/foreign/unjoined Go quiesce acknowledgement")
    done = [event for event in events if event.get("kind") == "command_done"
            and event.get("command_sequence") == command_sequence]
    require(len(done) <= 1, "duplicate quiesce command completion")
    if not done:
        return None
    require(done[0].get("source") == "go.fixture.append_only_control" and done[0].get("command_kind") == "quiesce_shutdown"
            and done[0].get("status") == "ok" and type(done[0].get("command_sequence")) is int
            and type(done[0].get("sequence")) is int and ack["sequence"] < done[0]["sequence"] <= len(events)
            and events[done[0]["sequence"] - 1] is done[0], "quiesce completion failed/mismatched")
    cleanup = {}
    _events(raw, "go", token, actor, active=active, cleanup_framing=cleanup)
    require(not any(event["sequence"] > ack["sequence"] and (event["kind"] == "protocol"
                    and cleanup.get(event["sequence"]) is not event
                    or event["kind"] == "rpc" and event.get("direction") == "write") for event in events),
            "native PubSub write/selected owner admitted after quiesce ACK")
    if active:
        require(all(type(raw.get(key)) is int and raw[key] == 0 for key in
                    ("active_stream_handlers_and_io", "active_fixture_workers", "active_callbacks")),
                "active quiesce result retains owned work")
        require(not any(event.get("kind") == "shutdown" for event in events), "quiesce claims completed host shutdown")
        require(raw.get("host_close_returned") is not True, "active quiesce claims completed Host.Close")
    return ack


def _go_pre_cancel_resets(raw, events, *, active):
    """Host-owned Reset RETURN ordering only, not lower QUIC or global join proof."""
    returns = [event for event in events if event["kind"] == "pre_cancel_retained_reset_return"]
    phases = [event for event in events if event["kind"] == "pre_cancel_retained_resets_returned"]
    acks = [event for event in events if event["kind"] == "shutdown_quiesced"]
    require(len(phases) <= 1 and len(acks) <= 1 and len(returns) <= 64, "ambiguous pre-cancel retained Reset phase")
    common = {"actor", "case_token", "local_peer_id", "pid", "command_sequence", "prepare_ack_sequence"}
    owner_fields = {"connection_id", "stream_id", "peer_id", "protocol", "protocol_receipt_sequence"}
    phase_fields = {"retained_owners", "reset_return_receipt_sequences", "native_admission_closed",
                    "pubsub_callback_admission_closed", "pubsub_context_cancelled", "subscriber_context_cancelled",
                    "active_stream_handlers_and_io", "active_pubsub_streams", "active_callbacks", "phase_scope"}
    for event in returns + phases:
        extra = owner_fields | {"operation", "outcome", "error", "error_type"} if event["kind"] == "pre_cancel_retained_reset_return" else phase_fields
        require(set(event) == EVENT_FIELDS | common | extra
                and event["source"] == "go.fixture.owned_pre_cancel_retained_resets"
                and all(_same_json(event[key], raw.get(key)) for key in ("actor", "case_token", "local_peer_id", "pid"))
                and type(event["pid"]) is int and event["pid"] > 0
                and type(event["command_sequence"]) is int and 2 <= event["command_sequence"] <= 64
                and type(event["prepare_ack_sequence"]) is int,
                "invalid/foreign pre-cancel retained Reset owner/fields")
        prepared = shutdown_ack(raw, "go", raw["actor"], raw["case_token"], raw["local_peer_id"],
                                event["command_sequence"] - 1, _check_operations=False)
        require(prepared is not None and prepared["sequence"] == event["prepare_ack_sequence"] < event["sequence"]
                and any(value["kind"] == "command_done" and value.get("command_kind") == "prepare_shutdown"
                        and type(value.get("command_sequence")) is int and value["command_sequence"] == event["command_sequence"] - 1
                        and prepared["sequence"] < value["sequence"] < event["sequence"] for value in events),
                "pre-cancel retained Reset lacks exact completed Prepare ACK")
        if event["kind"] == "pre_cancel_retained_reset_return":
            ref = event["protocol_receipt_sequence"]
            require(type(ref) is int and 0 < ref < event["sequence"] and all(_id(event[key]) for key in owner_fields - {"protocol_receipt_sequence"})
                    and event["protocol"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"}
                    and event["operation"] == "stream_reset" and event["outcome"] == "ok"
                    and event["error"] is None and event["error_type"] is None,
                    "failed/invalid pre-cancel retained full Reset RETURN")
            selected = events[ref - 1]
            require(selected["kind"] == "protocol" and selected["source"] == "go.network.Stream.Protocol"
                    and all(_same_json(selected.get(key), event[key]) for key in owner_fields - {"protocol_receipt_sequence"}),
                    "retained Reset RETURN has foreign host protocol owner")
            owners = [value for value in events if value["kind"] == "connection" and value.get("connection_id") == event["connection_id"]]
            selections = [value for value in events if value["kind"] == "protocol" and value["source"] == "go.network.Stream.Protocol"
                          and value.get("connection_id") == event["connection_id"] and value.get("stream_id") == event["stream_id"]]
            require(len(owners) == 1 and len(selections) == 1 and owners[0].get("authenticated") is True
                    and owners[0].get("peer_id") == event["peer_id"] and owners[0]["sequence"] < ref,
                    "retained Reset RETURN lacks unambiguous authenticated host owner")
    if not phases:
        require(not acks and (not returns or active), "missing pre-cancel retained Reset phase before quiesce ACK")
        return
    phase = phases[0]
    require(phase["native_admission_closed"] is True and phase["pubsub_callback_admission_closed"] is True
            and phase["pubsub_context_cancelled"] is False and phase["subscriber_context_cancelled"] is False
            and all(type(phase[key]) is int and 0 <= phase[key] < 2**32 for key in
                    ("active_stream_handlers_and_io", "active_pubsub_streams", "active_callbacks"))
            and phase["phase_scope"] == "retained_stream_Reset_returns_not_IO_framing_callback_lower_QUIC_or_router_join",
            "pre-cancel retained Reset phase falsely claims cancellation/join")
    refs, owners = phase["reset_return_receipt_sequences"], phase["retained_owners"]
    require(type(refs) is list and type(owners) is list and len(refs) == len(owners) == len(returns)
            and all(type(ref) is int and 0 < ref < phase["sequence"] for ref in refs)
            and refs == [event["sequence"] for event in returns] and len(set(refs)) == len(refs)
            and all(type(owner) is dict and set(owner) == owner_fields for owner in owners),
            "retained Reset phase has missing/duplicate/foreign snapshot/RETURN references")
    identities = set()
    for owner, event in zip(owners, returns):
        require(all(_same_json(owner[key], event[key]) for key in owner_fields)
                and all(_same_json(phase[key], event[key]) for key in common),
                "retained Reset phase differs from exact snapshot owner/RETURN")
        identity = (owner["connection_id"], owner["stream_id"])
        require(identity not in identities, "duplicate retained snapshot owner")
        identities.add(identity)
    require((active and not acks) or (len(acks) == 1 and phase["sequence"] < acks[0]["sequence"]
            and all(_same_json(phase[key], acks[0].get(key)) for key in common)),
            "pre-cancel retained Reset phase lacks ordered matching quiesce ACK")


def _framing_terminal(event, implementation):
    kind = event.get("kind")
    require(isinstance(kind, str), "invalid native framing event kind")
    require(kind not in {"native_io_error", "incomplete_rpc_frame"}
            and event.get("typed_cause") != "incomplete_rpc_frame",
            "native I/O/incomplete RPC observation remains fatal")
    if implementation == "go" and event.get("source") in GO_QUIC_SOURCES.values():
        if kind == "native_quic_negotiation_io_return":
            # Only the cross-record validator can authorize deferring a sealed
            # error through a complete diagnostic/cleanup chain, never this flag alone.
            require(event.get("outcome") in {"ok", "error"}, "unselected native I/O has unknown outcome")
        if kind == "stream_io_terminal":
            require(type(event.get("pending_frame_bytes")) is int and event["pending_frame_bytes"] == 0,
                    "native lower stream terminated with incomplete RPC residue")
            require(event.get("outcome") in {"eof", "owned_reset_pending", "owned_read_terminal_pending",
                                              "peer_zero_reset_pending", "connection_application_close_pending"},
                    "native lower QUIC I/O error remains fatal")
        if kind == "native_stream_operation":
            require(event.get("outcome") in {"ok", "native_send_reset_close_pending"}
                    or _quic_diagnostic_phase(event) and event.get("outcome") == "error",
                    "native lower QUIC close/reset error remains fatal")
        if kind == "native_quic_terminal_finalized":
            require(event.get("accepted") is True, "lower QUIC terminal was not actually accepted")
        if kind in {"native_quic_negotiation_cleanup_finalized", "native_quic_negotiation_abort_finalized"}:
            require(event.get("accepted") is True, "negotiation cleanup was not actually accepted")
        return
    if kind == "stream_io_terminal":
        direction = event.get("direction")
        require(implementation == "go" and isinstance(direction, str) and direction in {"read", "write"}
                and event.get("source") == "go.network.Stream." + direction,
                "invalid native stream terminal source/direction")
        require(type(event.get("pending_frame_bytes")) is int and event["pending_frame_bytes"] == 0,
                "native stream terminated with incomplete/missing RPC residue")
        require(type(event.get("successful_prefix_bytes")) is int
                and 0 <= event["successful_prefix_bytes"] <= 16 * 1024 * 1024
                and _id(event.get("connection_id")) and _id(event.get("stream_id"))
                and _id(event.get("remote_peer_id"))
                and isinstance(event.get("protocol"), str)
                and event["protocol"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"}
                and isinstance(event.get("error"), str) and 0 < len(event["error"]) <= 512,
                "unbounded/unowned native stream terminal record")
        require(event.get("operation") == "stream_" + direction
                and type(event.get("outcome")) is str and event["outcome"] in {
                    "eof", "owned_reset_pending", "peer_zero_reset_pending", "owned_read_terminal_pending"},
                "native stream I/O error remains fatal")
        if event["outcome"] == "eof":
            require(direction == "read" and event.get("typed_cause") == "io_eof", "invalid native EOF operation")
    if kind in {"native_stream_operation", "native_stream_close_finalized", "native_stream_io_finalized"}:
        require(implementation == "go" and event.get("source") == "go.pubsub.native_stream.native_operation",
                "invalid native stream operation authority")
        allowed = {"owned_reset_pending", "peer_zero_reset_pending", "owned_read_terminal_pending"} \
            if kind == "native_stream_io_finalized" else {"ok", "repeat_close_pending", "concurrent_reset_close_pending"}
        require(type(event.get("outcome")) is str and event["outcome"] in allowed, "native close/reset error remains fatal")
        if kind != "native_stream_operation":
            require(event.get("accepted") is True and event.get("framing_clean") is True
                    and event.get("io_joined") is True and event.get("read_finalized") is True
                    and event.get("write_finalized") is True and event.get("native_owner_disposed") is True
                    and type(event.get("pending_read_frame_bytes")) is int and event["pending_read_frame_bytes"] == 0
                    and type(event.get("pending_write_frame_bytes")) is int and event["pending_write_frame_bytes"] == 0,
                    "native terminal has unjoined/invalid RPC framing")


def _go_native_operations(events):
    """Bind terminal observations to immutable native operations and full disposal."""
    common = {"sequence", "mono_ns", "kind", "source", "connection_id", "stream_id", "remote_peer_id", "protocol",
              "operation", "started_order", "returned_order", "prepared", "native_yamux", "reset_receipt_sequence",
              "reset_returned_order", "requested_reset_code", "outcome", "typed_cause", "error", "error_type",
              "error_code", "remote", "prepare_ack_sequence", "reset_started_order", "transport_error_type",
              "transport_error_code", "transport_error_remote"}
    terminal = {"direction", "successful_prefix_bytes", "pending_frame_bytes"}
    peer = {"terminal_prepare_ack_sequence", "peer_reset_reason"}
    read_terminal = {"terminal_state_cause", "terminal_started_order", "terminal_returned_order", "terminal_receipt_sequence"}
    concurrent_close = {"terminal_state_cause", "native_close_succeeded"}
    finalized = {"operation_receipt_sequence", "accepted", "io_joined", "read_finalized", "write_finalized",
                 "pending_read_frame_bytes", "pending_write_frame_bytes", "framing_clean", "native_owner_disposed",
                 "owner_disposal_receipt_sequence"}
    causal_reset = {"causal_reset_receipt_sequence", "causal_reset_returned_order"}
    connections, streams, receipts, pending, resolutions, orders, acks = {}, {}, {}, {}, {}, {}, {}
    quiesced = False
    for event in events:
        kind = event.get("kind")
        if event.get("source") in GO_QUIC_SOURCES.values():
            continue
        if kind == "connection" and event.get("source") == "go.network.Conn.authenticated_output":
            require(_id(event.get("connection_id")), "invalid native connection owner")
            require(event["connection_id"] not in connections, "ambiguous native connection owner index")
            connections[event.get("connection_id")] = event
        elif kind == "protocol" and event.get("source") == "go.network.Stream.Protocol":
            require(_id(event.get("connection_id")) and _id(event.get("stream_id")), "invalid native stream owner")
            require((event["connection_id"], event["stream_id"]) not in streams, "ambiguous native stream owner index")
            streams[(event.get("connection_id"), event.get("stream_id"))] = event
        elif kind == "shutdown_prepared" and event.get("source") == "go.fixture.prepare_shutdown":
            require(type(event.get("sequence")) is int, "invalid native preparation sequence")
            acks[event["sequence"]] = event
        elif kind == "shutdown_quiesced" and event.get("source") == "go.fixture.owned_pubsub_quiesce":
            quiesced = True
        if kind not in {"stream_io_terminal", "native_stream_operation", "native_stream_close_finalized", "native_stream_io_finalized"}:
            continue
        _framing_terminal(event, "go")
        is_final = kind in {"native_stream_close_finalized", "native_stream_io_finalized"}
        is_io = kind in {"stream_io_terminal", "native_stream_io_finalized"}
        is_peer = event["outcome"] == "peer_zero_reset_pending"
        is_read_terminal = event["outcome"] == "owned_read_terminal_pending"
        is_concurrent_close = event["outcome"] == "concurrent_reset_close_pending"
        extra = ((terminal if is_io else set()) | (peer if is_peer else set())
                 | (read_terminal if is_read_terminal else set()) | (finalized if is_final else set())
                 | (concurrent_close if is_concurrent_close else set())
                 | ({"observed_terminal_receipt_sequence"} if is_final and is_read_terminal else set())
                 | ({"observed_reset_receipt_sequence", "observed_reset_returned_order", "finalization_order"}
                    if is_final and is_concurrent_close else set())
                 | (causal_reset if is_final and not is_peer and not is_read_terminal and not is_concurrent_close else set()))
        require(set(event) == common | extra, "native operation receipt fields differ from exact contract")
        require(type(event["sequence"]) is int and 1 <= event["sequence"] <= len(events)
                and events[event["sequence"] - 1] is event
                and type(event["started_order"]) is int and type(event["returned_order"]) is int
                and 0 < event["started_order"] < event["returned_order"] < 2**64
                and type(event["prepared"]) is bool and type(event["prepare_ack_sequence"]) is int
                and type(event["native_yamux"]) is bool
                and type(event["reset_receipt_sequence"]) is int and event["reset_receipt_sequence"] >= 0
                and type(event["reset_returned_order"]) is int and 0 <= event["reset_returned_order"] < 2**64
                and type(event["reset_started_order"]) is int and 0 <= event["reset_started_order"] < 2**64,
                "invalid native operation ordering/preparation receipt")
        require((event["prepared"] and event["prepare_ack_sequence"] in acks
                 and event["prepare_ack_sequence"] < event["sequence"])
                or (not event["prepared"] and event["prepare_ack_sequence"] == 0),
                "native operation lacks exact preparation-at-begin ACK")
        require(all(_id(event[key]) for key in ("connection_id", "stream_id", "remote_peer_id"))
                and _id(event["protocol"]) and event["protocol"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"}, "unowned native operation")
        owner = (event["connection_id"], event["stream_id"])
        connection, stream = connections.get(owner[0]), streams.get(owner)
        require(connection is not None and stream is not None and connection.get("authenticated") is True
                and connection.get("peer_id") == event["remote_peer_id"] == stream.get("peer_id")
                and stream.get("protocol") == event["protocol"], "native operation differs from authenticated stream owner")
        yamux = connection.get("transport") == "tcp" and connection.get("security") == "/noise" and connection.get("muxer") == "/yamux/1.0.0"
        require(event["native_yamux"] is yamux, "native operation muxer differs from authenticated connection")
        operation = event["operation"]
        require(type(operation) is str and operation in {"stream_read", "stream_write", "stream_close", "stream_close_read", "stream_close_write",
                              "stream_reset", "stream_reset_with_error"}, "unknown native stream operation")
        code = event["requested_reset_code"]
        require((operation == "stream_reset_with_error" and type(code) is int and 0 <= code < 2**32)
                or (operation != "stream_reset_with_error" and code is None), "invalid native Reset argument receipt")
        if is_io:
            direction = event["direction"]
            require(type(direction) is str and direction in {"read", "write"} and operation == "stream_" + direction
                    and type(event["pending_frame_bytes"]) is int and event["pending_frame_bytes"] == 0
                    and type(event["successful_prefix_bytes"]) is int and 0 <= event["successful_prefix_bytes"] <= 16 * 1024 * 1024,
                    "invalid native I/O prefix/residue receipt")
            if event["outcome"] == "eof":
                require(direction == "read" and event["typed_cause"] == "io_eof"
                        and event["error_code"] is None and event["remote"] is None
                        and event["error_type"] == "*errors.errorString", "native terminal is not an exact EOF observation")
        else:
            require(operation not in {"stream_read", "stream_write"}, "close receipt fabricates an I/O operation")
        if event["outcome"] == "ok":
            require(event["typed_cause"] == "none" and all(event[key] is None for key in ("error", "error_type", "error_code", "remote")),
                    "successful native operation contains an error")
        if event["outcome"] in {"owned_reset_pending", "peer_zero_reset_pending"}:
            require(is_io and event["native_yamux"] is True and event["typed_cause"] == "libp2p_stream_error"
                    and event["error_type"] == "*network.StreamError" and type(event["error_code"]) is int
                    and event["error_code"] == 0 and event["remote"] is is_peer
                    and event["transport_error_type"] == "*yamux.StreamError"
                    and type(event["transport_error_code"]) is int and event["transport_error_code"] == 0
                    and event["transport_error_remote"] is is_peer
                    and event["reset_receipt_sequence"] == 0 and event["reset_returned_order"] == 0
                    and isinstance(event["error"], str) and 0 < len(event["error"]) <= 512,
                    "native I/O lacks exact direct zero-reset cause")
            if is_peer:
                ack_ref = event["terminal_prepare_ack_sequence"]
                require(type(ack_ref) is int and ack_ref in acks and 0 < ack_ref < event["sequence"]
                        and event["peer_reset_reason"] == "unknown" and event["reset_started_order"] == 0,
                        "peer cancellation lacks actual preparation-at-return ACK or claims a cause")
            else:
                require(event["reset_started_order"] > 0, "owned Reset lacks captured START identity")
        else:
            require(all(event[key] is None for key in ("transport_error_type", "transport_error_code", "transport_error_remote")),
                    "native operation conceals an unrelated transport cause")
        if is_read_terminal:
            require(is_io and event["direction"] == "read" and event["native_yamux"] is True
                    and event["typed_cause"] == "yamux_reset_sentinel_pair" and event["error_type"] == "*fmt.wrapErrors"
                    and event["error_code"] is None and event["remote"] is None
                    and event["terminal_state_cause"] == "unknown"
                    and type(event["terminal_started_order"]) is int
                    and type(event["terminal_returned_order"]) is int
                    and 0 < event["terminal_started_order"] < event["terminal_returned_order"] < event["returned_order"]
                    and type(event["terminal_receipt_sequence"]) is int and event["terminal_receipt_sequence"] >= 0,
                    "read terminal lacks exact unknown-cause native sentinel/RETURN observation")
        if event["outcome"] == "repeat_close_pending":
            require(operation == "stream_close" and event["prepared"] is True and event["native_yamux"] is True
                    and event["typed_cause"] == "yamux_stream_error" and event["error_type"] == "*yamux.StreamError"
                    and type(event["error_code"]) is int and event["error_code"] == 0 and type(event["remote"]) is bool
                    and isinstance(event["error"], str) and 0 < len(event["error"]) <= 512,
                    "repeat Close lacks exact outer native Yamux StreamError0")
        elif is_concurrent_close:
            require(not quiesced, "concurrent Close observation occurs after actual quiesce ACK")
            require(operation == "stream_close" and event["prepared"] is True and event["native_yamux"] is True
                    and event["typed_cause"] == "yamux_stream_error" and event["error_type"] == "*yamux.StreamError"
                    and type(event["error_code"]) is int and event["error_code"] == 0 and event["remote"] is False
                    and event["terminal_state_cause"] == "unknown" and event["native_close_succeeded"] is False
                    and 0 < event["reset_started_order"] < event["started_order"]
                    and event["reset_receipt_sequence"] == 0 and event["reset_returned_order"] == 0
                    and isinstance(event["error"], str) and 0 < len(event["error"]) <= 512,
                    "concurrent Close lacks direct local Yamux0 and active Reset-at-BEGIN observation")
        elif operation != "stream_close":
            require(event["reset_receipt_sequence"] == 0 and event["reset_returned_order"] == 0
                    and (event["outcome"] == "owned_reset_pending" or event["reset_started_order"] == 0),
                    "native operation borrows unrelated Reset provenance")
        if is_final:
            reference = event["operation_receipt_sequence"]
            require(type(reference) is int and reference > 0 and reference not in resolutions,
                    "missing/duplicate native terminal finalization reference")
            resolutions[reference] = event
            if is_concurrent_close:
                seal = event["finalization_order"]
                used = orders.setdefault(owner, set())
                require(type(seal) is int and 0 < seal < 2**64 and seal not in used,
                        "invalid/reused concurrent Close finalization ordering seal")
                used.add(seal)
        else:
            used = orders.setdefault(owner, set())
            require(event["started_order"] not in used and event["returned_order"] not in used,
                    "reused native operation ordering counter")
            used.update((event["started_order"], event["returned_order"]))
            receipts[event["sequence"]] = event
            if event["outcome"] in {"repeat_close_pending", "concurrent_reset_close_pending", "owned_reset_pending", "peer_zero_reset_pending", "owned_read_terminal_pending"}:
                pending[event["sequence"]] = event
    require(set(pending) == set(resolutions), "native terminal lacks actual joined zero-residue finalization")
    for reference, event in resolutions.items():
        original = pending[reference]
        is_io = original["kind"] == "stream_io_terminal"
        is_peer = original["outcome"] == "peer_zero_reset_pending"
        is_read_terminal = original["outcome"] == "owned_read_terminal_pending"
        is_concurrent_close = original["outcome"] == "concurrent_reset_close_pending"
        require(event["kind"] == ("native_stream_io_finalized" if is_io else "native_stream_close_finalized")
                and reference < event["sequence"]
                and all(_same_json(original[key], event[key]) for key in (common - {"sequence", "mono_ns", "kind", "source"})
                        | (terminal if is_io else set()) | (peer if is_peer else set())
                        | (read_terminal if is_read_terminal else set()) | (concurrent_close if is_concurrent_close else set())),
                "native terminal finalization differs from immutable operation receipt")
        disposal_ref = event["owner_disposal_receipt_sequence"]
        require(type(disposal_ref) is int and disposal_ref > 0, "missing indexed native owner disposal receipt")
        disposal = receipts.get(disposal_ref)
        require(disposal is not None and disposal["kind"] == "native_stream_operation"
                and disposal["operation"] in {"stream_close", "stream_reset", "stream_reset_with_error"}
                and disposal["outcome"] == "ok" and disposal["error"] is None
                and disposal["sequence"] < event["sequence"]
                and all(_same_json(disposal[key], original[key]) for key in (
                    "connection_id", "stream_id", "remote_peer_id", "protocol", "native_yamux")),
                "native terminal lacks successful full disposal on exact wrapper")
        if is_concurrent_close:
            reset_ref = event["observed_reset_receipt_sequence"]
            reset = receipts.get(reset_ref) if type(reset_ref) is int else None
            require(reset is not None and reset_ref == disposal_ref and reset["operation"] == "stream_reset"
                    and reset["prepared"] is True and reset["prepare_ack_sequence"] == original["prepare_ack_sequence"]
                    and reset["started_order"] == original["reset_started_order"]
                    and type(event["observed_reset_returned_order"]) is int
                    and reset["returned_order"] == event["observed_reset_returned_order"]
                    and reset["started_order"] < original["started_order"] < reset["returned_order"],
                    "concurrent Close lacks the exact overlapping successful Prepared full Reset disposal")
            operations = [value for value in receipts.values()
                          if value["connection_id"] == original["connection_id"] and value["stream_id"] == original["stream_id"]]
            attempts = [value for value in operations if value["kind"] == "native_stream_operation"
                        and value["operation"] in {"stream_reset", "stream_reset_with_error"}
                        and value["started_order"] < original["started_order"]]
            require(attempts and max(value["started_order"] for value in attempts) == reset["started_order"],
                    "concurrent Close borrows a superseded Reset-at-BEGIN attempt")
            seal = event["finalization_order"]
            before_seal = [value for value in operations if value["started_order"] < seal]
            require(original["returned_order"] < seal and reset["returned_order"] < seal
                    and all(value["returned_order"] < seal for value in before_seal),
                    "concurrent Close seal precedes a native operation RETURN/join")
            attempts = [value for value in before_seal if value["kind"] == "native_stream_operation"
                        and value["operation"] in {"stream_reset", "stream_reset_with_error"}]
            require(attempts and max(value["started_order"] for value in attempts) == reset["started_order"],
                    "concurrent Close borrows a superseded Reset attempt")
            continue
        if is_peer:
            continue
        if is_read_terminal:
            terminal_ref = event["observed_terminal_receipt_sequence"]
            require(type(terminal_ref) is int and terminal_ref > 0, "missing indexed read-terminal operation receipt")
            operation = receipts.get(terminal_ref)
            require(operation is not None and operation["kind"] == "native_stream_operation"
                    and operation["operation"] in {"stream_close", "stream_close_read", "stream_reset"}
                    and operation["outcome"] == "ok" and operation["error"] is None
                    and operation["prepared"] is True and operation["prepare_ack_sequence"] in acks
                    and operation["sequence"] < event["sequence"]
                    and operation["started_order"] == original["terminal_started_order"]
                    and operation["returned_order"] == original["terminal_returned_order"]
                    and operation["returned_order"] < original["returned_order"]
                    and all(_same_json(operation[key], original[key]) for key in (
                        "connection_id", "stream_id", "remote_peer_id", "protocol", "native_yamux")),
                    "read terminal lacks returned successful Prepared operation on exact wrapper")
            published = original["terminal_receipt_sequence"]
            require(published == 0 or (published == terminal_ref and operation["sequence"] < reference),
                    "read terminal borrows unobserved/foreign publication sequence")
            attempts = [value for value in receipts.values() if value["kind"] == "native_stream_operation"
                        and value["operation"] in {"stream_close", "stream_close_read", "stream_reset", "stream_reset_with_error"}
                        and value["connection_id"] == original["connection_id"] and value["stream_id"] == original["stream_id"]
                        and value["started_order"] < original["returned_order"]]
            require(attempts and max(value["started_order"] for value in attempts) == operation["started_order"],
                    "read terminal borrows superseded terminal operation")
            continue
        reset_ref = event["causal_reset_receipt_sequence"]
        require(type(reset_ref) is int and type(event["causal_reset_returned_order"]) is int,
                "invalid causal Reset receipt/counter type")
        reset = receipts.get(reset_ref)
        require(reset is not None and reset["kind"] == "native_stream_operation" and reset["operation"] == "stream_reset"
                and reset["outcome"] == "ok" and reset["sequence"] < event["sequence"]
                and reset["started_order"] == original["reset_started_order"]
                and reset["returned_order"] == event["causal_reset_returned_order"]
                and all(_same_json(reset[key], original[key]) for key in ("connection_id", "stream_id", "remote_peer_id", "protocol", "native_yamux")),
                "native terminal lacks successful full Reset on exact wrapper")
        cutoff = original["returned_order"] if is_io else original["started_order"]
        attempts = [value for value in receipts.values() if value["kind"] == "native_stream_operation"
                    and value["operation"] in {"stream_reset", "stream_reset_with_error"}
                    and value["connection_id"] == original["connection_id"] and value["stream_id"] == original["stream_id"]
                    and value["started_order"] < cutoff]
        require(attempts and max(value["started_order"] for value in attempts) == reset["started_order"],
                "native terminal borrows superseded Reset provenance")
        if is_io:
            require(reset["prepared"] is True and reset["prepare_ack_sequence"] in acks
                    and reset["started_order"] < original["returned_order"],
                    "owned Reset did not begin Prepared before native I/O return")
        else:
            published_at_begin = original["reset_receipt_sequence"]
            require((published_at_begin == 0 or (published_at_begin == reset_ref and reset["sequence"] < reference))
                    and reset["returned_order"] == original["reset_returned_order"]
                    and reset["returned_order"] < original["started_order"],
                    "repeat Close lacks prior returned successful Reset")


def prepared_snapshot(snapshot, row, raw, pid, *, terminal_success=True):
    """Bind an immutable active ACK capture to the actual owner and final event prefix."""
    fields = {"schema_version", "source", "actor", "case_token", "pid", "command_sequence",
              "ack_event_sequence", "result"}
    require(isinstance(snapshot, dict) and set(snapshot) == fields
            and type(snapshot["schema_version"]) is int and snapshot["schema_version"] == 1
            and snapshot["source"] == "python.fixture.native_prepare_snapshot"
            and snapshot["actor"] == row["actor"] and snapshot["case_token"] == row["case_token"]
            and type(snapshot["pid"]) is int and snapshot["pid"] == pid
            and type(snapshot["command_sequence"]) is int and snapshot["command_sequence"] == row["command_sequence"]
            and type(snapshot["ack_event_sequence"]) is int and snapshot["ack_event_sequence"] == row["ack_event_sequence"],
            "prepare snapshot differs from exact indexed owner/ACK reference")
    captured = snapshot["result"]
    ack = shutdown_ack(captured, raw["implementation"], row["actor"], row["case_token"], row["local_peer_id"],
                       row["command_sequence"], active=True)
    require(ack is not None and ack["sequence"] == row["ack_event_sequence"], "missing indexed active prepare ACK")
    require(type(terminal_success) is bool, "invalid snapshot proof scope")
    if terminal_success:
        final_ack = shutdown_ack(raw, raw["implementation"], row["actor"], row["case_token"], row["local_peer_id"],
                                 row["command_sequence"])
        require(_same_json(final_ack, ack), "final actual ACK differs from captured active ACK")
    require(_same_json(raw["events"][:len(captured["events"])], captured["events"]),
            "indexed prepare ACK/event prefix differs from native terminal result")
    require(not any(event.get("kind") in {"connection_closed", "shutdown", "shutdown_requested", "native_close",
                                          "host_muxer_dropped", "expected_native_close", "native_terminal_state"}
                    or event.get("kind") == "native_quic_send_context" and not _quic_diagnostic_phase(event)
                    or event.get("kind") in {"native_quic_connection_context",
                                             "native_quic_framing_finalized", "native_quic_terminal_finalized", "native_quic_join",
                                             "native_quic_negotiation_cleanup_finalized"}
                    or event.get("outcome") in {"peer_zero_reset_pending", "owned_read_terminal_pending",
                                                "owned_reset_pending", "connection_application_close_pending",
                                                "native_send_reset_close_pending"}
                    for event in captured["events"]), "native close/peer cancellation occurred before active preparation capture")
    return captured


def _yamux_terminal_state(raw, event, events):
    fields = {"sequence", "mono_ns", "kind", "source", "operation", "io_kind", "raw_os_error", "typed_cause",
              "prepared", "prepare_ack_sequence", "error_boundary", "closure_reason", "message",
              "connection_trace_id", "stream_trace_id", "remote_peer_id", "swarm_connection_id", "peer_id",
              "connection_id", "stream_id", "endpoint", "native_stack"}
    boundary = "libp2p_yamux_public_into_io"
    require(set(event) == fields and raw["implementation"] == "rust"
            and event["typed_cause"] in {"yamux012_closed", "yamux013_closed"}
            and event["closure_reason"] == "unknown" and event["error_boundary"] == boundary
            and event["prepared"] is True and type(event["prepare_ack_sequence"]) is int
            and event["operation"] in {"muxer_inbound", "muxer_outbound", "muxer_poll", "muxer_close"}
            and event["io_kind"] == "Other" and event["raw_os_error"] is None
            and isinstance(event["message"], str) and 0 < len(event["message"]) <= 512,
            "invalid public Yamux terminal-state receipt")
    require(type(event["connection_trace_id"]) is int and 1 <= event["connection_trace_id"] <= 16
            and _id(event["connection_id"]) and event["swarm_connection_id"] == event["connection_id"]
            and _id(event["peer_id"]) and event["remote_peer_id"] == event["peer_id"]
            and event["stream_trace_id"] is None and event["stream_id"] is None,
            "Yamux terminal state lacks exact native muxer owner")
    owners = [owner for owner in events if owner.get("kind") == "connection"
              and type(owner.get("connection_trace_id")) is int
              and owner["connection_trace_id"] == event["connection_trace_id"]
              and owner.get("connection_id") == event["connection_id"] and owner.get("peer_id") == event["peer_id"]]
    require(len(owners) == 1, "missing/ambiguous authenticated Yamux terminal owner")
    owner, stack = owners[0], event["native_stack"]
    require(owner["sequence"] < event["sequence"] and owner.get("authenticated") is True
            and owner.get("owner_basis") == "exact_native_transport_output_muxer"
            and owner.get("swarm_connection_id") == owner.get("connection_id")
            and owner.get("remote_peer_id") == owner.get("peer_id")
            and owner.get("transport") == "tcp" and owner.get("security") == "/noise"
            and owner.get("muxer") == "/yamux/1.0.0" and _id(owner.get("remote_address"))
            and owner.get("authentication_basis") in {AUTHENTICATION["rust"]["tcp"], AUTHENTICATION["rust"]["tcp-pnet-noise"]}
            and owner.get("muxer_error_boundary") == boundary
            and isinstance(stack, dict)
            and set(stack) == {"transport", "security", "muxer", "authentication_basis", "muxer_error_boundary"}
            and stack.get("muxer_error_boundary") == boundary
            and all(stack.get(key) == owner.get(key) for key in ("transport", "security", "muxer", "authentication_basis"))
            and _same_json(stack, owner.get("native_stack")) and isinstance(event["endpoint"], dict)
            and _same_json(event["endpoint"], owner.get("endpoint")),
            "Yamux terminal state borrowed a foreign/non-opt-in transport owner")
    acks = [value for value in events if value.get("kind") == "shutdown_prepared"]
    require(len(acks) == 1, "Yamux terminal state lacks actual preparation")
    ack = shutdown_ack(raw, "rust", raw["actor"], raw["case_token"], raw["local_peer_id"], acks[0].get("command_sequence"))
    require(ack is not None and ack["sequence"] == event["prepare_ack_sequence"]
            and ack["sequence"] < event["sequence"], "Yamux terminal state preceded its actual prepare ACK")
    _terminal_owners(raw)
    disposed = [value for value in raw["native_close"]["connections"]
                if type(value.get("connection_trace_id")) is int
                and value["connection_trace_id"] == event["connection_trace_id"]
                and value.get("connection_id") == event["connection_id"] and value.get("peer_id") == event["peer_id"]]
    require(len(disposed) == 1 and disposed[0].get("dropped") is True
            and (disposed[0].get("close_returned") is True or disposed[0].get("native_terminal_observed") is True),
            "Yamux typed Closed did not establish actual owner close/drop/join")


def _events(raw, implementation, token, actor, *, cleanup_framing=None, active=False):
    require(isinstance(raw, dict) and type(raw.get("schema_version")) is int
            and raw["schema_version"] == 1, "missing PubSub actor schema")
    require(raw.get("implementation") == implementation and raw.get("case_token") == token
            and raw.get("actor") == actor and _id(raw.get("local_peer_id")), "actor identity/token mismatch")
    require(type(active) is bool and raw.get("finalized") is (not active) and raw.get("joined") is (not active)
            and raw.get("overflow") is False and raw.get("error") is None, "unjoined/failed/overflowed actor")
    events, previous = raw.get("events"), 0
    require(isinstance(events, list) and 0 < len(events) <= EVENT_LIMIT, "missing bounded PubSub events")
    require(len(json.dumps(raw).encode()) <= 16 * 1024 * 1024, "actor trace exceeds byte bound")
    for sequence, event in enumerate(events, 1):
        require(isinstance(event, dict) and type(event.get("sequence")) is int and event["sequence"] == sequence,
                "noncontiguous native event sequence")
        require(type(event.get("mono_ns")) is int and event["mono_ns"] >= previous and event["mono_ns"] > 0,
                "invalid native event clock")
        require(isinstance(event.get("source"), str) and event["source"].startswith(implementation + "."),
                "event lacks attributable native source")
        require(event.get("kind") in SOURCES[implementation]
                and event["source"] in SOURCES[implementation][event["kind"]], "event has wrong native authority")
        if event.get("kind") == "rpc":
            require(event.get("direction") in {"read", "write"}, "native RPC has unknown direction")
            if implementation == "go":
                require(event["source"] in {"go.pubsub.native_stream." + event["direction"],
                                             "go.quic.native_stream." + event["direction"]},
                        "native RPC direction/source mismatch")
        if event.get("kind") == "command_done":
            require(event.get("status", "ok") == "ok", "native command failed")
        if event.get("kind") == "native_rejected_stream_disposal":
            require(set(event) == EVENT_FIELDS | {"connection_id", "stream_id", "peer_id", "protocol_at_disposal",
                    "operation", "started_order", "returned_order", "outcome", "error", "error_type"}
                    and _id(event.get("connection_id")) and _id(event.get("stream_id")) and _id(event.get("peer_id"))
                    and event.get("protocol_at_disposal") in {"/meshsub/1.0.0", "/meshsub/1.1.0"}
                    and event.get("operation") == "stream_reset" and event.get("outcome") == "ok"
                    and event.get("error") is None and event.get("error_type") is None
                    and type(event.get("started_order")) is int and type(event.get("returned_order")) is int
                    and 0 < event["started_order"] < event["returned_order"] < 2 ** 64,
                    "failed/invalid rejected native stream disposal")
        _framing_terminal(event, implementation)
        if event.get("kind") == "native_terminal_state":
            _yamux_terminal_state(raw, event, events)
        if event.get("kind") == "expected_native_close":
            require(event.get("prepared") is True
                    and event.get("typed_cause") in {"quinn_application_closed_0", "quinn_locally_closed"}
                    and event.get("operation") in {"stream_read", "stream_write", "stream_flush", "stream_close",
                                                   "muxer_inbound", "muxer_outbound", "muxer_poll", "muxer_close"}
                    and isinstance(event.get("io_kind"), str) and 0 < len(event["io_kind"]) <= 64
                    and "raw_os_error" in event and event["raw_os_error"] is None
                    and isinstance(event.get("message"), str) and len(event["message"]) <= 512,
                    "invalid typed normal-close observation")
            require(type(event.get("connection_trace_id")) is int and 1 <= event["connection_trace_id"] <= 16
                    and _id(event.get("connection_id")) and event.get("swarm_connection_id") == event["connection_id"]
                    and _id(event.get("peer_id")) and event.get("remote_peer_id") == event["peer_id"],
                    "normal close lacks exact native connection ownership")
            stream = event.get("stream_trace_id")
            if event["operation"].startswith("stream_"):
                require(type(stream) is int and 1 <= stream <= 64
                        and event.get("stream_id") == f'{event["connection_trace_id"]}:{stream}',
                        "normal close lacks exact native stream ownership")
            else:
                require(stream is None and event.get("stream_id") is None, "muxer close has fabricated stream owner")
        require(len(json.dumps(event).encode()) <= 64 * 1024, "event exceeds byte bound")
        previous = event["mono_ns"]
    if implementation == "go":
        _go_native_operations(events)
        verified_cleanup = _go_quic_operations(raw, events, terminal=not active)
        _go_pre_cancel_resets(raw, events, active=active)
        if cleanup_framing is not None:
            cleanup_framing.update(verified_cleanup)
    return events


def _terminal_owners(raw):
    if raw["implementation"] == "rust":
        close, tasks = raw.get("native_close"), raw.get("task_join")
        require(isinstance(close, dict) and all(type(close.get(key)) is int and close[key] == 0
                                               for key in ("live_muxers", "live_streams")), "Rust native owners still live")
        owners = close.get("connections")
        require(isinstance(owners, list) and 0 < len(owners) <= 16
                and all(isinstance(owner, dict) and owner.get("dropped") is True
                        and (owner.get("connection_id") is None or owner.get("close_returned") is True
                             or owner.get("native_terminal_observed") is True) for owner in owners),
                "Rust authenticated owner lacks actual close/terminal/drop")
        require(isinstance(tasks, dict) and tasks.get("fixture_owned_tasks_joined") is True
                and tasks.get("overflow") is False and tasks.get("errors") == [], "Rust native executor tasks did not join")
    if raw["implementation"] == "go":
        require(raw.get("host_close_returned") is True
                and all(type(raw.get(key)) is int and raw[key] == 0 for key in
                        ("active_stream_handlers_and_io", "active_fixture_workers", "active_callbacks")),
                "Go native host/stream/worker/callback owners did not close/join")


def _donor_shutdown_event(raw, events):
    implementation = raw["implementation"]
    require(implementation in {"go", "rust"}, "shutdown join has no actual donor owner")
    kind = "shutdown" if implementation == "go" else "shutdown_requested"
    matches = [event for event in events if event["kind"] == kind]
    require(len(matches) == 1 and matches[0].get("source") in SOURCES[implementation][kind],
            "missing/duplicate/foreign native donor shutdown event")
    event = matches[0]
    if implementation == "go":
        require(set(event) == EVENT_FIELDS | {"context_cancelled", "joined", "host_close_returned",
                                             "active_stream_handlers_and_io", "active_fixture_workers"}
                and event["context_cancelled"] is True and event["joined"] is True
                and event["host_close_returned"] is True
                and all(type(event[key]) is int and event[key] == raw[key] == 0
                        for key in ("active_stream_handlers_and_io", "active_fixture_workers")),
                "Go shutdown event lacks actual host/IO/worker join")
    else:
        require(set(event) == EVENT_FIELDS | {"listeners"}
                and type(event["listeners"]) is int and 0 <= event["listeners"] <= 16,
                "invalid native Rust host-close request")
    return event


def _shutdown_barrier(artifact, actors, events):
    barrier = artifact.get("shutdown_barrier")
    require(isinstance(barrier, dict) and set(barrier) == {"source", "operations"}
            and barrier.get("source") == "python.fixture.all_actor_prepare_barrier",
            "missing causal shutdown barrier")
    operations = barrier.get("operations")
    names = set(actors)
    donors = {actor for actor, raw in actors.items() if raw["implementation"] in {"go", "rust"}}
    go = {actor for actor, raw in actors.items() if raw["implementation"] == "go"}
    count, go_count = len(donors), len(go)
    stops_begin, joins_begin, forge_begin = 4 + 2 * go_count, 4 + 2 * go_count + count, 4 + 2 * go_count + 2 * count
    require(isinstance(operations, list) and len(operations) == 8 + count + 2 * go_count
            and all(isinstance(row, dict) and row.get("actor") in names for row in operations),
            "missing all-actor prepare/stop/donor-join operations")
    require({row["actor"] for row in operations[:4]} == names
            and {row["actor"] for row in operations[4:4 + go_count]} == go
            and {row["actor"] for row in operations[4 + go_count:stops_begin]} == go
            and {row["actor"] for row in operations[stops_begin:joins_begin]} == donors
            and {row["actor"] for row in operations[joins_begin:forge_begin]} == donors
            and {row["actor"] for row in operations[forge_begin:]} == names - donors,
            "missing/duplicate/foreign shutdown actor phase")
    for index, row in enumerate(operations, 1):
        fields = {"sequence", "kind", "actor", "case_token", "local_peer_id"}
        requested = 4 < index <= 4 + go_count
        quiesced = 4 + go_count < index <= stops_begin
        joined = joins_begin < index <= forge_begin
        if index <= 4:
            fields |= {"command_sequence", "ack_event_sequence", "evidence_file"}
        elif joined:
            fields |= {"pid", "shutdown_event_sequence"}
        elif requested or quiesced:
            fields |= {"pid", "command_sequence", "prepare_ack_sequence"}
            if quiesced:
                fields.add("quiesce_event_sequence")
        require(set(row) == fields, "shutdown operation has missing fields or undeclared claims")
        require(isinstance(row, dict) and type(row.get("sequence")) is int and row["sequence"] == index
                and row.get("kind") == ("prepare_ack" if index <= 4 else "quiesce_requested" if requested
                                        else "quiesce_ack" if quiesced else "donor_joined" if joined else "stop_requested"),
                "stop/join violated all-actor prepare or donor-before-Forge order")
        actor, raw = row["actor"], actors[row["actor"]]
        require(row.get("local_peer_id") == raw["local_peer_id"] and row.get("case_token") == artifact["case_token"],
                "runner shutdown operation has foreign identity/token")
        if requested or quiesced:
            prepared = next(value for value in operations[:4] if value["actor"] == actor)
            require(type(row["pid"]) is int and row["pid"] > 0 and row["pid"] == artifact["processes"][actor]["pid"]
                    and type(row["command_sequence"]) is int and row["command_sequence"] == prepared["command_sequence"] + 1
                    and type(row["prepare_ack_sequence"]) is int
                    and row["prepare_ack_sequence"] == prepared["ack_event_sequence"], "foreign Go quiesce operation owner/Prepare")
            if quiesced:
                request = next(value for value in operations[4:4 + go_count] if value["actor"] == actor)
                require(all(_same_json(row[key], request[key]) for key in
                            ("actor", "case_token", "local_peer_id", "pid", "command_sequence", "prepare_ack_sequence")),
                        "quiesce ACK differs from actual request")
                ack = quiesce_ack(raw, actor, artifact["case_token"], raw["local_peer_id"], row["pid"],
                                  row["command_sequence"], row["prepare_ack_sequence"])
                require(ack is not None and type(row["quiesce_event_sequence"]) is int
                        and row["quiesce_event_sequence"] == ack["sequence"]
                        < _donor_shutdown_event(raw, events[actor])["sequence"], "missing exact pre-Stop Go quiesce ACK")
        if joined:
            process = artifact["processes"][actor]
            status = process.get("terminal_status")
            require(type(row["pid"]) is int and row["pid"] > 0 and row["pid"] == process["pid"]
                    and type(process.get("returncode")) is int and process["returncode"] == 0
                    and process.get("forced_termination") is False
                    and isinstance(status, dict) and set(status) == {"exit_code", "termination"}
                    and type(status["exit_code"]) is int and status["exit_code"] == 0
                    and status["termination"] == "graceful", "donor join differs from actual graceful process owner")
            _terminal_owners(raw)
            shutdown = _donor_shutdown_event(raw, events[actor])
            ack_row = next(value for value in operations[:4] if value["actor"] == actor)
            require(type(row["shutdown_event_sequence"]) is int
                    and row["shutdown_event_sequence"] == shutdown["sequence"] > ack_row["ack_event_sequence"],
                    "donor join lacks exact post-Prepare native shutdown reference")
        if index > 4:
            continue
        require(isinstance(row["evidence_file"], str) and 0 < len(row["evidence_file"]) <= 4096,
                "missing bounded prepare snapshot reference")
        ack = shutdown_ack(raw, raw["implementation"], actor, artifact["case_token"], raw["local_peer_id"],
                           row.get("command_sequence"))
        require(ack is not None and type(row.get("ack_event_sequence")) is int
                and row["ack_event_sequence"] == ack["sequence"], "missing exact terminal prepare ack")
        for event in events[actor]:
            if event["kind"] in {"connection_closed", "shutdown", "shutdown_requested", "native_close",
                                 "host_muxer_dropped", "expected_native_close", "native_terminal_state"}:
                require(event["sequence"] > ack["sequence"], "native close preceded preparation")
            if event["kind"] == "native_terminal_state":
                require(event["prepare_ack_sequence"] == ack["sequence"]
                        and event["native_stack"]["authentication_basis"] == AUTHENTICATION["rust"].get(PROFILES[artifact["case"]["profile"]]),
                        "Yamux terminal state differs from exact prepare barrier/profile")
            if event["kind"] == "expected_native_close":
                owners = [owner for owner in events[actor] if owner["kind"] == "connection"
                          and owner.get("connection_trace_id") == event.get("connection_trace_id")
                          and owner.get("connection_id") == event.get("connection_id")
                          and owner.get("peer_id") == event.get("peer_id") and owner.get("transport") == "quic"
                          and owner.get("authenticated") is True]
                require(len(owners) == 1 and owners[0]["sequence"] < event["sequence"], "normal close lacks native QUIC owner")
            if event["kind"] == "command_done" and event["sequence"] > ack["sequence"]:
                lifecycle = raw["implementation"] == "go" and event.get("command_kind") == "quiesce_shutdown" \
                    and type(event.get("command_sequence")) is int and event["command_sequence"] == ack["command_sequence"] + 1
                require(lifecycle or event.get("command_sequence") == ack["command_sequence"]
                        and event.get("command_kind") == "prepare_shutdown", "fixture command admitted after preparation")
    prepared = {row["actor"]: row["ack_event_sequence"] for row in operations[:4]}
    peers = {raw["local_peer_id"]: actor for actor, raw in actors.items()}
    for actor, owned_events in events.items():
        for event in owned_events:
            if event.get("outcome") == "peer_zero_reset_pending":
                remote = peers.get(event["remote_peer_id"])
                require(event["terminal_prepare_ack_sequence"] == prepared[actor]
                        and remote in prepared and remote != actor,
                        "peer cancellation lacks exact all-actor preparation barrier/owner")


def _single(events, kind, **fields):
    found = [event for event in events if event.get("kind") == kind
             and all(event.get(key) == value for key, value in fields.items())]
    require(len(found) == 1, f"missing/ambiguous native {kind}: {fields}")
    return found[0]


def _snapshot(events, label):
    event = _single(events, "snapshot", label=label)
    mesh, scores = event.get("mesh_peer_ids"), event.get("peer_scores")
    require(isinstance(mesh, list) and len(mesh) <= 128 and all(_id(peer) for peer in mesh)
            and len(set(mesh)) == len(mesh), "unbounded/duplicate native mesh")
    require(isinstance(scores, list) and len(scores) <= 128, "missing bounded native scores")
    seen = set()
    for score in scores:
        require(isinstance(score, dict) and _id(score.get("peer_id")) and score["peer_id"] not in seen
                and _number(score.get("value")), "ambiguous/nonfinite native score")
        if score.get("invalid_deliveries_available") is False:
            require(score.get("invalid_deliveries") is None
                    and score.get("counter_limitation") == "not exposed by pinned public Behaviour"
                    and event["source"].startswith("rust."), "unsupported native score counter exception")
        else:
            require(_number(score.get("invalid_deliveries")) and score["invalid_deliveries"] >= 0,
                    "missing/nonfinite native invalid delivery counter")
        seen.add(score["peer_id"])
    return event


def _score(snapshot, peer):
    found = [score for score in snapshot["peer_scores"] if score["peer_id"] == peer]
    require(len(found) == 1, "native peer score is missing")
    return found[0]


def _connection_owner(events, connection_id, peer, implementation, transport, expected_fingerprint):
    require(_id(connection_id) and _id(peer) and implementation in AUTHENTICATION
            and transport in AUTHENTICATION[implementation], "invalid native carrier identity/profile")
    connections = [value for value in events if value.get("kind") == "connection"
                   and value.get("connection_id") == connection_id]
    require(len(connections) == 1, "native connection owner is missing/ambiguous")
    owner = connections[0]
    require(implementation in AUTHENTICATION and owner.get("source") in SOURCES[implementation]["connection"]
            and owner.get("peer_id") == peer and owner.get("authenticated") is True and _id(owner.get("remote_address"))
            and owner.get("authentication_basis") == AUTHENTICATION[implementation][transport],
            "native carrier owner is not natively authenticated")
    suffix = r"(?:/p2p/" + re.escape(peer) + r")?"
    host = r"/(?:ip4/[0-9.]+|ip6/[0-9a-fA-F:]+)/"
    if transport == "quic":
        require(owner.get("transport") == "quic" and owner.get("security") == "/tls/1.0.0"
                and owner.get("muxer") in (None, "", "quic")
                and re.fullmatch(host + r"udp/[1-9][0-9]*/quic-v1" + suffix, owner["remote_address"]),
                "native QUIC authentication/transport mismatch")
    else:
        require(owner.get("transport") == "tcp" and owner.get("security") == "/noise"
                and owner.get("muxer") == "/yamux/1.0.0"
                and re.fullmatch(host + r"tcp/[1-9][0-9]*" + suffix, owner["remote_address"]),
                "native TCP Noise/Yamux owner mismatch")
        if transport == "tcp-pnet-noise":
            require(owner.get("pnet_verified") is True and expected_fingerprint is not None
                    and owner.get("pnet_fingerprint") == expected_fingerprint,
                    "private PubSub has no actual protected owner")
    return owner


def _rpc_peer(event):
    if "peer_id" in event and "remote_peer_id" in event:
        require(event["peer_id"] == event["remote_peer_id"], "conflicting native RPC peer identities")
    if event.get("source") in {"go.quic.native_stream.read", "go.quic.native_stream.write"}:
        return event.get("remote_peer_id")
    return event.get("peer_id")


def _quic_indexed(events, event):
    require(type(event.get("sequence")) is int and 1 <= event["sequence"] <= len(events)
            and events[event["sequence"] - 1] is event
            and type(event.get("mono_ns")) is int and 0 < event["mono_ns"] < 2**63,
            "lower QUIC receipt lacks exact indexed native sequence/clock")


def _quic_ref(events, reference, kind, before):
    require(type(reference) is int and 1 <= reference < before <= len(events) + 1,
            "missing/future/noninteger lower QUIC receipt reference")
    event = events[reference - 1]
    _quic_indexed(events, event)
    source = "go.fixture.prepare_shutdown" if kind == "shutdown_prepared" else GO_QUIC_SOURCES[kind]
    require(event.get("sequence") == reference and type(event.get("sequence")) is int
            and event.get("kind") == kind and event.get("source") == source,
            "lower QUIC reference has wrong native authority")
    return event


def _quic_context(value, *, live=False):
    require(isinstance(value, dict) and set(value) == QUIC_CONTEXT_FIELDS and type(value["done"]) is bool,
            "invalid exact native QUIC context snapshot")
    if live or not value["done"]:
        require(value["done"] is False and all(value[key] is None for key in QUIC_CONTEXT_FIELDS - {"done"}),
                "native QUIC context was already terminal at preparation")
    else:
        require(isinstance(value["cause_type"], str) and 0 < len(value["cause_type"]) <= 128
                and isinstance(value["error"], str) and 0 < len(value["error"]) <= 512,
                "missing bounded native context cause")
        if value["cause_type"] in {"*quic.StreamError", "*qerr.ApplicationError"}:
            require(type(value["error_code"]) is int and 0 <= value["error_code"] < 2**62
                    and type(value["remote"]) is bool
                    and (type(value["native_stream_id"]) is int and 0 <= value["native_stream_id"] < 2**62
                         if value["cause_type"] == "*quic.StreamError" else value["native_stream_id"] is None),
                    "invalid native typed context metadata")
        else:
            require(all(value[key] is None for key in ("error_code", "remote", "native_stream_id")),
                    "opaque native context has invented typed metadata")
    return value


def _quic_zero_context(value, stream=None):
    _quic_context(value)
    require(value["done"] is True and type(value["error_code"]) is int and value["error_code"] == 0
            and type(value["remote"]) is bool
            and value["cause_type"] == ("*qerr.ApplicationError" if stream is None else "*quic.StreamError")
            and (value["native_stream_id"] is None if stream is None else
                 type(value["native_stream_id"]) is int and value["native_stream_id"] == stream),
            "native QUIC context has nonzero/wrapped/foreign cause")
    return value


def _go_quic_connection(events, connection):
    _quic_indexed(events, connection)
    fields = EVENT_FIELDS | {"native_connection_id", "local_peer_id", "remote_peer_id", "local_address",
                            "remote_address", "remote_public_key_sha256", "native_connection_basis",
                            "authentication_basis", "secured_callback", "prepare_ack_sequence",
                            "connection_context_at_capable_return"}
    require(set(connection) == fields and connection.get("kind") == "native_quic_connection"
            and connection.get("source") == GO_QUIC_SOURCES["native_quic_connection"]
            and _id(connection["native_connection_id"]) and _id(connection["local_peer_id"])
            and _id(connection["remote_peer_id"]) and connection["native_connection_basis"] == "CapableConn.As(**quic.Conn)"
            and connection["authentication_basis"] == "same_native_quic_Conn_and_InterceptSecured_capable_output"
            and type(connection["prepare_ack_sequence"]) is int and connection["prepare_ack_sequence"] == 0
            and isinstance(connection["remote_public_key_sha256"], str)
            and re.fullmatch(r"[a-f0-9]{64}", connection["remote_public_key_sha256"]),
            "unattributable lower QUIC capable connection")
    require(len([value for value in events if value.get("kind") == "native_quic_connection"
                 and value.get("native_connection_id") == connection["native_connection_id"]]) == 1,
            "ambiguous lower QUIC connection owner")
    secured = connection["secured_callback"]
    require(isinstance(secured, dict) and set(secured) == {"source", "native_connection_id", "native_connection_basis",
            "observed_mono_ns", "local_peer_id", "remote_peer_id", "local_address", "remote_address", "direction", "security_role"}
            and secured["source"] == "go.quic.transport.InterceptSecured"
            and secured["native_connection_basis"] == "network.Conn.As(**quic.Conn)"
            and type(secured["observed_mono_ns"]) is int and secured["observed_mono_ns"] > 0
            and secured["direction"] in {"inbound", "outbound"}
            and secured["security_role"] == ("server" if secured["direction"] == "inbound" else "client")
            and all(secured[key] == connection[key] for key in
                    ("native_connection_id", "local_peer_id", "remote_peer_id", "local_address", "remote_address")),
            "lower QUIC owner differs from actual secured callback")
    _quic_context(connection["connection_context_at_capable_return"], live=True)
    aliases = [value for value in events if value.get("kind") == "connection"
               and value.get("lower_connection_receipt_sequence") == connection["sequence"]
               and type(value.get("lower_connection_receipt_sequence")) is int]
    require(len(aliases) == 1, "missing/ambiguous actual capable-to-host connection receipt")
    host = _connection_owner(events, aliases[0].get("connection_id"), connection["remote_peer_id"], "go", "quic", None)
    require(host.get("lower_stream_binding_basis") == "same_native_CapableConn_not_Swarm_stream_ID_mapping"
            and host.get("remote_public_key_sha256") == connection["remote_public_key_sha256"]
            and host.get("remote_peer_id") == connection["remote_peer_id"]
            and host.get("local_peer_id") == connection["local_peer_id"]
            and host.get("local_address") == connection["local_address"]
            and host.get("remote_address") == connection["remote_address"]
            and _same_json(host.get("quic_security"), secured), "lower QUIC authentication borrowed a foreign host owner")
    return host


def _go_quic_stream(events, event, *, diagnostic=False):
    _quic_indexed(events, event)
    require(_id(event.get("native_connection_id")) and type(event.get("native_stream_id")) is int
            and 0 <= event["native_stream_id"] < 2**62 and event["native_stream_id"] & 2 == 0
            and isinstance(event.get("stream_direction"), str) and event["stream_direction"] in {"Inbound", "Outbound"}
            and event.get("owner_basis") == "same_native_CapableConn_returned_MuxedStream"
            and "stream_id" not in event and "connection_id" not in event and "peer_id" not in event,
            "lower QUIC receipt has invalid native owner or invented Swarm mapping")
    connection = _quic_ref(events, event.get("connection_receipt_sequence"), "native_quic_connection", event["sequence"])
    host = _go_quic_connection(events, connection)
    stream = event if event.get("kind") == "native_quic_stream" else _quic_ref(
        events, event.get("native_stream_receipt_sequence"), "native_quic_stream", event["sequence"])
    _quic_indexed(events, stream)
    require(set(stream) == EVENT_FIELDS | QUIC_OWNER_FIELDS | {"prepare_ack_sequence", "send_context_at_stream_return",
            "native_call_begin_prepare_ack_sequence", "native_call_begin_observation_basis"}
            and stream.get("source") == GO_QUIC_SOURCES["native_quic_stream"]
            and stream.get("protocol") == "" and type(stream.get("native_stream_receipt_sequence")) is int
            and stream["native_stream_receipt_sequence"] == 0
            and type(stream.get("prepare_ack_sequence")) is int
            and 0 <= stream["prepare_ack_sequence"] < stream["sequence"]
            and type(stream.get("native_call_begin_prepare_ack_sequence")) is int
            and 0 <= stream["native_call_begin_prepare_ack_sequence"] < stream["sequence"]
            and stream["native_call_begin_observation_basis"] == "published_Prepare_ACK_before_native_CapableConn_call"
            and (not stream["native_call_begin_prepare_ack_sequence"]
                 or stream["native_call_begin_prepare_ack_sequence"] == stream["prepare_ack_sequence"])
            and (diagnostic or stream["prepare_ack_sequence"] == 0)
            and all(stream.get(key) == event.get(key) for key in
                    ("native_connection_id", "native_stream_id", "connection_receipt_sequence", "remote_peer_id",
                     "stream_direction", "owner_basis"))
            and connection["native_connection_id"] == event["native_connection_id"]
            and connection["remote_peer_id"] == event.get("remote_peer_id")
            and len([value for value in events if value.get("kind") == "native_quic_stream"
                     and value.get("native_connection_id") == event["native_connection_id"]
                     and value.get("native_stream_id") == event["native_stream_id"]]) == 1,
            "missing/ambiguous/foreign actual lower QUIC stream")
    _quic_context(stream["send_context_at_stream_return"], live=not diagnostic)
    if stream["prepare_ack_sequence"]:
        ack = _quic_ref(events, stream["prepare_ack_sequence"], "shutdown_prepared", stream["sequence"])
        require(ack.get("local_peer_id") == connection["local_peer_id"], "late native stream has foreign Prepare owner")
    if stream["native_call_begin_prepare_ack_sequence"]:
        _quic_ref(events, stream["native_call_begin_prepare_ack_sequence"], "shutdown_prepared", stream["sequence"])
    require(connection["sequence"] < stream["sequence"], "native lower connection did not precede stream")
    return connection, stream


def _go_quic_owner(events, event, peer, protocol, transport, expected_fingerprint=None, *, diagnostic=False):
    """One lower-owner validator for active runner observations and terminal evidence."""
    require(transport == "quic" and expected_fingerprint is None and event.get("remote_peer_id") == peer
            and event.get("protocol") == protocol, "lower QUIC receipt has foreign profile/peer/protocol")
    _rpc_peer(event)
    connection, stream = _go_quic_stream(events, event, diagnostic=diagnostic)
    selected = [value for value in events if value.get("kind") == "protocol"
                and value.get("source") == GO_QUIC_SOURCES["protocol"]
                and value.get("native_connection_id") == event["native_connection_id"]
                and value.get("native_stream_id") == event["native_stream_id"]]
    require(len(selected) == 1, "missing/ambiguous lower QUIC protocol selection")
    selected = selected[0]
    _quic_indexed(events, selected)
    require(set(selected) == EVENT_FIELDS | QUIC_OWNER_FIELDS | {"negotiation_frame_sequences"}
            and selected["protocol"] == protocol and stream["sequence"] < selected["sequence"] <= event["sequence"]
            and all(_same_json(selected[key], event[key]) for key in QUIC_OWNER_FIELDS),
            "lower QUIC receipt lacks preceding actual protocol")
    references = selected["negotiation_frame_sequences"]
    require(isinstance(references, list) and 4 <= len(references) <= 18
            and all(type(ref) is int for ref in references) and references == sorted(set(references)),
            "invalid bounded lower multistream references")
    actual_frames = [value["sequence"] for value in events if value.get("kind") == "multistream_frame"
                     and value.get("native_connection_id") == event["native_connection_id"]
                     and _same_json(value.get("native_stream_id"), event["native_stream_id"])
                     and type(value.get("sequence")) is int and value["sequence"] < selected["sequence"]]
    require(references == actual_frames, "selected lower protocol omitted an observed negotiation frame")
    sides = {"read": [], "write": []}
    for reference in references:
        side, token = _quic_negotiation_frame(events, reference, event, selected["sequence"])
        sides[side].append(token)
    require(all(values and values[0] == HEADER for values in sides.values()), "missing bilateral lower multistream headers")
    proposals = sides["read" if stream["stream_direction"] == "Inbound" else "write"][1:]
    replies = sides["write" if stream["stream_direction"] == "Inbound" else "read"][1:]
    require(1 <= len(proposals) <= 8 and len(proposals) == len(replies)
            and all(token.startswith("/") and token != HEADER for token in proposals)
            and replies[:-1] == ["na"] * (len(replies) - 1)
            and proposals[-1] == replies[-1] == protocol, "lower multistream proposal/ACK mismatch")
    if event.get("kind") == "rpc":
        direction = event.get("direction")
        require(set(event) == EVENT_FIELDS | QUIC_OWNER_FIELDS | {"direction", "receipt"}
                and direction in {"read", "write"} and event.get("source") == "go.quic.native_stream." + direction,
                "lower RPC has wrong exact source/schema")
        validate_rpc_receipt(event.get("receipt"), protocol, direction)
    return connection, stream, selected


def _quic_same_owner(left, right):
    return all(_same_json(left.get(key), right.get(key)) for key in QUIC_OWNER_FIELDS)


def _quic_diagnostic_phase(event):
    return event.get("kind") in {"native_quic_negotiation_io_return", "native_stream_operation", "native_quic_send_context"} \
        and event.get("source") == GO_QUIC_SOURCES[event["kind"]] \
        and event.get("observation_phase") == "unselected_at_native_return"


def _quic_negotiation_frame(events, reference, owner, before):
    frame = _quic_ref(events, reference, "multistream_frame", before)
    side, receipt = frame.get("direction"), frame.get("receipt")
    require(set(frame) == EVENT_FIELDS | QUIC_OWNER_FIELDS | {"direction", "receipt"}
            and all(_same_json(frame.get(key), owner.get(key)) for key in QUIC_OWNER_FIELDS - {"protocol"})
            and side in {"read", "write"} and isinstance(receipt, dict) and set(receipt) == {"framed_hex", side},
            "foreign/malformed lower multistream frame")
    token = wire_token(receipt["framed_hex"])
    wire = bytes.fromhex(receipt["framed_hex"])
    require(frame["protocol"] == token and _same_json(receipt[side], {
        "framed_bytes": len(wire), "frames": 1, "framed_sha256": hashlib.sha256(wire).hexdigest(),
        "complete_frames": True, "invalid_or_over_limit": False}), "lower multistream bytes/hash differ")
    return side, token


def _quic_prepare_rows(raw, event, events, reference):
    ack = _quic_ref(events, reference, "shutdown_prepared", event["sequence"])
    actual = shutdown_ack(raw, "go", raw["actor"], raw["case_token"], raw["local_peer_id"], ack.get("command_sequence"),
                          active=False, _check_operations=False)
    require(actual is ack, "lower QUIC terminal has no actual owned Prepare ACK")
    rows = ack.get("native_quic_context_snapshots")
    require(isinstance(rows, list) and len(rows) <= 64, "missing bounded native QUIC prepare snapshots")
    indexed = {}
    for row in rows:
        require(isinstance(row, dict) and set(row) == {"native_connection_id", "native_stream_id", "send_context", "connection_context"}
                and _id(row["native_connection_id"]) and type(row["native_stream_id"]) is int
                and 0 <= row["native_stream_id"] < 2**62 and row["native_stream_id"] & 2 == 0,
                "invalid lower QUIC Prepare owner snapshot")
        key = (row["native_connection_id"], row["native_stream_id"])
        require(key not in indexed, "duplicate lower QUIC Prepare owner snapshot")
        indexed[key] = row
        _quic_context(row["send_context"])
        _quic_context(row["connection_context"], live=True)
    expected = {(value["native_connection_id"], value["native_stream_id"]) for value in events
                if value.get("kind") == "native_quic_stream" and value["sequence"] < ack["sequence"]}
    require(set(indexed) == expected, "missing/foreign lower QUIC stream at actual Prepare")
    return indexed


def _quic_baseline(raw, event, events, reference, *, live=False):
    rows = _quic_prepare_rows(raw, event, events, reference)
    row = rows.get((event["native_connection_id"], event["native_stream_id"]))
    require(row is not None, "missing lower QUIC stream at actual Prepare")
    if live:
        _quic_context(row["send_context"], live=True)
    return row


def _quic_captured_bytes(value, bound):
    require(isinstance(value, dict) and set(value) == {"bytes", "sha256", "hex", "capture_complete"}
            and value["capture_complete"] is True and type(value["bytes"]) is int and 0 <= value["bytes"] <= bound
            and isinstance(value["hex"], str) and len(value["hex"]) == 2 * value["bytes"]
            and re.fullmatch(r"[0-9a-f]*", value["hex"])
            and isinstance(value["sha256"], str) and re.fullmatch(r"[0-9a-f]{64}", value["sha256"]),
            "incomplete/over-limit diagnostic byte capture")
    data = bytes.fromhex(value["hex"])
    require(hashlib.sha256(data).hexdigest() == value["sha256"], "diagnostic byte capture hash differs")
    return data


def _quic_negotiation_snapshot(events, event, *, is_io):
    snapshot = event["negotiation_snapshot"]
    fields = {"capture_complete", "selected_protocol", "proposal", "reply", "touched_pubsub", "parser_failed",
              "proposals", "frame_sequences", "snapshot_basis", "read", "write"} | ({"successful_prefix"} if is_io else set())
    require(isinstance(snapshot, dict) and set(snapshot) == fields and snapshot["capture_complete"] is True
            and snapshot["parser_failed"] is False and type(snapshot["touched_pubsub"]) is bool
            and type(snapshot["proposals"]) is int and 0 <= snapshot["proposals"] <= 8
            and snapshot["snapshot_basis"] == "parser_state_after_successful_prefix_observation"
            and isinstance(event["protocol"], str) and len(event["protocol"]) <= 255
            and snapshot["selected_protocol"] == event["protocol"], "invalid/failed diagnostic negotiation snapshot")
    refs = snapshot["frame_sequences"]
    require(isinstance(refs, list) and len(refs) <= 18 and all(type(ref) is int for ref in refs)
            and refs == sorted(set(refs)) and refs == [value["sequence"] for value in events
                if value.get("kind") == "multistream_frame" and value.get("native_connection_id") == event["native_connection_id"]
                and _same_json(value.get("native_stream_id"), event["native_stream_id"])
                and value["sequence"] < event["sequence"]], "diagnostic omitted/forged negotiation frame references")
    states = {side: {"header_seen": False, "paused": False, "token": ""} for side in ("read", "write")}
    proposal = reply = selected = ""
    proposals, touched = 0, False
    proposer = "read" if event["stream_direction"] == "Inbound" else "write"
    for reference in refs:
        side, token = _quic_negotiation_frame(events, reference, event, event["sequence"])
        state = states[side]
        require(not selected and not state["paused"], "diagnostic negotiation continued after selection/pause")
        touched |= token in {"/meshsub/1.0.0", "/meshsub/1.1.0"}
        if not state["header_seen"]:
            require(token == HEADER, "diagnostic negotiation lacks actual header")
            state["header_seen"] = True
            continue
        require(token != HEADER, "diagnostic negotiation repeated header")
        state.update(paused=True, token=token)
        if side == proposer:
            require(token != "na", "diagnostic NA is not a proposal")
            proposal, proposals = token, proposals + 1
        else:
            reply = token
        if proposal and reply:
            if reply == "na":
                proposal = reply = ""
                for state in states.values():
                    state.update(paused=False, token="")
            else:
                require(proposal == reply, "diagnostic wrong proposal/ACK")
                selected = proposal
    require((proposal, reply, selected, proposals) == (snapshot["proposal"], snapshot["reply"], snapshot["selected_protocol"], snapshot["proposals"])
            and touched is snapshot["touched_pubsub"], "diagnostic state contradicts indexed negotiation")
    captured = 0
    for side, state in states.items():
        actual = snapshot[side]
        require(isinstance(actual, dict) and set(actual) == set(state) | {"partial_frame", "lazy_tail"}
                and all(_same_json(actual[key], value) for key, value in state.items()), "diagnostic direction state differs")
        partial = _quic_captured_bytes(actual["partial_frame"], 266)
        tail = _quic_captured_bytes(actual["lazy_tail"], 16 * 1024 + 10)
        require(not (partial and (state["paused"] or selected)) and not (tail and (not state["paused"] or selected or state["token"] == "na")),
                "diagnostic partial/lazy bytes contradict parser phase")
        if partial:
            header = 2 if partial[0] & 128 else 1
            if len(partial) >= header:
                size = partial[0] & 127
                if header == 2:
                    require(0 < partial[1] < 128, "diagnostic partial varint is noncanonical")
                    size |= partial[1] << 7
                require(1 <= size <= 256 and len(partial) < header + size, "diagnostic partial frame is complete/over-limit")
        captured += len(partial) + len(tail)
    if is_io:
        require(type(event["requested_bytes"]) is int and 0 <= event["requested_bytes"] <= 16 * 1024 * 1024
                and type(event["successful_prefix_bytes"]) is int and 0 <= event["successful_prefix_bytes"] <= event["requested_bytes"]
                and event["successful_prefix_valid"] is True, "invalid diagnostic native I/O prefix")
        prefix = _quic_captured_bytes(snapshot["successful_prefix"], 4 * 1024 * 1024)
        require(len(prefix) == event["successful_prefix_bytes"], "diagnostic prefix length differs from native return")
        captured += len(prefix)
    return captured, touched


def _go_quic_diagnostic(raw, events, event, *, cleanup_error=False, empty_error=False, abort_error=False, abort_close=False, abort_peer_read=False):
    """Unselected native returns describe physical owners, never accepted PubSub outcomes."""
    connection, stream = _go_quic_stream(events, event, diagnostic=True)
    require(connection["local_peer_id"] == raw["local_peer_id"] and _quic_diagnostic_phase(event)
            and event.get("protocol_at_native_return") == "", "invalid diagnostic native-return phase/owner")
    common = EVENT_FIELDS | QUIC_OWNER_FIELDS | {"started_order", "returned_order", "prepare_ack_sequence", "send_context",
                                                "observation_phase", "protocol_at_native_return"}
    is_context = event["kind"] == "native_quic_send_context"
    passive = {"terminal_prepare_ack_sequence", "prepare_snapshot_ack_sequence", "prepare_baseline_present",
               "stream_prepare_baseline_present", "connection_prepare_baseline_present", "send_context_at_prepare",
               "connection_context_at_prepare", "connection_context", "context_observation_basis", "negotiation_snapshot"}
    is_io = event["kind"] == "native_quic_negotiation_io_return"
    extra = {"observation_basis", "connection_context"} if is_context else passive | {
        "operation", "error", "error_type", "outcome", "typed_cause"} | (
        {"direction", "requested_bytes", "successful_prefix_bytes", "successful_prefix_valid"} if is_io else {"requested_reset_code"})
    if not is_context and event.get("operation") == "stream_close":
        extra |= {"native_close_error_classification", "send_context_receipt_sequence"}
    if cleanup_error or empty_error:
        extra |= {"error_code", "remote", "transport_error_type", "transport_error_code", "transport_error_remote",
                  "same_native_connection_context_cause"}
    if abort_error:
        extra |= {"error_code", "remote", "transport_error_type", "transport_error_code", "transport_error_remote",
                  "transport_native_stream_id", "same_send_context_cause"}
    if abort_close:
        extra |= {"terminal_state_cause", "native_close_succeeded", "observed_reset_started_order", "observed_reset_returned_order"}
    require(set(event) == common | extra and type(event["started_order"]) is int and type(event["returned_order"]) is int
            and 0 < event["started_order"] < event["returned_order"] < 2**64,
            "diagnostic fields/order differ from exact native contract")
    for field in ("send_context", "connection_context"):
        context = _quic_context(event[field])
        require(context["native_stream_id"] is None or _same_json(context["native_stream_id"], event["native_stream_id"]),
                "diagnostic context has foreign native stream cause")
        if field == "connection_context":
            require(context["native_stream_id"] is None, "diagnostic connection borrowed stream cause")
    ack_fields = ["prepare_ack_sequence"] if is_context else ["prepare_ack_sequence", "terminal_prepare_ack_sequence", "prepare_snapshot_ack_sequence"]
    for field in ack_fields:
        ref = event[field]
        require(type(ref) is int and 0 <= ref < event["sequence"], "invalid diagnostic Prepare reference")
        if ref:
            _quic_prepare_rows(raw, event, events, ref)
    if stream["prepare_ack_sequence"]:
        require(all(event[field] == stream["prepare_ack_sequence"] for field in ack_fields), "late diagnostic changed its actual preceding ACK")
    if is_context:
        require(event["observation_basis"] == "same_lower_delegate_Context_at_actual_Close_RETURN", "diagnostic context was not at Close return")
        if event["protocol"]:
            _go_quic_owner(events, event, event["remote_peer_id"], event["protocol"], "quic", diagnostic=True)
        return 0, False
    if cleanup_error or empty_error:
        require(is_io and (empty_error or event["operation"] == "stream_read" and event["direction"] == "read")
                and type(event["successful_prefix_bytes"]) is int and event["successful_prefix_bytes"] == 0
                and event["outcome"] == "error" and event["error_type"] == "*network.ConnError"
                and event["typed_cause"] == "libp2p_quic_application_error"
                and event["transport_error_type"] == "*qerr.ApplicationError"
                and type(event["error_code"]) is int and event["error_code"] == 0
                and type(event["transport_error_code"]) is int and event["transport_error_code"] == 0
                and type(event["remote"]) is bool and event["transport_error_remote"] is event["remote"]
                and event["same_native_connection_context_cause"] is True
                and isinstance(event["error"], str) and 0 < len(event["error"]) <= 512,
                "diagnostic error is not a sealed direct same-parent AppClosed0 native return")
        parent = _quic_zero_context(event["connection_context"])
        require(parent["remote"] is event["remote"], "diagnostic native return borrowed a mismatched parent cause")
    elif abort_error:
        require(is_io and event["outcome"] == "error" and event["error_type"] == "*network.StreamError"
                and event["typed_cause"] == "libp2p_quic_stream_error" and event["transport_error_type"] == "*quic.StreamError"
                and type(event["error_code"]) is int and event["error_code"] == 0
                and type(event["transport_error_code"]) is int and event["transport_error_code"] == 0
                and type(event["remote"]) is bool and event["transport_error_remote"] is event["remote"]
                and type(event["transport_native_stream_id"]) is int and event["transport_native_stream_id"] == event["native_stream_id"]
                and type(event["same_send_context_cause"]) is bool and event["successful_prefix_bytes"] == 0
                and isinstance(event["error"], str) and 0 < len(event["error"]) <= 512,
                "abort is not an exact direct same-stream zero native error")
    elif abort_close:
        require(not is_io and event["operation"] == "stream_close" and event["outcome"] == "error"
                and event["typed_cause"] == "opaque" and event["native_close_error_classification"] == "opaque_unwrapped_native_error"
                and event["error_type"] == "*errors.errorString" and isinstance(event["error"], str) and 0 < len(event["error"]) <= 512
                and event["terminal_state_cause"] == "unknown" and event["native_close_succeeded"] is False
                and type(event["observed_reset_started_order"]) is int and type(event["observed_reset_returned_order"]) is int
                and 0 < event["observed_reset_started_order"] < event["observed_reset_returned_order"] < event["started_order"],
                "abort Close is wrapped/typed/nonopaque or borrows a future Reset")
        require(_quic_zero_context(event["send_context"], event["native_stream_id"])["remote"] is False or abort_peer_read,
                "abort Close lacks its actual zero send context for this abort branch")
    else:
        require(event["outcome"] == "ok" and event["error"] is None and event["error_type"] is None and event["typed_cause"] == "none",
                "diagnostic native error remains fatal")
    require(event["context_observation_basis"] == "send_context_after_seal_and_parent_context_sealed_at_native_return"
            and (not event["prepare_ack_sequence"] or event["prepare_ack_sequence"] == event["terminal_prepare_ack_sequence"])
            and (not event["terminal_prepare_ack_sequence"] or event["terminal_prepare_ack_sequence"] == event["prepare_snapshot_ack_sequence"]),
            "diagnostic retrofitted native BEGIN/RETURN preparation")
    live = {"done": False, "cause_type": None, "error_code": None, "remote": None, "native_stream_id": None, "error": None}
    rows = _quic_prepare_rows(raw, event, events, event["prepare_snapshot_ack_sequence"]) if event["prepare_snapshot_ack_sequence"] else {}
    row = rows.get((event["native_connection_id"], event["native_stream_id"]))
    parent_rows = [value for key, value in rows.items() if key[0] == event["native_connection_id"]]
    require(event["stream_prepare_baseline_present"] is (row is not None)
            and event["connection_prepare_baseline_present"] is bool(parent_rows)
            and event["prepare_baseline_present"] is (row is not None and bool(parent_rows))
            and _same_json(event["send_context_at_prepare"], row["send_context"] if row else live)
            and _same_json(event["connection_context_at_prepare"], parent_rows[0]["connection_context"] if parent_rows else live)
            and all(_same_json(value["connection_context"], event["connection_context_at_prepare"]) for value in parent_rows)
            and (not stream["prepare_ack_sequence"] or bool(parent_rows)), "diagnostic fabricated/missing actual Prepare baseline")
    if is_io:
        require(event["direction"] in {"read", "write"} and event["operation"] == "stream_" + event["direction"], "invalid diagnostic I/O direction")
    else:
        require(event["operation"] in {"stream_close", "stream_close_read", "stream_close_write", "stream_reset", "stream_reset_with_error"}
                and (type(event["requested_reset_code"]) is int and event["requested_reset_code"] == 0
                     if event["operation"] == "stream_reset_with_error" else event["requested_reset_code"] is None), "invalid diagnostic reset/close operation")
        if event["operation"] == "stream_close":
            context = _quic_ref(events, event["send_context_receipt_sequence"], "native_quic_send_context", event["sequence"])
            require(_quic_diagnostic_phase(context) and _quic_same_owner(context, event)
                    and all(_same_json(context[field], event[field]) for field in
                            ("started_order", "returned_order", "prepare_ack_sequence", "send_context", "connection_context"))
                    and event["native_close_error_classification"] == ("opaque_unwrapped_native_error" if abort_close else "none"),
                    "diagnostic Close borrowed/fabricated native context")
    captured, touched = _quic_negotiation_snapshot(events, event, is_io=is_io)
    if empty_error:
        ack = event["prepare_ack_sequence"]
        require(event["protocol"] == "" and ack > 0
                and stream["native_call_begin_prepare_ack_sequence"] == stream["prepare_ack_sequence"] == ack
                and event["prepare_baseline_present"] is False and event["stream_prepare_baseline_present"] is False
                and event["connection_prepare_baseline_present"] is True
                and event["negotiation_snapshot"]["frame_sequences"] == [] and captured == 0 and touched is False,
                "empty diagnostic error lacks actual late birth/ACK or contains negotiation bytes")
        _quic_context(event["connection_context_at_prepare"], live=True)
    if event["protocol"]:
        _go_quic_owner(events, event, event["remote_peer_id"], event["protocol"], "quic", diagnostic=True)
    return captured, touched


def _quic_subscription_candidate(data, topic):
    """Mirror the donor's canonical Marshal equality, without creating RPC authority."""
    size, header = _varint(data, 0)
    require(0 < size <= 16 * 1024 and len(data) == header + size and data[:header] == varint(size),
            "cleanup candidate is partial/trailing/noncanonical RPC framing")
    fields = _fields(data[header:])
    require(set(fields) == {1} and len(fields[1]) == 1 and fields[1][0][0] == 2,
            "cleanup candidate is not exactly one SUBSCRIBE-only RPC")
    sub = _fields(fields[1][0][1])
    encoded_topic = topic.encode("utf-8")
    require(0 < len(encoded_topic) <= 255 and {1, 2} <= set(sub) <= {1, 2, 3, 4}
            and sub[1] == [(0, 1)] and sub[2] == [(2, encoded_topic)]
            and all(sub[key] == [(0, 0)] for key in (3, 4) if key in sub),
            "cleanup candidate has foreign topic, duplicate/unknown fields or partial capability")
    canonical_sub = b"\x08\x01\x12" + varint(len(encoded_topic)) + encoded_topic
    canonical_sub += (b"\x18\x00" if 3 in sub else b"") + (b"\x20\x00" if 4 in sub else b"")
    require(data[header:] == b"\x0a" + varint(len(canonical_sub)) + canonical_sub,
            "cleanup candidate protobuf bytes are not canonical")


def _quic_cleanup_shape(events, event, *, finalized=False):
    _go_quic_stream(events, event, diagnostic=True)
    extra = {"operation_receipt_sequence", "candidate_write_receipt_sequence", "connection_context_receipt_sequence",
             "prepare_ack_sequence", "candidate_protocol", "candidate_protocol_frame_sequence", "candidate_bytes_complete",
             "selected_rpc_authority", "negotiation_complete", "framing_clean", "terminal_outcome",
             "same_native_connection_context_cause", "peer_header_frame_sequence"}
    if finalized:
        extra |= {"pending_receipt_sequence", "framing_receipt_sequence", "owner_disposal_receipt_sequence",
                  "first_owner_disposal_receipt_sequence", "native_join_receipt_sequence", "accepted"}
    require(set(event) == EVENT_FIELDS | QUIC_OWNER_FIELDS | extra
            and event["source"] == "go.quic.native_stream.negotiation_cleanup"
            and event["protocol"] == "" and event["stream_direction"] == "Outbound"
            and isinstance(event["candidate_protocol"], str) and event["candidate_protocol"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"}
            and event["candidate_bytes_complete"] is True and event["selected_rpc_authority"] is False
            and event["negotiation_complete"] is False and event["framing_clean"] is False
            and event["same_native_connection_context_cause"] is True
            and event["terminal_outcome"] == ("negotiation_aborted_cleanup" if finalized else "negotiation_aborted_cleanup_pending")
            and (not finalized or event["accepted"] is True)
            and type(event["peer_header_frame_sequence"]) is int
            and 0 <= event["peer_header_frame_sequence"] < event["sequence"]
            and all(type(event[field]) is int and 0 < event[field] < event["sequence"] for field in extra
                    if field.endswith("receipt_sequence") or field in {"prepare_ack_sequence", "candidate_protocol_frame_sequence"}),
            "invalid exact cleanup-only receipt schema/authority/phase")
    return event["native_connection_id"], event["native_stream_id"]


def _quic_cleanup_pending(raw, events, event):
    key = _quic_cleanup_shape(events, event)
    original = _quic_ref(events, event["operation_receipt_sequence"], "native_quic_negotiation_io_return", event["sequence"])
    write = _quic_ref(events, event["candidate_write_receipt_sequence"], "native_quic_negotiation_io_return", original["sequence"])
    parent = _quic_ref(events, event["connection_context_receipt_sequence"], "native_quic_connection_context", original["sequence"])
    _go_quic_diagnostic(raw, events, write)
    _go_quic_diagnostic(raw, events, original, cleanup_error=True)
    connection, stream = _go_quic_stream(events, original, diagnostic=True)
    ack = event["prepare_ack_sequence"]
    require(_quic_same_owner(write, event) and _quic_same_owner(original, event)
            and connection["sequence"] < ack < stream["sequence"] < write["sequence"] < original["sequence"]
            and stream["prepare_ack_sequence"] == stream["native_call_begin_prepare_ack_sequence"] == ack
            and write["operation"] == "stream_write" and write["direction"] == "write"
            and write["returned_order"] < original["returned_order"]
            and all(value[field] == ack for value in (write, original) for field in
                    ("prepare_ack_sequence", "terminal_prepare_ack_sequence", "prepare_snapshot_ack_sequence"))
            and all(value["prepare_baseline_present"] is False and value["stream_prepare_baseline_present"] is False
                    and value["connection_prepare_baseline_present"] is True for value in (write, original)),
            "cleanup candidate is not the actual late-born prepared owner/Write/Read")
    _quic_context(stream["send_context_at_stream_return"], live=True)
    _quic_context(write["connection_context_at_prepare"], live=True)
    require(parent["native_connection_id"] == connection["native_connection_id"]
            and parent["remote_peer_id"] == connection["remote_peer_id"] and parent["connection_receipt_sequence"] == connection["sequence"]
            and type(parent["prepare_ack_sequence"]) is int and parent["prepare_ack_sequence"] == ack
            and parent["prepare_baseline_present"] is True
            and _same_json(parent["context_at_prepare"], original["connection_context_at_prepare"])
            and _same_json(parent["context"], original["connection_context"]),
            "cleanup error borrowed a different/current-after-return parent context")
    snapshot = write["negotiation_snapshot"]
    terminal_snapshot = original["negotiation_snapshot"]
    peer_header = event["peer_header_frame_sequence"]
    require(snapshot["selected_protocol"] == snapshot["reply"] == "" and snapshot["proposal"] == event["candidate_protocol"]
            and snapshot["proposals"] == 1 and snapshot["touched_pubsub"] is True and len(snapshot["frame_sequences"]) == 2
            and event["candidate_protocol_frame_sequence"] == snapshot["frame_sequences"][1]
            and terminal_snapshot["frame_sequences"] == snapshot["frame_sequences"] + ([peer_header] if peer_header else [])
            and _same_json({key: value for key, value in terminal_snapshot.items()
                            if key not in {"successful_prefix", "read", "frame_sequences"}},
                           {key: value for key, value in snapshot.items()
                            if key not in {"successful_prefix", "read", "frame_sequences"}}),
            "cleanup candidate changed parser state/ACK/proposal after Write")
    if peer_header:
        side, token = _quic_negotiation_frame(events, peer_header, original, original["sequence"])
        require(write["sequence"] < peer_header and side == "read" and token == HEADER,
                "cleanup peer header is not the exact third canonical read frame after Write")
    require(terminal_snapshot["read"]["header_seen"] is bool(peer_header)
            and terminal_snapshot["read"]["paused"] is False and terminal_snapshot["read"]["token"] == ""
            and terminal_snapshot["read"]["partial_frame"]["bytes"] == 0
            and terminal_snapshot["read"]["lazy_tail"]["bytes"] == 0,
            "cleanup peer header acquired reply/selection or partial/tail bytes")
    prefix = _quic_captured_bytes(snapshot["successful_prefix"], 16 * 1024 + 512)
    require(write["requested_bytes"] == write["successful_prefix_bytes"] == len(prefix) and len(prefix) > 0,
            "cleanup Write was not a complete native successful prefix")
    wire = b""
    for index, reference in enumerate(snapshot["frame_sequences"]):
        side, token = _quic_negotiation_frame(events, reference, write, write["sequence"])
        require(side == "write" and token == (HEADER if index == 0 else event["candidate_protocol"]),
                "cleanup candidate lacks canonical outbound header/proposal")
        wire += bytes.fromhex(events[reference - 1]["receipt"]["framed_hex"])
    tail = _quic_captured_bytes(snapshot["write"]["lazy_tail"], 16 * 1024 + 10)
    require(prefix == wire + tail and not snapshot["read"]["header_seen"] and not snapshot["read"]["paused"]
            and snapshot["read"]["token"] == "" and snapshot["read"]["partial_frame"]["bytes"] == 0
            and snapshot["read"]["lazy_tail"]["bytes"] == 0 and snapshot["write"]["partial_frame"]["bytes"] == 0,
            "cleanup canonical prefix/tail did not fully consume the observed bytes")
    _quic_subscription_candidate(tail, "forge-pr11:" + raw["case_token"])
    require(not any(value.get("kind") in {"protocol", "rpc", "stream_io_terminal", "native_quic_terminal_finalized"}
                    and value.get("native_connection_id") == key[0] and _same_json(value.get("native_stream_id"), key[1])
                    for value in events), "cleanup owner acquired selected RPC/terminal authority")
    return original, write


def _quic_cleanup_framing(raw, events, event, pending):
    _go_quic_stream(events, event, diagnostic=True)
    extra = {"framing_clean", "io_joined", "read_finalized", "write_finalized", "negotiation_complete",
             "pending_read_frame_bytes", "pending_write_frame_bytes", "native_owner_disposed", "owner_disposal_receipt_sequence",
             "negotiation_snapshot", "candidate_bytes_complete", "selected_rpc_authority", "negotiation_cleanup_pending_receipt_sequence",
             "peer_header_frame_sequence", "cleanup_owner_disposal_receipt_sequence"}
    require(set(event) == EVENT_FIELDS | QUIC_OWNER_FIELDS | extra and _quic_same_owner(event, pending)
            and event["negotiation_complete"] is False and event["framing_clean"] is False
            and event["candidate_bytes_complete"] is True and event["selected_rpc_authority"] is False
            and all(event[field] is True for field in ("io_joined", "read_finalized", "write_finalized", "native_owner_disposed"))
            and all(type(event[field]) is int and event[field] == 0 for field in ("pending_read_frame_bytes", "pending_write_frame_bytes"))
            and type(event["negotiation_cleanup_pending_receipt_sequence"]) is int
            and event["negotiation_cleanup_pending_receipt_sequence"] == pending["sequence"]
            and _same_json(event["peer_header_frame_sequence"], pending["peer_header_frame_sequence"]),
            "cleanup framing is unjoined/incomplete or claims selected authority")
    captured, touched = _quic_negotiation_snapshot(events, event, is_io=False)
    original = events[pending["operation_receipt_sequence"] - 1]
    require(touched and _same_json(event["negotiation_snapshot"], {key: value for key, value in original["negotiation_snapshot"].items()
                                                                if key != "successful_prefix"}), "cleanup framing changed its candidate bytes/parser")
    first = _quic_ref(events, event["owner_disposal_receipt_sequence"], "native_stream_operation", event["sequence"])
    disposal = _quic_ref(events, event["cleanup_owner_disposal_receipt_sequence"], "native_stream_operation", event["sequence"])
    full_disposals = [value for value in events if value.get("kind") == "native_stream_operation"
                     and _quic_same_owner(value, pending) and _quic_diagnostic_phase(value)
                     and value.get("operation") in {"stream_close", "stream_reset", "stream_reset_with_error"}
                     and value.get("outcome") == "ok" and value.get("error") is None
                     and value["sequence"] < event["sequence"]]
    require(full_disposals and first is full_disposals[0], "cleanup rewrote/borrowed its original first owner disposal")
    post_return = [value for value in full_disposals if value["operation"] in {"stream_close", "stream_reset"}
                   and value["prepare_ack_sequence"] == pending["prepare_ack_sequence"]
                   and original["returned_order"] < value["started_order"]]
    require(post_return and disposal is min(post_return, key=lambda value: value["returned_order"]),
            "cleanup post-Read disposal pin is missing/prior/foreign or rewritten")
    require(_quic_same_owner(disposal, pending) and disposal["operation"] in {"stream_close", "stream_reset"}
            and _quic_diagnostic_phase(disposal) and disposal["outcome"] == "ok" and disposal["error"] is None
            and disposal["error_type"] is None and disposal["typed_cause"] == "none"
            and disposal["prepare_ack_sequence"] == pending["prepare_ack_sequence"]
            and disposal["sequence"] < event["sequence"]
            and original["returned_order"] < disposal["started_order"] < disposal["returned_order"]
            and _same_json(disposal["negotiation_snapshot"], event["negotiation_snapshot"]),
            "cleanup lacks actual subsequent successful full owner Close/Reset")
    require(_same_json(disposal["connection_context"], original["connection_context"]),
            "cleanup disposal changed its sealed original parent context")
    send = _quic_context(disposal["send_context"])
    if send["done"]:
        if send["cause_type"] == "*qerr.ApplicationError":
            require(_same_json(send, original["connection_context"]), "cleanup disposal has a foreign parent cause")
        else:
            _quic_zero_context(send, event["native_stream_id"])
    return captured


def _quic_native_join(lower):
    joins = [event for event in lower if event["kind"] == "native_quic_join"]
    require(len(joins) == 1, "missing/ambiguous lower native QUIC join")
    joined = joins[0]
    require(set(joined) == EVENT_FIELDS | {"active_native_calls", "observed_connections", "observed_streams", "joined_scope"}
            and type(joined["active_native_calls"]) is int and joined["active_native_calls"] == 0
            and type(joined["observed_connections"]) is int and joined["observed_connections"] == len(
                [event for event in lower if event["kind"] == "native_quic_connection"])
            and type(joined["observed_streams"]) is int and joined["observed_streams"] == len(
                [event for event in lower if event["kind"] == "native_quic_stream"])
            and joined["joined_scope"] == "fixture_lower_stream_IO_and_operations_not_all_donor_goroutines"
            and all(event["sequence"] < joined["sequence"] for event in lower if event is not joined
                    and event["kind"] not in {"native_quic_negotiation_cleanup_finalized", "native_quic_negotiation_abort_finalized"}
                    and not (event["kind"] == "native_quic_terminal_finalized"
                             and event.get("terminal_outcome") == "negotiation_abort_close_pending")),
            "lower QUIC join has live calls, mismatched owners or later native observations")
    return joined


def _quic_stream_aborts(raw, events, *, terminal):
    """Independently bound whole-owner abort bytes/disposal; never RPC authority."""
    pendings = [value for value in events if value["kind"] == "native_quic_negotiation_abort_pending"]
    finals = [value for value in events if value["kind"] == "native_quic_negotiation_abort_finalized"]
    close_pendings = [value for value in events if value["kind"] == "native_quic_negotiation_abort_close_pending"]
    close_finals = [value for value in events if value["kind"] == "native_quic_terminal_finalized"
                    and value.get("terminal_outcome") == "negotiation_abort_close_pending"]
    errors, closes, proofs, owners_seen, final_refs, close_refs, close_pending_refs = set(), {}, {}, set(), set(), set(), set()
    identity = {"actor", "case_token", "local_peer_id", "pid"}
    flags = {"negotiation_complete", "framing_clean", "selected_rpc_authority", "candidate_bytes_complete", "terminal_outcome", "terminal_state_cause"}
    common = EVENT_FIELDS | QUIC_OWNER_FIELDS | identity | flags | {"operation_receipt_sequence", "prepare_ack_sequence", "negotiation_io_receipt_sequences"}
    for pending in pendings:
        require(set(pending) == common | {"owned_reset_started_order", "owned_reset_returned_order"}
                and pending["source"] == GO_QUIC_SOURCES[pending["kind"]] and pending["protocol"] == ""
                and pending["terminal_outcome"] == "negotiation_stream_reset_abort_pending" and pending["terminal_state_cause"] == "unknown"
                and all(pending[key] is False for key in ("negotiation_complete", "framing_clean", "selected_rpc_authority"))
                and pending["candidate_bytes_complete"] is True
                and all(_same_json(pending[key], raw.get(key)) for key in identity)
                and type(pending["pid"]) is int and pending["pid"] > 0,
                "invalid/foreign exact negotiation stream-abort pending receipt")
        _, stream = _go_quic_stream(events, pending, diagnostic=True)
        key = (pending["native_connection_id"], pending["native_stream_id"])
        require(key not in owners_seen, "duplicate negotiation stream-abort owner")
        owners_seen.add(key)
        original = _quic_ref(events, pending["operation_receipt_sequence"], "native_quic_negotiation_io_return", pending["sequence"])
        _go_quic_diagnostic(raw, events, original, abort_error=True)
        ack = pending["prepare_ack_sequence"]
        require(type(ack) is int and 0 < ack < stream["sequence"] < original["sequence"]
                and stream["prepare_ack_sequence"] == ack == original["prepare_ack_sequence"] == original["terminal_prepare_ack_sequence"]
                and _quic_same_owner(pending, original), "abort borrowed a zero/future/foreign Prepare or owner")
        _quic_context(stream["send_context_at_stream_return"], live=True)
        _quic_context(original["connection_context"], live=True)
        _quic_context(original["connection_context_at_prepare"], live=True)
        outbound = pending["stream_direction"] == "Outbound"
        peer_read = outbound and original["direction"] == "read" and original["remote"] is True
        require((outbound and original["direction"] == "read"
                 and stream["native_call_begin_prepare_ack_sequence"] == ack)
                or (not outbound and pending["stream_direction"] == "Inbound" and original["direction"] == "write"
                    and original["remote"] is True and original["same_send_context_cause"] is True),
                "abort has wrong Read/peer Write direction or native birth")
        if not outbound:
            require(_quic_zero_context(original["send_context"], original["native_stream_id"])["remote"] is True,
                    "peer abort Write borrowed an unrelated send cause")
        calls = [value for value in events if value["kind"] == "native_quic_negotiation_io_return" and _quic_same_owner(value, original)]
        refs = pending["negotiation_io_receipt_sequences"]
        require(type(refs) is list and all(type(ref) is int and 0 < ref < pending["sequence"] for ref in refs)
                and refs == [value["sequence"] for value in calls] and calls and calls[-1] is original,
                "abort omitted/duplicated/later native I/O returns")
        observed = {"read": b"", "write": b""}
        for value in calls:
            _go_quic_diagnostic(raw, events, value, abort_error=value is original)
            require(value["prepare_ack_sequence"] == ack and value["protocol"] == "", "abort prefix has foreign preparation/selection")
            snapshot = value["negotiation_snapshot"]
            side = value["direction"]
            prefix = _quic_captured_bytes(snapshot["successful_prefix"], 16 * 1024 + 512)
            require(side != "write" or value is original or value["requested_bytes"] == len(prefix),
                    "abort has an incomplete native successful Write")
            observed[side] += prefix
            require(sum(map(len, observed.values())) <= 16 * 1024 + 512, "abort wire capture overflow")
            for direction in observed:
                framed = b"".join(bytes.fromhex(events[ref - 1]["receipt"]["framed_hex"]) for ref in snapshot["frame_sequences"]
                                  if events[ref - 1]["direction"] == direction)
                state = snapshot[direction]
                partial = _quic_captured_bytes(state["partial_frame"], 266)
                tail = _quic_captured_bytes(state["lazy_tail"], 16 * 1024 + 10)
                require(observed[direction] == framed + partial + tail, "abort native prefix disagrees with exact parser bytes")
        snapshot = original["negotiation_snapshot"]
        proposer = "write" if outbound else "read"
        require(snapshot["proposal"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"} and snapshot["proposals"] == 1
                and snapshot["reply"] == snapshot["selected_protocol"] == ""
                and all(_quic_captured_bytes(snapshot[side]["partial_frame"], 266) == b"" for side in observed),
                "abort acquired ACK/selection or partial negotiation bytes")
        frame_tokens = {side: [] for side in observed}
        for ref in snapshot["frame_sequences"]:
            side, token = _quic_negotiation_frame(events, ref, original, original["sequence"])
            frame_tokens[side].append(token)
        peer = "read" if outbound else "write"
        require(frame_tokens[proposer] == [HEADER, snapshot["proposal"]]
                and frame_tokens[peer] in ([[], [HEADER]] if outbound else [[HEADER]]), "abort lacks exact canonical header/proposal")
        tail = _quic_captured_bytes(snapshot[proposer]["lazy_tail"], 16 * 1024 + 10)
        if outbound:
            _quic_subscription_candidate(tail, "forge-pr11:" + raw["case_token"])
        else:
            require(tail == b"", "inbound proposal abort borrowed peer RPC bytes")
        require(_quic_captured_bytes(snapshot[peer]["lazy_tail"], 16 * 1024 + 10) == b"", "abort peer direction has application bytes")
        operations = [value for value in events if value["kind"] == "native_stream_operation" and _quic_same_owner(value, original)]
        attempts = [value for value in operations if value["operation"] in {"stream_reset", "stream_reset_with_error"}]
        for field in ("owned_reset_started_order", "owned_reset_returned_order"):
            require(type(pending[field]) is int and 0 <= pending[field] < 2**64, "abort has invalid captured Reset counters")
        owned = None
        if outbound and not peer_read:
            before = [value for value in attempts if type(value.get("started_order")) is int and value["started_order"] < original["returned_order"]]
            require(before, "local abort Read lacks its sealed full Reset")
            owned = max(before, key=lambda value: value["started_order"])
            _go_quic_diagnostic(raw, events, owned)
            require(owned["operation"] == "stream_reset" and owned["prepare_ack_sequence"] == ack
                    and owned["returned_order"] < original["returned_order"]
                    and pending["owned_reset_started_order"] == owned["started_order"]
                    and pending["owned_reset_returned_order"] == owned["returned_order"], "abort Read borrowed a future/superseded/failed Reset")
        else:
            require(pending["owned_reset_started_order"] == pending["owned_reset_returned_order"] == 0, "peer abort claims a local/remote reset cause")
        require(not any(value["kind"] in {"protocol", "rpc", "stream_io_terminal"}
                        and value.get("native_connection_id") == key[0] and _same_json(value.get("native_stream_id"), key[1]) for value in events),
                "abort exported selected authority")
        failures = [value for value in operations if value["outcome"] == "error"]
        close_pending = close = close_reset = None
        linked = [value for value in close_pendings if _quic_same_owner(value, original)
                  and _same_json(value.get("negotiation_abort_returned_order"), original["returned_order"])]
        require(len(failures) <= 1 and len(linked) == len(failures), "abort has missing/duplicate/unrelated opaque Close pending")
        if failures:
            close, close_pending = failures[0], linked[0]
            _go_quic_diagnostic(raw, events, close, abort_close=True, abort_peer_read=peer_read)
            require(set(close_pending) == EVENT_FIELDS | QUIC_OWNER_FIELDS | identity | {
                        "operation_receipt_sequence", "negotiation_abort_returned_order", "prepare_ack_sequence",
                        "terminal_state_cause", "native_close_succeeded", "observed_reset_started_order", "observed_reset_returned_order"}
                    and close_pending["source"] == GO_QUIC_SOURCES[close_pending["kind"]]
                    and _quic_same_owner(close_pending, original) and all(_same_json(close_pending[field], raw[field]) for field in identity)
                    and type(close_pending["operation_receipt_sequence"]) is int and close_pending["operation_receipt_sequence"] == close["sequence"]
                    and type(close_pending["negotiation_abort_returned_order"]) is int
                    and close["sequence"] < close_pending["sequence"]
                    and close_pending["native_close_succeeded"] is False and close_pending["terminal_state_cause"] == "unknown"
                    and all(_same_json(close_pending[field], close[field]) for field in
                            ("prepare_ack_sequence", "observed_reset_started_order", "observed_reset_returned_order"))
                    and close["prepare_ack_sequence"] == ack and original["returned_order"] < close["started_order"]
                    and _same_json(close["connection_context"], original["connection_context"]),
                    "opaque abort Close lacks its exact immutable pending/context")
            before = [value for value in attempts if type(value.get("started_order")) is int and value["started_order"] < close["started_order"]]
            require(before, "opaque abort Close lacks a completed native Reset at BEGIN")
            close_reset = max(before, key=lambda value: value["started_order"])
            _go_quic_diagnostic(raw, events, close_reset)
            require(close_reset["operation"] == "stream_reset" and close_reset["prepare_ack_sequence"] == ack
                    and close_reset["returned_order"] < close["started_order"]
                    and (not peer_read or close_reset["started_order"] > original["returned_order"])
                    and close_reset["started_order"] == close["observed_reset_started_order"]
                    and close_reset["returned_order"] == close["observed_reset_returned_order"],
                    "opaque abort Close borrowed old/future/failed Reset instead of latest at BEGIN")
            closes[close["sequence"]] = peer_read
            close_pending_refs.add(close_pending["sequence"])
            proofs[close_pending["sequence"]] = close_pending
        errors.add(original["sequence"])
        proofs[pending["sequence"]] = pending
        matched = [value for value in finals if value.get("pending_receipt_sequence") == pending["sequence"]]
        if not matched and not terminal:
            require(not any(_quic_same_owner(value, pending) for value in close_finals), "active abort claims completed Close cleanup")
            continue
        require(len(matched) == 1, "missing/duplicate abort finalization")
        final = matched[0]
        final_refs.add(final["sequence"])
        require(set(final) == common | {"pending_receipt_sequence", "framing_receipt_sequence", "native_join_receipt_sequence",
                "owner_disposal_receipt_sequence", "first_owner_disposal_receipt_sequence", "owned_reset_receipt_sequence",
                "close_finalization_receipt_sequence", "accepted"}
                and final["source"] == GO_QUIC_SOURCES[final["kind"]] and _quic_same_owner(final, pending)
                and all(_same_json(final[field], pending[field]) for field in identity | {"operation_receipt_sequence", "prepare_ack_sequence", "negotiation_io_receipt_sequences"})
                and final["terminal_outcome"] == "negotiation_stream_reset_abort" and final["terminal_state_cause"] == "unknown"
                and final["accepted"] is True and final["candidate_bytes_complete"] is True
                and all(final[field] is False for field in ("negotiation_complete", "framing_clean", "selected_rpc_authority")),
                "abort finalizer changed immutable facts or claimed selected authority")
        framing = _quic_ref(events, final["framing_receipt_sequence"], "native_quic_framing_finalized", final["sequence"])
        framing_fields = EVENT_FIELDS | QUIC_OWNER_FIELDS | {"framing_clean", "io_joined", "read_finalized", "write_finalized",
                "negotiation_complete", "pending_read_frame_bytes", "pending_write_frame_bytes", "native_owner_disposed",
                "owner_disposal_receipt_sequence", "negotiation_snapshot", "candidate_bytes_complete", "selected_rpc_authority",
                "negotiation_abort_pending_receipt_sequence", "abort_owner_disposal_receipt_sequence"}
        require(set(framing) == framing_fields and _quic_same_owner(framing, original) and pending["sequence"] < framing["sequence"]
                and type(framing["negotiation_abort_pending_receipt_sequence"]) is int and framing["negotiation_abort_pending_receipt_sequence"] == pending["sequence"]
                and all(framing[field] is True for field in ("io_joined", "read_finalized", "write_finalized", "native_owner_disposed", "candidate_bytes_complete"))
                and all(framing[field] is False for field in ("framing_clean", "negotiation_complete", "selected_rpc_authority"))
                and all(type(framing[field]) is int and framing[field] == 0 for field in ("pending_read_frame_bytes", "pending_write_frame_bytes"))
                and _same_json(framing["negotiation_snapshot"], {key: value for key, value in snapshot.items() if key != "successful_prefix"}),
                "abort framing changed bytes or lacks actual zero-residue joined state")
        _quic_negotiation_snapshot(events, framing, is_io=False)
        disposal = _quic_ref(events, final["owner_disposal_receipt_sequence"], "native_stream_operation", framing["sequence"])
        _go_quic_diagnostic(raw, events, disposal)
        require(attempts and disposal is max(attempts, key=lambda value: value["started_order"])
                and disposal["operation"] == "stream_reset" and disposal["prepare_ack_sequence"] == ack
                and _quic_same_owner(disposal, original) and (owned is not None or disposal["started_order"] > original["returned_order"])
                and type(framing["abort_owner_disposal_receipt_sequence"]) is int and framing["abort_owner_disposal_receipt_sequence"] == disposal["sequence"]
                and _same_json(disposal["connection_context"], original["connection_context"]),
                "abort lacks exact latest successful full Reset disposal after peer error RETURN")
        first = _quic_ref(events, final["first_owner_disposal_receipt_sequence"], "native_stream_operation", framing["sequence"])
        full = [value for value in operations if value["operation"] in {"stream_reset", "stream_close"} and value["outcome"] == "ok" and value["error"] is None]
        require(full and first is full[0] and type(framing["owner_disposal_receipt_sequence"]) is int
                and framing["owner_disposal_receipt_sequence"] == first["sequence"], "abort rewrote first owner disposal")
        owned_ref = final["owned_reset_receipt_sequence"]
        require(type(owned_ref) is int and owned_ref == (owned["sequence"] if owned is not None else 0), "abort finalizer rebound native Read's Reset")
        joined = _quic_ref(events, final["native_join_receipt_sequence"], "native_quic_join", final["sequence"])
        require(framing["sequence"] < joined["sequence"] and type(joined["active_native_calls"]) is int and joined["active_native_calls"] == 0,
                "abort finalized without actual complete native join")
        ref = final["close_finalization_receipt_sequence"]
        require(type(ref) is int and (ref > 0 if failures else ref == 0) and len(failures) <= 1, "abort has missing/extra opaque Close proof")
        if failures:
            close_final = _quic_ref(events, ref, "native_quic_terminal_finalized", final["sequence"])
            close_refs.add(ref)
            require(set(close_final) == EVENT_FIELDS | QUIC_OWNER_FIELDS | identity | {"operation_receipt_sequence", "negotiation_abort_pending_receipt_sequence",
                    "framing_receipt_sequence", "native_join_receipt_sequence", "owner_disposal_receipt_sequence", "owned_terminal_receipt_sequence",
                    "prepare_ack_sequence", "send_context_receipt_sequence", "terminal_outcome", "terminal_state_cause", "native_close_succeeded", "accepted",
                    "pending_receipt_sequence"}
                    and _quic_same_owner(close_final, original) and all(_same_json(close_final[key], raw[key]) for key in identity)
                    and type(close_final["operation_receipt_sequence"]) is int and close_final["operation_receipt_sequence"] == close["sequence"]
                    and close_final["accepted"] is True and close_final["native_close_succeeded"] is False
                    and close_final["terminal_outcome"] == "negotiation_abort_close_pending" and close_final["terminal_state_cause"] == "unknown"
                    and all(_same_json(close_final[key], final[key]) for key in ("framing_receipt_sequence", "native_join_receipt_sequence", "owner_disposal_receipt_sequence", "prepare_ack_sequence"))
                    and type(close_final["negotiation_abort_pending_receipt_sequence"]) is int and close_final["negotiation_abort_pending_receipt_sequence"] == pending["sequence"]
                    and type(close_final["send_context_receipt_sequence"]) is int and close_final["send_context_receipt_sequence"] == close["send_context_receipt_sequence"]
                    and type(close_final["pending_receipt_sequence"]) is int and close_final["pending_receipt_sequence"] == close_pending["sequence"]
                    and close_pending["sequence"] < framing["sequence"]
                    and joined["sequence"] < close_final["sequence"] and original["returned_order"] < close["started_order"]
                    and close["prepare_ack_sequence"] == ack and _same_json(close["connection_context"], original["connection_context"]),
                    "opaque abort Close finalizer lacks its own original/context/disposal/join")
            reset = _quic_ref(events, close_final["owned_terminal_receipt_sequence"], "native_stream_operation", close_final["sequence"])
            require(reset is close_reset, "opaque abort Close finalizer rebound its Reset")
            proofs[close_final["sequence"]] = close_final
        require(not any(value["kind"] in {"protocol", "rpc", "stream_io_terminal"}
                        and value.get("native_connection_id") == key[0] and _same_json(value.get("native_stream_id"), key[1]) for value in events)
                and not any(value["sequence"] > framing["sequence"] for value in calls + operations), "abort exported selected authority or admitted native calls after join")
        for value in (pending, framing, final): proofs[value["sequence"]] = value
    require(final_refs == {value["sequence"] for value in finals} and close_refs == {value["sequence"] for value in close_finals}
            and close_pending_refs == {value["sequence"] for value in close_pendings},
            "orphan/duplicate stream-abort finalizer")
    return errors, closes, proofs, owners_seen


def _quic_late_selected_cleanup(raw, events, *, terminal):
    """Native negotiation/disposal only; no Swarm mapping or handler/reason claim."""
    proofs, owners = {}, set()
    for selected in events:
        if selected["kind"] != "protocol" or selected["source"] != GO_QUIC_SOURCES["protocol"] \
                or selected.get("protocol") not in {"/meshsub/1.0.0", "/meshsub/1.1.0"}:
            continue
        _, stream = _go_quic_stream(events, selected, diagnostic=True)
        ack = stream["prepare_ack_sequence"]
        if not ack:
            continue
        _, _, selected = _go_quic_owner(events, selected, selected["remote_peer_id"], selected["protocol"], "quic", diagnostic=True)
        require(selected["protocol"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"}, "late native selection is not the supported PubSub protocol")
        rows = _quic_prepare_rows(raw, selected, events, ack)
        parent = [row for key, row in rows.items() if key[0] == selected["native_connection_id"]]
        require(parent and (selected["native_connection_id"], selected["native_stream_id"]) not in rows,
                "late native selection lacks its actual existing parent or invents a stream Prepare baseline")
        _quic_context(stream["send_context_at_stream_return"], live=True)
        for row in parent:
            _quic_context(row["connection_context"], live=True)
        key = (selected["native_connection_id"], selected["native_stream_id"])
        require(key not in owners, "duplicate late native-only selected owner")
        owners.add(key)
        owned = [value for value in events if value.get("native_connection_id") == key[0]
                 and _same_json(value.get("native_stream_id"), key[1])]
        require(all(value["kind"] in {"native_quic_stream", "multistream_frame", "protocol", "native_quic_negotiation_io_return",
                                      "native_stream_operation", "native_quic_framing_finalized"} for value in owned),
                "late native-only owner exported RPC/application/terminal authority")
        refs = selected["negotiation_frame_sequences"]
        frames = [value for value in owned if value["kind"] == "multistream_frame"]
        require(len(refs) == 4 and refs == [value["sequence"] for value in frames], "late native selection lacks exactly four canonical frames")
        tokens = {"read": [], "write": []}
        wire = {"read": b"", "write": b""}
        for ref in refs:
            side, token = _quic_negotiation_frame(events, ref, selected, selected["sequence"])
            tokens[side].append(token)
            wire[side] += bytes.fromhex(events[ref - 1]["receipt"]["framed_hex"])
        require(all(values == [HEADER, selected["protocol"]] for values in tokens.values()), "late native proposal/ACK is not canonical")
        observed = {"read": b"", "write": b""}
        calls = [value for value in owned if value["kind"] == "native_quic_negotiation_io_return"]
        orders = set()
        for value in calls:
            _go_quic_diagnostic(raw, events, value)
            require(value["prepare_ack_sequence"] == value["terminal_prepare_ack_sequence"] == ack,
                    "late native I/O has a foreign preparation phase")
            _quic_context(value["connection_context"], live=True)
            snapshot = value["negotiation_snapshot"]
            prefix = _quic_captured_bytes(snapshot["successful_prefix"], 512)
            require(value["direction"] != "write" or value["requested_bytes"] == len(prefix), "late native selection has partial successful Write")
            observed[value["direction"]] += prefix
            for side in observed:
                require(_quic_captured_bytes(snapshot[side]["partial_frame"], 266) == b""
                        or snapshot["selected_protocol"] == "", "selected native owner retains partial negotiation")
                require(_quic_captured_bytes(snapshot[side]["lazy_tail"], 16 * 1024 + 10) == b"", "late native-only owner contains application bytes")
                consumed = b"".join(bytes.fromhex(events[ref - 1]["receipt"]["framed_hex"])
                                    for ref in snapshot["frame_sequences"] if events[ref - 1]["direction"] == side)
                partial = _quic_captured_bytes(snapshot[side]["partial_frame"], 266)
                require(observed[side] == consumed + partial and wire[side].startswith(observed[side]),
                        "late native prefix disagrees with its exact canonical frames")
            require(value["started_order"] not in orders and value["returned_order"] not in orders, "ambiguous late native I/O counters")
            orders.update((value["started_order"], value["returned_order"]))
        operations = [value for value in owned if value["kind"] == "native_stream_operation"]
        for value in operations:
            require(set(value) == EVENT_FIELDS | QUIC_OWNER_FIELDS | {"operation", "started_order", "returned_order", "prepare_ack_sequence",
                    "send_context", "error", "error_type", "outcome", "typed_cause", "requested_reset_code"}
                    and value["source"] == GO_QUIC_SOURCES["native_stream_operation"] and _quic_same_owner(value, selected)
                    and value["operation"] == "stream_reset" and value["requested_reset_code"] is None
                    and value["outcome"] == "ok" and value["error"] is value["error_type"] is None and value["typed_cause"] == "none"
                    and type(value["prepare_ack_sequence"]) is int and value["prepare_ack_sequence"] == ack
                    and type(value["started_order"]) is int and type(value["returned_order"]) is int
                    and 0 < value["started_order"] < value["returned_order"] < 2**64
                    and value["started_order"] not in orders and value["returned_order"] not in orders,
                    "late native-only disposal is failed/foreign/half/explicit/ambiguous")
            context = _quic_context(value["send_context"])
            if context["done"]:
                _quic_zero_context(context, key[1])
            orders.update((value["started_order"], value["returned_order"]))
        framings = [value for value in owned if value["kind"] == "native_quic_framing_finalized"]
        require(len(framings) <= 1, "duplicate late native-only framing")
        if terminal or framings:
            require(observed == wire and calls and operations and len(framings) == 1,
                    "late native-only owner lacks complete observed handshake/full Reset/framing")
            framing = framings[0]
            extra = {"framing_clean", "io_joined", "read_finalized", "write_finalized", "negotiation_complete",
                     "pending_read_frame_bytes", "pending_write_frame_bytes", "native_owner_disposed", "owner_disposal_receipt_sequence"}
            disposal = _quic_ref(events, framing["owner_disposal_receipt_sequence"], "native_stream_operation", framing["sequence"])
            require(set(framing) == EVENT_FIELDS | QUIC_OWNER_FIELDS | extra and _quic_same_owner(framing, selected)
                    and framing["source"] == GO_QUIC_SOURCES["native_quic_framing_finalized"]
                    and all(framing[field] is True for field in extra - {"pending_read_frame_bytes", "pending_write_frame_bytes", "owner_disposal_receipt_sequence"})
                    and all(type(framing[field]) is int and framing[field] == 0 for field in ("pending_read_frame_bytes", "pending_write_frame_bytes"))
                    and disposal is min(operations, key=lambda value: value["sequence"])
                    and all(max(value["returned_order"] for value in calls) < op["started_order"] for op in operations)
                    and all(value["sequence"] < framing["sequence"] for value in owned if value is not framing),
                    "late native-only framing lacks exact successful full disposal and zero-residue joined state")
            proofs[framing["sequence"]] = framing
        if terminal:
            joined = _quic_native_join([value for value in events if value["source"] in GO_QUIC_SOURCES.values()
                                      or value["source"] in {"go.quic.native_stream.read", "go.quic.native_stream.write"}])
            require(framings[0]["sequence"] < joined["sequence"] and raw.get("finalized") is True and raw.get("joined") is True,
                    "late native-only selection lacks actual terminal native join")
            _terminal_owners(raw)
        # Active snapshots expose only checked native facts, not completed cleanup.
        proofs[selected["sequence"]] = selected
        proofs.update({value["sequence"]: value for value in operations})
    return proofs, owners


def _go_quic_operations(raw, events, *, terminal):
    """Independently bind lower native outcomes, context and actual disposal/join."""
    lower = [event for event in events if event.get("source") in GO_QUIC_SOURCES.values()
             or event.get("source") in {"go.quic.native_stream.read", "go.quic.native_stream.write"}]
    if not lower:
        return {}
    require(raw.get("error") is None and raw.get("overflow") is False, "native cleanup cannot clear a prior sticky failure")
    require(len([event for event in lower if event.get("kind") == "native_quic_connection"]) <= 16
            and len([event for event in lower if event.get("kind") == "native_quic_stream"]) <= 64,
            "native QUIC physical owner capture exceeds fixture bounds")
    abort_errors, abort_closes, abort_proofs, abort_owners = _quic_stream_aborts(raw, events, terminal=terminal)
    native_only_proofs, native_only_owners = _quic_late_selected_cleanup(raw, events, terminal=terminal)
    base = EVENT_FIELDS | QUIC_OWNER_FIELDS
    operations, pending, frames, finals, contexts, orders = {}, {}, {}, {}, {}, {}
    diagnostics, diagnostic_contexts, diagnostic_touched, captured_bytes = {}, {}, set(), 0
    cleanup_errors, cleanups, cleanup_frames, cleanup_finals = {}, {}, {}, {}
    empty_errors = []
    for event in lower:
        if event.get("kind") == "native_quic_negotiation_cleanup_pending":
            _quic_cleanup_shape(events, event)
            ref = event["operation_receipt_sequence"]
            _quic_ref(events, ref, "native_quic_negotiation_io_return", event["sequence"])
            require(ref not in cleanup_errors, "duplicate cleanup pending native error reference")
            cleanup_errors[ref] = event
    common = {"operation", "started_order", "returned_order", "prepare_ack_sequence", "send_context",
              "error", "error_type", "outcome", "typed_cause"}
    for event in lower:
        kind = event["kind"]
        require(kind in GO_QUIC_SOURCES and event["source"] == GO_QUIC_SOURCES[kind]
                or kind == "rpc" and event["source"] in {"go.quic.native_stream.read", "go.quic.native_stream.write"},
                "lower native event has wrong kind/source authority")
        if kind == "native_quic_connection":
            _go_quic_connection(events, event)
            require(event["local_peer_id"] == raw["local_peer_id"], "lower connection has foreign local actor identity")
            continue
        if kind == "native_quic_join":
            continue
        if event["sequence"] in abort_proofs:
            require(abort_proofs[event["sequence"]] is event, "foreign abort proof dispatch")
            continue
        if event["sequence"] in native_only_proofs:
            require(native_only_proofs[event["sequence"]] is event, "foreign native-only cleanup dispatch")
            continue
        require(kind not in {"native_quic_negotiation_abort_pending", "native_quic_negotiation_abort_close_pending",
                             "native_quic_negotiation_abort_finalized"}, "unvalidated negotiation abort receipt")
        if kind == "native_quic_negotiation_cleanup_pending":
            original, write = _quic_cleanup_pending(raw, events, event)
            key = (event["native_connection_id"], event["native_stream_id"])
            require(key not in cleanups, "duplicate cleanup pending owner")
            cleanups[key] = event
            continue
        if kind == "native_quic_negotiation_cleanup_finalized":
            key = _quic_cleanup_shape(events, event, finalized=True)
            preceding = _quic_ref(events, event["pending_receipt_sequence"], "native_quic_negotiation_cleanup_pending", event["sequence"])
            require(key not in cleanup_finals and cleanups.get(key) is preceding
                    and all(_same_json(event[field], preceding[field]) for field in set(preceding) - EVENT_FIELDS - {"terminal_outcome"}),
                    "orphan/duplicate cleanup finalizer rewrote/borrowed its pending receipt")
            framing = _quic_ref(events, event["framing_receipt_sequence"], "native_quic_framing_finalized", event["sequence"])
            joined = _quic_ref(events, event["native_join_receipt_sequence"], "native_quic_join", event["sequence"])
            require(cleanup_frames.get(key) is framing
                    and framing["cleanup_owner_disposal_receipt_sequence"] == event["owner_disposal_receipt_sequence"]
                    and framing["owner_disposal_receipt_sequence"] == event["first_owner_disposal_receipt_sequence"]
                    and framing["sequence"] < joined["sequence"] < event["sequence"],
                    "cleanup finalization lacks its actual framing/disposal/native join chain")
            cleanup_finals[key] = event
            continue
        if kind == "native_quic_stream":
            _go_quic_stream(events, event, diagnostic=True)
            if event["prepare_ack_sequence"]:
                rows = _quic_prepare_rows(raw, event, events, event["prepare_ack_sequence"])
                require(any(key[0] == event["native_connection_id"] for key in rows), "late native stream lacks actual parent Prepare baseline")
            continue
        if kind == "native_quic_connection_context":
            fields = EVENT_FIELDS | {"native_connection_id", "local_peer_id", "remote_peer_id", "local_address",
                     "remote_address", "remote_public_key_sha256", "native_connection_basis", "connection_receipt_sequence",
                     "context", "prepare_ack_sequence", "context_at_prepare", "prepare_baseline_present"}
            require(set(event) == fields, "invalid exact native QUIC connection context receipt")
            connection = _quic_ref(events, event["connection_receipt_sequence"], "native_quic_connection", event["sequence"])
            require(all(_same_json(event[key], connection[key])
                        for key in fields & set(connection) - EVENT_FIELDS - {"prepare_ack_sequence"})
                    and event["native_connection_id"] not in contexts and event["prepare_baseline_present"] is True,
                    "foreign/duplicate native connection terminal context")
            ack = _quic_ref(events, event["prepare_ack_sequence"], "shutdown_prepared", event["sequence"])
            _quic_context(event["context_at_prepare"], live=True)
            _quic_zero_context(event["context"])
            rows = ack.get("native_quic_context_snapshots")
            require(isinstance(rows, list) and len(rows) <= 64
                    and all(isinstance(row, dict) and set(row) == {"native_connection_id", "native_stream_id",
                            "send_context", "connection_context"} for row in rows)
                    and any(row.get("native_connection_id") == event["native_connection_id"] for row in rows)
                    and all(_same_json(row["connection_context"], event["context_at_prepare"])
                            for row in rows if row.get("native_connection_id") == event["native_connection_id"]),
                    "connection terminal borrowed a foreign Prepare baseline")
            contexts[event["native_connection_id"]] = event
            continue
        if kind == "multistream_frame":
            _go_quic_stream(events, event, diagnostic=True)
            _quic_negotiation_frame(events, event["sequence"], event, event["sequence"] + 1)
            if event["protocol"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"}:
                diagnostic_touched.add((event["native_connection_id"], event["native_stream_id"]))
            continue
        if kind == "native_quic_framing_finalized" and "negotiation_cleanup_pending_receipt_sequence" in event:
            _go_quic_stream(events, event, diagnostic=True)
            key = (event.get("native_connection_id"), event.get("native_stream_id"))
            require(key in cleanups and key not in cleanup_frames, "orphan/duplicate cleanup framing")
            captured_bytes += _quic_cleanup_framing(raw, events, event, cleanups[key])
            require(captured_bytes <= 4 * 1024 * 1024, "aggregate cleanup diagnostic wire capture overflow")
            cleanup_frames[key] = event
            continue
        if _quic_diagnostic_phase(event):
            empty_error = kind == "native_quic_negotiation_io_return" and event.get("outcome") == "error" \
                and event["sequence"] not in cleanup_errors and event["sequence"] not in abort_errors
            captured, touched = _go_quic_diagnostic(raw, events, event,
                                                   cleanup_error=event["sequence"] in cleanup_errors, empty_error=empty_error,
                                                   abort_error=event["sequence"] in abort_errors,
                                                   abort_close=event["sequence"] in abort_closes,
                                                   abort_peer_read=abort_closes.get(event["sequence"], False))
            captured_bytes += captured
            require(captured_bytes <= 4 * 1024 * 1024, "aggregate native diagnostic wire capture overflow")
            key = (event["native_connection_id"], event["native_stream_id"])
            if kind == "native_quic_send_context":
                diagnostic_contexts[event["sequence"]] = event
            else:
                seen = orders.setdefault(key, set())
                require(event["started_order"] not in seen and event["returned_order"] not in seen,
                        "ambiguous diagnostic/native operation counters")
                seen.update((event["started_order"], event["returned_order"]))
                diagnostics[event["sequence"]] = event
                if empty_error:
                    empty_errors.append(event)
            if touched:
                diagnostic_touched.add(key)
            continue
        require(kind != "native_quic_negotiation_io_return", "diagnostic I/O lacks exact unselected native-return phase")
        _go_quic_owner(events, event, event.get("remote_peer_id"), event.get("protocol"), "quic",
                       diagnostic=kind == "protocol" and event.get("protocol") not in {"/meshsub/1.0.0", "/meshsub/1.1.0"})
        if kind == "rpc":
            captured_bytes += len(event["receipt"]["framed_hex"]) // 2
            require(captured_bytes <= 4 * 1024 * 1024, "aggregate native diagnostic/RPC wire capture overflow")
            continue
        if kind == "protocol":
            continue
        key = (event["native_connection_id"], event["native_stream_id"])
        if kind == "native_quic_framing_finalized":
            extra = {"framing_clean", "io_joined", "read_finalized", "write_finalized", "negotiation_complete",
                     "pending_read_frame_bytes", "pending_write_frame_bytes", "native_owner_disposed", "owner_disposal_receipt_sequence"}
            require(set(event) == base | extra and key not in frames
                    and all(event[value] is True for value in extra - {"pending_read_frame_bytes", "pending_write_frame_bytes",
                                                                    "owner_disposal_receipt_sequence"})
                    and all(type(event[value]) is int and event[value] == 0 for value in
                            ("pending_read_frame_bytes", "pending_write_frame_bytes")),
                    "unjoined/duplicate/incomplete native lower QUIC framing")
            frames[key] = event
            continue
        if kind == "native_quic_terminal_finalized":
            extra = {"operation_receipt_sequence", "framing_receipt_sequence", "owner_disposal_receipt_sequence", "accepted",
                     "terminal_outcome", "prepare_ack_sequence", "connection_context_receipt_sequence", "send_context_receipt_sequence"}
            if event.get("terminal_outcome") == "connection_application_close_pending":
                extra.add("same_native_connection_context_cause")
            if event.get("terminal_outcome") in {"owned_reset_pending", "owned_read_terminal_pending"} \
                    or event.get("terminal_outcome") == "native_send_reset_close_pending" and "owned_terminal_receipt_sequence" in event:
                extra.add("owned_terminal_receipt_sequence")
            reference = event.get("operation_receipt_sequence")
            require(set(event) == base | extra and type(reference) is int
                    and reference not in finals and event["accepted"] is True
                    and all(type(event[field]) is int and 0 <= event[field] < event["sequence"] for field in extra
                            if field.endswith("receipt_sequence") or field == "prepare_ack_sequence"),
                    "duplicate/malformed/unaccepted lower native terminal finalizer")
            finals[reference] = event
            continue
        if kind == "native_quic_send_context":
            require(set(event) == base | {"started_order", "returned_order", "prepare_ack_sequence", "send_context", "observation_basis"}
                    and event["observation_basis"] == "same_lower_delegate_Context_at_actual_Close_RETURN",
                    "invalid Close RETURN native send context receipt")
            _quic_context(event["send_context"])
            continue
        is_io = kind == "stream_io_terminal"
        require(kind in {"stream_io_terminal", "native_stream_operation"}, "unknown native lower QUIC operation")
        extra = {"direction", "successful_prefix_bytes", "pending_frame_bytes", "terminal_prepare_ack_sequence",
                 "prepare_baseline_present", "send_context_at_prepare", "connection_context_at_prepare"} if is_io else {"requested_reset_code"}
        outcome = event.get("outcome")
        if outcome in {"owned_reset_pending", "owned_read_terminal_pending", "peer_zero_reset_pending", "connection_application_close_pending"}:
            extra |= {"error_code", "remote", "transport_error_type", "transport_error_code", "transport_error_remote"}
            if outcome != "connection_application_close_pending":
                extra |= {"transport_native_stream_id", "same_send_context_cause"}
            if outcome == "peer_zero_reset_pending":
                extra.add("peer_reset_reason")
        if event.get("operation") == "stream_close":
            extra |= {"native_close_error_classification", "send_context_receipt_sequence"}
        if outcome == "native_send_reset_close_pending":
            extra |= {"terminal_state_cause", "send_context_at_prepare"}
        require(set(event) == base | common | extra, "native lower QUIC operation fields differ from exact contract")
        start, returned = event["started_order"], event["returned_order"]
        require(type(start) is int and type(returned) is int and 0 < start < returned < 2**64
                and type(event["prepare_ack_sequence"]) is int and 0 <= event["prepare_ack_sequence"] < event["sequence"],
                "invalid native lower operation phase/order")
        seen = orders.setdefault(key, set())
        require(start not in seen and returned not in seen, "ambiguous lower native operation order")
        seen.update((start, returned))
        _quic_context(event["send_context"])
        if event["prepare_ack_sequence"]:
            _quic_ref(events, event["prepare_ack_sequence"], "shutdown_prepared", event["sequence"])
        if is_io:
            direction = event["direction"]
            require(direction in {"read", "write"} and event["operation"] == "stream_" + direction
                    and type(event["successful_prefix_bytes"]) is int and 0 <= event["successful_prefix_bytes"] <= 16 * 1024 * 1024
                    and type(event["pending_frame_bytes"]) is int and event["pending_frame_bytes"] == 0,
                    "invalid native lower I/O direction/successful prefix/RPC residue")
        else:
            require(event["operation"] in {"stream_close", "stream_close_read", "stream_close_write", "stream_reset", "stream_reset_with_error"}
                    and (type(event["requested_reset_code"]) is int and event["requested_reset_code"] == 0
                         if event["operation"] == "stream_reset_with_error" else event["requested_reset_code"] is None),
                    "invalid native terminal operation/nonzero reset")
        if outcome == "ok":
            require(not is_io and event["error"] is None and event["error_type"] is None and event["typed_cause"] == "none",
                    "successful native lower operation has error residue")
        else:
            require(isinstance(event["error"], str) and 0 < len(event["error"]) <= 512,
                    "missing bounded original native error")
            if outcome == "eof":
                require(is_io and event["direction"] == "read" and event["typed_cause"] == "io_eof"
                        and event["error_type"] == "*errors.errorString", "invalid native lower EOF")
            elif outcome == "native_send_reset_close_pending":
                require(not is_io and event["operation"] == "stream_close" and event["typed_cause"] == "opaque"
                        and event["error_type"] == "*errors.errorString"
                        and event["native_close_error_classification"] == "opaque_unwrapped_native_error"
                        and event["terminal_state_cause"] == "observed_native_send_context_not_graceful_close",
                        "public/sentinel/wrapped Close is not opaque native normalization")
                _quic_zero_context(event["send_context"], event["native_stream_id"])
            else:
                require(is_io and outcome in {"owned_reset_pending", "owned_read_terminal_pending", "peer_zero_reset_pending",
                                               "connection_application_close_pending"}
                        and type(event["error_code"]) is int and event["error_code"] == 0
                        and type(event["transport_error_code"]) is int and event["transport_error_code"] == 0
                        and type(event["remote"]) is bool and event["transport_error_remote"] is event["remote"],
                        "nonzero/opaque/mismatched native lower I/O error")
                connection_error = outcome == "connection_application_close_pending"
                require(event["error_type"] == ("*network.ConnError" if connection_error else "*network.StreamError")
                        and event["transport_error_type"] == ("*qerr.ApplicationError" if connection_error else "*quic.StreamError")
                        and event["typed_cause"] == ("libp2p_quic_application_error" if connection_error else "libp2p_quic_stream_error"),
                        "native lower I/O has wrapped/foreign typed cause")
                if not connection_error:
                    require(type(event["transport_native_stream_id"]) is int
                            and event["transport_native_stream_id"] == event["native_stream_id"]
                            and type(event["same_send_context_cause"]) is bool,
                            "native lower I/O borrowed another stream's cause")
            if outcome != "eof":
                ack_ref = event["terminal_prepare_ack_sequence"] if is_io else event["prepare_ack_sequence"]
                row = _quic_baseline(raw, event, events, ack_ref, live=outcome in {"peer_zero_reset_pending", "connection_application_close_pending"})
                if is_io:
                    require(event["prepare_baseline_present"] is True
                            and _same_json(event["send_context_at_prepare"], row["send_context"])
                            and _same_json(event["connection_context_at_prepare"], row["connection_context"]),
                            "native I/O changed its actual Prepare baseline")
                else:
                    require(_same_json(event["send_context_at_prepare"], row["send_context"]), "Close changed its Prepare snapshot")
                pending[event["sequence"]] = event
        if event["operation"] == "stream_close":
            context = _quic_ref(events, event["send_context_receipt_sequence"], "native_quic_send_context", event["sequence"])
            require(_quic_same_owner(context, event) and all(_same_json(context[field], event[field]) for field in
                    ("started_order", "returned_order", "prepare_ack_sequence", "send_context")),
                    "Close normalization did not use its exact current native send context")
            if outcome == "ok":
                require(event["native_close_error_classification"] == "none", "nil native Close has a fabricated cause")
        operations[event["sequence"]] = event
    joined = _quic_native_join(lower) if terminal or any(event["kind"] == "native_quic_join" for event in lower) else None
    if empty_errors:
        require(raw.get("finalized") is True and raw.get("joined") is True and joined is not None,
                "empty diagnostic errors lack actual terminal native join")
        _terminal_owners(raw)
    for original in empty_errors:
        parent = contexts.get(original["native_connection_id"])
        require(parent is not None and parent["sequence"] < original["sequence"]
                and parent["prepare_ack_sequence"] == original["prepare_ack_sequence"]
                and _same_json(parent["context"], original["connection_context"])
                and _same_json(parent["context_at_prepare"], original["connection_context_at_prepare"]),
                "empty diagnostic error lacks its indexed sealed same-parent terminal context")
        owned = [value for value in lower if value.get("native_connection_id") == original["native_connection_id"]
                 and _same_json(value.get("native_stream_id"), original["native_stream_id"])]
        require(all(value["kind"] in {"native_quic_stream", "native_quic_negotiation_io_return",
                                      "native_stream_operation", "native_quic_send_context"}
                    and value["protocol"] == "" for value in owned),
                "empty diagnostic owner has negotiation/selection/RPC/framing authority")
        calls = [value for value in owned if value["sequence"] in diagnostics]
        require(all(value["negotiation_snapshot"]["frame_sequences"] == []
                    and all(value["negotiation_snapshot"][side][field]["bytes"] == 0
                            for side in ("read", "write") for field in ("partial_frame", "lazy_tail"))
                    and (value["kind"] != "native_quic_negotiation_io_return" or value["successful_prefix_bytes"] == 0)
                    for value in calls), "empty diagnostic owner has any native negotiation bytes")
        require(all(value["sequence"] < joined["sequence"] for value in owned)
                and any(value["kind"] == "native_stream_operation" and value["operation"] in {"stream_reset", "stream_close"}
                        and value["outcome"] == "ok" and value["error"] is None
                        and value["prepare_ack_sequence"] == original["prepare_ack_sequence"]
                        and original["returned_order"] < value["started_order"] < value["returned_order"]
                        and _same_json(value["connection_context"], original["connection_context"]) for value in calls),
                "empty diagnostic error lacks actual successful full disposal after its native RETURN")
    require(not cleanup_finals or joined is not None and all(value["native_join_receipt_sequence"] == joined["sequence"]
                                                            for value in cleanup_finals.values()), "cleanup finalized without exact native join")
    for key, framing in cleanup_frames.items():
        require(not any(value["native_connection_id"] == key[0] and _same_json(value["native_stream_id"], key[1])
                        and value["sequence"] > framing["sequence"] for value in diagnostics.values()),
                "native I/O/operation occurred after cleanup framing joined")
    if not terminal:
        return native_only_proofs
    require(set(cleanups) == set(cleanup_frames) == set(cleanup_finals), "missing/orphan cleanup framing/join/finalization")
    require(set(diagnostic_contexts) == {event["send_context_receipt_sequence"] for event in diagnostics.values()
                                       if event["operation"] == "stream_close"}, "missing/duplicated/orphan diagnostic Close context")
    require(set(finals) == set(pending), "missing/duplicate/foreign lower QUIC terminal finalization")
    for key, framing in frames.items():
        disposal = _quic_ref(events, framing["owner_disposal_receipt_sequence"], "native_stream_operation", framing["sequence"])
        require(not _quic_diagnostic_phase(disposal) and _quic_same_owner(disposal, framing)
                and disposal.get("operation") in {"stream_close", "stream_reset", "stream_reset_with_error"}
                and disposal.get("outcome") == "ok" and disposal.get("error") is None,
                "lower framing lacks actual successful full owner disposal")
        require(not any(_quic_same_owner(value, framing) and value["sequence"] > framing["sequence"]
                        for value in list(operations.values()) + list(diagnostics.values())), "native operation occurred after lower framing joined")
    repeat_close_links, validated_repeat_closes = [], {}
    for reference, original in pending.items():
        final = finals[reference]
        framing = _quic_ref(events, final["framing_receipt_sequence"], "native_quic_framing_finalized", final["sequence"])
        require(reference < framing["sequence"] and _quic_same_owner(final, original) and _quic_same_owner(framing, original)
                and final["terminal_outcome"] == original["outcome"]
                and final["owner_disposal_receipt_sequence"] == framing["owner_disposal_receipt_sequence"],
                "lower terminal finalizer rewrote/borrowed an original receipt")
        ack_ref = original.get("terminal_prepare_ack_sequence", original["prepare_ack_sequence"])
        require(type(final["prepare_ack_sequence"]) is int and final["prepare_ack_sequence"] == ack_ref,
                "lower finalizer changed native return preparation")
        disposal = _quic_ref(events, final["owner_disposal_receipt_sequence"], "native_stream_operation", framing["sequence"])
        require(disposal["prepare_ack_sequence"] == ack_ref, "terminal disposal did not begin in actual Prepared phase")
        outcome = original["outcome"]
        owned_ref = final.get("owned_terminal_receipt_sequence")
        if outcome in {"owned_reset_pending", "owned_read_terminal_pending"} or owned_ref is not None:
            owned = _quic_ref(events, owned_ref, "native_stream_operation", final["sequence"])
            require(not _quic_diagnostic_phase(owned) and _quic_same_owner(owned, original) and owned["outcome"] == "ok" and owned["error"] is None
                    and owned["prepare_ack_sequence"] == ack_ref and owned["started_order"] < original["returned_order"]
                    and owned["operation"] in ({"stream_reset"} if outcome == "owned_reset_pending" else
                                              {"stream_close", "stream_close_read", "stream_reset"}),
                    "lower terminal has no exact successful owned native operation")
            if outcome == "native_send_reset_close_pending":
                require(owned["operation"] == "stream_reset" and owned["returned_order"] < original["started_order"],
                        "RepeatClose borrowed a later/not-returned native Reset")
                boundary = original["started_order"]
                names = {"stream_reset", "stream_reset_with_error"}
            else:
                boundary = original["returned_order"]
                names = {"stream_close", "stream_close_read", "stream_reset", "stream_reset_with_error"}
            superseding = [value for value in operations.values() if _quic_same_owner(value, original)
                           and value["operation"] in names and owned["started_order"] < value["started_order"] < boundary]
            if superseding:
                require(outcome == "owned_read_terminal_pending" and owned["operation"] == "stream_reset"
                        and len(superseding) == 1 and superseding[0]["operation"] == "stream_close"
                        and superseding[0]["outcome"] == "native_send_reset_close_pending"
                        and superseding[0]["prepare_ack_sequence"] == ack_ref
                        and owned["returned_order"] < original["returned_order"]
                        and owned["returned_order"] < superseding[0]["started_order"]
                        < superseding[0]["returned_order"],
                        "owned lower terminal attempt was superseded before native return")
                # Resolve only after the intervening Close's entire independent finalization passed.
                repeat_close_links.append((owned["sequence"], superseding[0]["sequence"]))
        if outcome in {"owned_reset_pending", "peer_zero_reset_pending"} and original["direction"] == "write":
            require(original["same_send_context_cause"] is True, "send context cannot be borrowed for another Write cause")
            context = _quic_zero_context(original["send_context"], original["native_stream_id"])
            require(context["remote"] is original["remote"], "native Write has mismatched send direction cause")
        if outcome == "peer_zero_reset_pending":
            require(original["remote"] is True and original["peer_reset_reason"] == "unknown" and owned_ref is None,
                    "peer zero reset invented a local/remote cleanup cause")
        if outcome in {"owned_reset_pending", "owned_read_terminal_pending"}:
            require(original["remote"] is False and (outcome != "owned_read_terminal_pending" or original["direction"] == "read"),
                    "local terminal operation reclassified a peer reset or Write")
        if outcome == "native_send_reset_close_pending":
            require(final["send_context_receipt_sequence"] == original["send_context_receipt_sequence"],
                    "Close finalizer inherited another operation's send context")
            if owned_ref is None:
                require(original["send_context"]["remote"] is True, "local Close lacks own prior full Reset")
                _quic_baseline(raw, original, events, ack_ref, live=True)
        else:
            require(type(final["send_context_receipt_sequence"]) is int and final["send_context_receipt_sequence"] == 0,
                    "I/O finalizer borrowed a Close send context")
        connection_ref = final["connection_context_receipt_sequence"]
        if outcome == "connection_application_close_pending":
            connection = _quic_ref(events, connection_ref, "native_quic_connection_context", framing["sequence"])
            require(connection["native_connection_id"] == original["native_connection_id"]
                    and connection["remote_peer_id"] == original["remote_peer_id"]
                    and final.get("same_native_connection_context_cause") is True
                    and connection["context"]["remote"] is original["remote"] and owned_ref is None,
                    "connection I/O lacks its same native typed terminal context")
        else:
            require(type(connection_ref) is int and 0 <= connection_ref < final["sequence"], "invalid lower connection context reference")
            if connection_ref:
                connection = _quic_ref(events, connection_ref, "native_quic_connection_context", framing["sequence"])
                require(connection["native_connection_id"] == original["native_connection_id"], "foreign connection context")
        if outcome == "native_send_reset_close_pending" and owned_ref is not None:
            validated_repeat_closes[reference] = final
    for reset_ref, close_ref in repeat_close_links:
        require(close_ref in validated_repeat_closes
                and validated_repeat_closes[close_ref]["owned_terminal_receipt_sequence"] == reset_ref,
                "Read terminal lacks independently finalized RepeatClose on its exact owned Reset")
    selected_keys = {(event["native_connection_id"], event["native_stream_id"]) for event in lower
                     if event["kind"] == "protocol" and event["protocol"] in {"/meshsub/1.0.0", "/meshsub/1.1.0"}}
    require(set(frames) | native_only_owners == selected_keys, "missing actual lower QUIC framing/disposal join")
    require(diagnostic_touched <= selected_keys | set(cleanup_finals) | abort_owners,
            "unselected PubSub proposal/tail lacks actual ACK and joined framing")
    # Export only exact framing receipts after their entire cleanup chain passed.
    return {**{event["sequence"]: event for event in cleanup_frames.values()}, **abort_proofs, **native_only_proofs}


def _stream_owner(events, event, peer, protocol, transport, expected_fingerprint, *, peer_field="peer_id"):
    """Apply the same carrier/protocol authority to RPC and native terminal receipts."""
    if event.get("source", "").startswith("go.quic."):
        return _go_quic_owner(events, event, peer, protocol, transport, expected_fingerprint)
    require(event.get(peer_field) == peer and event.get("protocol") == protocol
            and _id(event.get("connection_id")) and _id(event.get("stream_id")), "native receipt lacks actual stream owner")
    implementation = event["source"].split(".")[0]
    owner = _connection_owner(events, event["connection_id"], peer, implementation, transport, expected_fingerprint)
    selected = [value for value in events if value.get("kind") == "protocol"
                and all(value.get(key) == event[key] for key in ("connection_id", "stream_id"))]
    require(len(selected) == 1 and selected[0].get("source") in SOURCES[implementation]["protocol"]
            and selected[0].get("peer_id") == peer and selected[0].get("protocol") == protocol
            and owner["sequence"] <= selected[0]["sequence"] <= event["sequence"],
            "native receipt lacks preceding unambiguous authentication/selected protocol")


def _owner(events, event, peer, protocol, transport, expected_fingerprint):
    return _stream_owner(events, event, peer, protocol, transport, expected_fingerprint)


def _rpcs(events, peer, protocol, transport, direction, expected_fingerprint=None):
    result = []
    for event in events:
        if event.get("kind") == "rpc" and _rpc_peer(event) == peer and event.get("direction") == direction:
            _owner(events, event, peer, protocol, transport, expected_fingerprint)
            result.append((event, validate_rpc_receipt(event.get("receipt"), protocol, direction)))
    return result


def _validation(events, payload, source, author, topic, outcome, protocol, transport, fingerprint):
    digest = hashlib.sha256(payload.encode()).hexdigest()
    native = events[0]["source"].split(".")[0]
    kind = ("delivery" if outcome == "accept" else "rejection") if native == "go" else "validation"
    fields = {"payload_sha256": digest, "propagation_peer": source, "outcome": outcome}
    if native == "go":
        fields.update(committed=True, phase="post_decision")
    event = _single(events, kind, **fields)
    require(event.get("author_peer") == author and event.get("topic") == topic and _id(event.get("message_id")),
            "validation has wrong author/topic/message ID")
    require(isinstance(event.get("seqno_hex"), str) and re.fullmatch(r"[a-f0-9]{16}", event["seqno_hex"]),
            "validation lacks exact native author sequence")
    require(event["message_id"] == (_peer(author) + bytes.fromhex(event["seqno_hex"])).hex(),
            "message ID differs from configured native author/sequence policy")
    matching = [(rpc, message) for rpc, decoded in _rpcs(events, source, protocol, transport, "read", fingerprint)
                for message in decoded["messages"] if message["payload_sha256"] == digest]
    require(any(message["author_hex"] == _peer(author).hex() and message["topic"] == topic
                and message["seqno_hex"] == event["seqno_hex"] and bool(message["signature_hex"])
                and rpc["sequence"] < event["sequence"] for rpc, message in matching),
            "validation lacks preceding exact native signed input")
    return event


def _control_pair(actors, events, source, target, kind, topic, protocol, transport, after, fingerprint):
    source_peer, target_peer = actors[source]["local_peer_id"], actors[target]["local_peer_id"]
    sent = [(event, value) for event, decoded in _rpcs(events[source], target_peer, protocol, transport, "write", fingerprint)
            for value in decoded[kind] if value["topic"] == topic and event["sequence"] > after]
    received = [(event, value) for event, decoded in _rpcs(events[target], source_peer, protocol, transport, "read", fingerprint)
                for value in decoded[kind] if value["topic"] == topic
                and event["sequence"] > _snapshot(events[target], "before")["sequence"]]
    matched = [(left, right) for left, _ in sent for right, _ in received
               if left["receipt"]["framed_hex"] == right["receipt"]["framed_hex"]]
    require(bool(matched), f"native {kind} lacks matching real send/receive receipts")
    return matched[0]


def _message_pair(actors, events, source, target, validation, protocol, transport, fingerprint, after=0, read_after=0):
    def matches(message):
        return (message["payload_sha256"] == validation["payload_sha256"]
                and message["seqno_hex"] == validation["seqno_hex"]
                and message["topic"] == validation["topic"]
                and message["author_hex"] == _peer(validation["author_peer"]).hex()
                and bool(message["signature_hex"]))

    sent = [event for event, decoded in _rpcs(events[source], actors[target]["local_peer_id"], protocol, transport, "write", fingerprint)
            if event["sequence"] > after
            if any(matches(message) for message in decoded["messages"])]
    received = [event for event, decoded in _rpcs(events[target], actors[source]["local_peer_id"], protocol, transport, "read", fingerprint)
            if read_after < event["sequence"] < validation["sequence"]
            and any(matches(message) for message in decoded["messages"])]
    matched = [(left, right) for left in sent for right in received
               if left["receipt"]["framed_hex"] == right["receipt"]["framed_hex"]]
    require(bool(matched), "message has no matched actual source write/target read")
    return matched[0]


def _score_observation(events, snapshot, after=0):
    if snapshot.get("source") != "go.pubsub.RawTracer_mesh_and_score_inspection":
        return
    sequence = snapshot.get("score_observation_sequence")
    require(type(sequence) is int and after < sequence < snapshot["sequence"],
            "Go snapshot lacks a fresh native score inspection")
    observations = [event for event in events if event["sequence"] == sequence
                    and event.get("kind") == "score" and event.get("source") == "go.pubsub.WithPeerScoreInspect"]
    require(len(observations) == 1 and observations[0].get("peer_scores") == snapshot["peer_scores"],
            "Go snapshot differs from its actual native score inspection")


def _gossip_receipts(actors, events, source, target, kind, message_id, topic, protocol, transport, fingerprint):
    def contains(value):
        return message_id in value["ids_hex"] and (kind != "ihave" or value["topic"] == topic)

    sent = [event for event, decoded in _rpcs(events[source], actors[target]["local_peer_id"], protocol,
                                            transport, "write", fingerprint) if any(contains(v) for v in decoded[kind])]
    received = [event for event, decoded in _rpcs(events[target], actors[source]["local_peer_id"], protocol,
                                                transport, "read", fingerprint) if any(contains(v) for v in decoded[kind])]
    matched = [(left, right) for left in sent for right in received
               if left["receipt"]["framed_hex"] == right["receipt"]["framed_hex"]]
    require(bool(matched), "gossip recovery lacks native matched " + kind)
    return matched[0]


def validate_case(artifact, *, expected_fingerprint=None):
    require(isinstance(artifact, dict) and artifact.get("schema_version") == 1
            and artifact.get("suite") == "pubsub-scoring", "invalid scoring case schema")
    spec, token = artifact.get("case"), artifact.get("case_token")
    require(isinstance(spec, dict) and (spec.get("source"), spec.get("destination")) in DIRECTIONS
            and spec.get("version") in ("1.0", "1.1") and spec.get("profile") in PROFILES,
            "case outside required version/profile/direction matrix")
    require(isinstance(token, str) and re.fullmatch(r"[a-f0-9]{32}", token), "invalid scoring case token")
    require(artifact.get("errors") == [] and artifact.get("cleanup_errors") == [], "fixture/cleanup failed")
    actors, processes, roles = artifact.get("raw"), artifact.get("processes"), artifact.get("roles")
    require(isinstance(actors, dict) and set(actors) == {"victim", "offender", "replacement", "sink"}
            and isinstance(processes, dict) and set(processes) == set(actors)
            and isinstance(roles, dict) and set(roles) == {"offender", "replacement", "sink"}
            and set(roles.values()) == {"offender", "replacement", "sink"}, "missing/doubled actor role mapping")
    events, cleanup_framing = {}, {}
    for name, raw in actors.items():
        implementation = spec["destination"] if name == "victim" else spec["source"]
        cleanup_framing[name] = {}
        events[name] = _events(raw, implementation, token, name, cleanup_framing=cleanup_framing[name])
        _terminal_owners(raw)
        process = processes[name]
        require(isinstance(process, dict) and type(process.get("pid")) is int and process["pid"] > 0
                and type(process.get("returncode")) is int and process["returncode"] == 0
                and process.get("forced_termination") is False, "actor process was not actually joined")
    require(len({actor["local_peer_id"] for actor in actors.values()}) == 4, "actors share an identity")
    require(len({process["pid"] for process in processes.values()}) == 4, "actors share a process")
    _shutdown_barrier(artifact, actors, events)
    return _validate_traffic(artifact, actors, events, cleanup_framing, expected_fingerprint)


def validate_active_case(artifact, snapshots, *, expected_fingerprint=None):
    """Prove only active traffic, never the terminal outcome of another execution."""
    require(isinstance(artifact, dict) and artifact.get("schema_version") == 1
            and artifact.get("suite") == "pubsub-scoring", "invalid active scoring case schema")
    spec, token = artifact.get("case"), artifact.get("case_token")
    require(isinstance(spec, dict) and (spec.get("source"), spec.get("destination")) in DIRECTIONS
            and spec.get("version") in ("1.0", "1.1") and spec.get("profile") in PROFILES,
            "active case outside required matrix")
    require(isinstance(token, str) and re.fullmatch(r"[a-f0-9]{32}", token), "invalid active scoring token")
    final, processes = artifact.get("raw"), artifact.get("processes")
    require(isinstance(final, dict) and isinstance(processes, dict) and isinstance(snapshots, dict)
            and set(final) == set(processes) == set(snapshots) == {"victim", "offender", "replacement", "sink"},
            "active case lacks four independently captured owners")
    roles = artifact.get("roles")
    require(roles == {role: role for role in ("offender", "replacement", "sink")}, "invalid active role mapping")
    barrier = artifact.get("shutdown_barrier")
    require(isinstance(barrier, dict) and isinstance(barrier.get("operations"), list), "missing active Prepare barrier")
    rows = barrier["operations"][:4]
    require(len(rows) == 4 and {row.get("actor") for row in rows} == set(final)
            and all(row.get("kind") == "prepare_ack" for row in rows), "missing four actual active ACKs")
    actors, events, cleanup_framing = {}, {}, {}
    for row in rows:
        role = row["actor"]
        process = processes[role]
        require(type(process.get("pid")) is int and process["pid"] > 0
                and type(process.get("returncode")) is int and process["returncode"] in (0, 1)
                and process.get("forced_termination") is False, "original traffic owner was not actually joined")
        captured = prepared_snapshot(snapshots[role], row, final[role], process["pid"], terminal_success=False)
        actors[role] = captured
        implementation = spec["destination"] if role == "victim" else spec["source"]
        cleanup_framing[role] = {}
        events[role] = _events(captured, implementation, token, role,
                              cleanup_framing=cleanup_framing[role], active=True)
    require(len({actor["local_peer_id"] for actor in actors.values()}) == 4
            and len({process["pid"] for process in processes.values()}) == 4, "active actors reuse identity/PID")
    return _validate_traffic(artifact, actors, events, cleanup_framing, expected_fingerprint)


def _validate_traffic(artifact, actors, events, cleanup_framing, expected_fingerprint):
    spec, token, roles = artifact["case"], artifact["case_token"], artifact["roles"]
    offender, replacement, sink = (roles[key] for key in ("offender", "replacement", "sink"))
    offender_peer = actors[offender]["local_peer_id"]
    replacement_peer, sink_peer = actors[replacement]["local_peer_id"], actors[sink]["local_peer_id"]
    protocol, transport, topic = "/meshsub/" + spec["version"] + ".0", PROFILES[spec["profile"]], "forge-pr11:" + token
    victim = events["victim"]
    allowed_graph = {"victim": {offender_peer, replacement_peer}, offender: {actors["victim"]["local_peer_id"]},
                     replacement: {actors["victim"]["local_peer_id"], sink_peer}, sink: {replacement_peer}}
    for name in events:
        if actors[name]["implementation"] == "go" and transport == "quic":
            require(any(event.get("kind") == "native_quic_connection" for event in events[name])
                    and all(event.get("source") in {"go.quic.native_stream.read", "go.quic.native_stream.write"}
                            for event in events[name] if event.get("kind") == "rpc"),
                    "Go QUIC requires actual lower connection/stream RPC authority")
        connected = {event.get("peer_id") for event in events[name] if event.get("kind") == "connection"}
        require(connected == allowed_graph[name], "actual connection graph differs from isolated repaired path")
        for event in events[name]:
            if event.get("kind") == "native_terminal_state" and transport == "tcp-pnet-noise":
                owner = _single(events[name], "connection", connection_id=event["connection_id"], peer_id=event["peer_id"])
                require(owner.get("pnet_verified") is True and expected_fingerprint is not None
                        and owner.get("pnet_fingerprint") == expected_fingerprint,
                        "Yamux terminal owner lacks actual private-group fingerprint")
            if event.get("kind") == "rpc":
                _owner(events[name], event, _rpc_peer(event), protocol, transport, expected_fingerprint)
                validate_rpc_receipt(event.get("receipt"), protocol, event["direction"])
            if event.get("kind") == "native_rejected_stream_disposal":
                owner = _connection_owner(events[name], event["connection_id"], event["peer_id"], "go", transport, expected_fingerprint)
                require(owner["sequence"] < event["sequence"] and event["protocol_at_disposal"] == protocol,
                        "rejected disposal lacks actual preceding native carrier/profile")
            if event.get("kind") == "pre_cancel_retained_reset_return":
                _stream_owner(events[name], event, event["peer_id"], protocol, transport, expected_fingerprint)
            if actors[name]["implementation"] == "go" and event.get("source") not in GO_QUIC_SOURCES.values() and event.get("kind") in {
                    "stream_io_terminal", "native_stream_operation", "native_stream_close_finalized", "native_stream_io_finalized"}:
                _stream_owner(events[name], event, event["remote_peer_id"], protocol, transport, expected_fingerprint,
                              peer_field="remote_peer_id")
            if actors[name]["implementation"] == "go" and event.get("source") in GO_QUIC_SOURCES.values() \
                    and event.get("kind") in {"protocol", "stream_io_terminal", "native_stream_operation", "native_quic_negotiation_io_return",
                                              "native_quic_send_context", "native_quic_framing_finalized",
                                              "native_quic_terminal_finalized"} \
                    and (event["kind"] != "protocol" or event.get("protocol") == protocol
                         or cleanup_framing[name].get(event["sequence"]) is event):
                if _quic_diagnostic_phase(event):
                    require(transport == "quic" and expected_fingerprint is None, "diagnostic owner has foreign profile")
                elif cleanup_framing[name].get(event["sequence"]) is event:
                    require(transport == "quic" and expected_fingerprint is None, "cleanup owner has foreign profile")
                    require(event["protocol"] in {"", protocol}, "native-only cleanup has foreign forced protocol")
                else:
                    _go_quic_owner(events[name], event, event.get("remote_peer_id"), protocol, transport, expected_fingerprint)
                remote_roles = [role for role, actor in actors.items() if actor["local_peer_id"] == event["remote_peer_id"]]
                require(len(remote_roles) == 1 and remote_roles[0] != name,
                        "lower terminal peer is not another actual prepared actor")
                remote = actors[remote_roles[0]]
                row = next(row for row in artifact["shutdown_barrier"]["operations"][:4] if row["actor"] == remote_roles[0])
                require(shutdown_ack(remote, remote["implementation"], remote_roles[0], token, remote["local_peer_id"],
                                     row["command_sequence"]) is not None,
                        "lower peer cancellation lacks actual other-actor Prepare prefix")
    for name, expected in ((offender, [actors["victim"]["local_peer_id"]]),
                           (replacement, [sink_peer]), (sink, [replacement_peer])):
        require(_snapshot(events[name], "before")["mesh_peer_ids"] == expected,
                "initial donor mesh is not the required native two-pair topology")
    before, ignored, penalized, repaired = (_snapshot(victim, name)
                                           for name in ("before", "ignored", "penalized", "repaired"))
    for actor_events in events.values():
        for event in actor_events:
            if event.get("kind") == "snapshot":
                _score_observation(actor_events, event)
    for name, remote in (("victim", replacement), (replacement, "victim")):
        snapshot = _snapshot(events[name], "before")
        received = _rpcs(events[name], actors[remote]["local_peer_id"], protocol, transport, "read", expected_fingerprint)
        require(any(event["sequence"] < snapshot["sequence"]
                    and {"topic": topic, "subscribe": True} in decoded["subscriptions"]
                    for event, decoded in received), "offmesh candidate lacked actual native subscription before publish")
    require(before["mesh_peer_ids"] == [offender_peer]
            and replacement_peer not in before["mesh_peer_ids"], "offender/replacement roles were not native mesh facts")
    initial, neutral, rejected = (_score(snapshot, offender_peer) for snapshot in (before, ignored, penalized))
    require(initial["value"] == neutral["value"] == 0, "ignore changed malicious score")
    if initial.get("invalid_deliveries_available") is not False:
        require(initial["invalid_deliveries"] == neutral["invalid_deliveries"] == 0,
                "ignore changed invalid delivery counter")
    gossip_input = _validation(victim, "accept:" + token + ":gossip", offender_peer, offender_peer,
                               topic, "accept", protocol, transport, expected_fingerprint)
    _message_pair(actors, events, offender, "victim", gossip_input, protocol, transport, expected_fingerprint,
                  read_after=before["sequence"])
    gossip = _validation(events[replacement], "accept:" + token + ":gossip", actors["victim"]["local_peer_id"],
                         offender_peer, topic, "accept", protocol, transport, expected_fingerprint)
    require(gossip["message_id"] == gossip_input["message_id"], "cached recovery changed native message identity")
    ihave_sent, ihave_read = _gossip_receipts(actors, events, "victim", replacement, "ihave", gossip["message_id"],
                                            topic, protocol, transport, expected_fingerprint)
    iwant_sent, iwant_read = _gossip_receipts(actors, events, replacement, "victim", "iwant", gossip["message_id"],
                                            topic, protocol, transport, expected_fingerprint)
    message_sent, message_read = _message_pair(actors, events, "victim", replacement, gossip, protocol, transport,
                                               expected_fingerprint, after=iwant_read["sequence"])
    require(gossip_input["sequence"] < ihave_sent["sequence"] < iwant_read["sequence"]
            and ihave_read["sequence"] < iwant_sent["sequence"] < message_read["sequence"],
            "cached recovery does not causally follow IHAVE and IWANT")
    gossip_sink = _validation(events[sink], "accept:" + token + ":gossip", replacement_peer,
                              offender_peer, topic, "accept", protocol, transport, expected_fingerprint)
    require(gossip_sink["message_id"] == gossip["message_id"], "gossip path combines different native messages")
    _message_pair(actors, events, replacement, sink, gossip_sink, protocol, transport, expected_fingerprint,
                  after=gossip["sequence"])
    for actor, validation in ((replacement, gossip), (sink, gossip_sink)):
        delivered = _single(events[actor], "delivery", payload_sha256=validation["payload_sha256"])
        require(all(delivered.get(field) == validation[field] for field in
                    ("propagation_peer", "author_peer", "topic", "message_id", "seqno_hex"))
                and delivered["sequence"] >= validation["sequence"], "cached recovery lacks committed native delivery")
    ignore = _validation(victim, "ignore:" + token + ":one", offender_peer, offender_peer, topic,
                         "ignore", protocol, transport, expected_fingerprint)
    reject = _validation(victim, "reject:" + token + ":one", offender_peer, offender_peer, topic,
                         "reject", protocol, transport, expected_fingerprint)
    _score_observation(victim, ignored, ignore["sequence"])
    _score_observation(victim, penalized, reject["sequence"])
    require(gossip_input["sequence"] < ihave_sent["sequence"] < iwant_read["sequence"]
            < message_sent["sequence"] < ignore["sequence"],
            "initial cached recovery was not completed before Ignore/Reject")
    _message_pair(actors, events, offender, "victim", ignore, protocol, transport, expected_fingerprint)
    _message_pair(actors, events, offender, "victim", reject, protocol, transport, expected_fingerprint)
    require(before["sequence"] < ignore["sequence"] < ignored["sequence"] < reject["sequence"]
            < penalized["sequence"] <= repaired["sequence"], "native validation/snapshot order differs")
    require(rejected["value"] < -80
            and (rejected.get("invalid_deliveries_available") is False or rejected["invalid_deliveries"] > 0),
            "real P4 did not cross configured graylist/publish/gossip thresholds")
    require(offender_peer not in repaired["mesh_peer_ids"]
            and repaired["mesh_peer_ids"] == [replacement_peer],
            "native mesh did not replace rejected peer")
    _control_pair(actors, events, "victim", offender, "prune", topic, protocol, transport, reject["sequence"], expected_fingerprint)
    _, graft_received = _control_pair(actors, events, "victim", replacement, "graft", topic, protocol, transport,
                                      reject["sequence"], expected_fingerprint)
    accepted_payload = "accept:" + token + ":repaired"
    published = _single(victim, "publish", payload_sha256=hashlib.sha256(accepted_payload.encode()).hexdigest(), topic=topic)
    require(published["sequence"] > repaired["sequence"], "accepted publish predates native mesh repair")
    replacement_repaired = _snapshot(events[replacement], "repaired")
    require(set(replacement_repaired["mesh_peer_ids"]) == {actors["victim"]["local_peer_id"], sink_peer},
            "replacement did not natively join both repaired path edges")
    accepted_id = None
    replacement_accepted = None
    for actor, source in ((replacement, "victim"), (sink, replacement)):
        validation = _validation(events[actor], accepted_payload, actors[source]["local_peer_id"], actors["victim"]["local_peer_id"],
                                 topic, "accept", protocol, transport, expected_fingerprint)
        if actor == replacement:
            require(validation["sequence"] > graft_received["sequence"], "delivery predates native replacement GRAFT")
            accepted_id, replacement_accepted = validation["message_id"], validation["sequence"]
        require(validation["message_id"] == accepted_id, "repaired path combines different native messages")
        _message_pair(actors, events, source, actor, validation, protocol, transport, expected_fingerprint,
                      after=repaired["sequence"] if source == "victim" else replacement_accepted,
                      read_after=max(graft_received["sequence"], replacement_repaired["sequence"]) if actor == replacement else 0)
        delivery = _single(events[actor], "delivery", payload_sha256=hashlib.sha256(accepted_payload.encode()).hexdigest(),
                           propagation_peer=actors[source]["local_peer_id"])
        require(delivery.get("author_peer") == actors["victim"]["local_peer_id"] and delivery.get("message_id") == validation["message_id"]
                and delivery.get("seqno_hex") == validation["seqno_hex"]
                and delivery["sequence"] >= validation["sequence"], "delivery is not on the repaired mesh path")
    for payload in ("ignore:" + token + ":one", "reject:" + token + ":one"):
        digest = hashlib.sha256(payload.encode()).hexdigest()
        require(not any(event.get("kind") == "delivery" and event.get("payload_sha256") == digest
                        for name in ("victim", replacement, sink) for event in events[name]),
                "nonaccepted message reached an application beyond its author")
        require(not any(message["payload_sha256"] == digest
                        for _, decoded in _rpcs(events[sink], replacement_peer, protocol, transport, "read", expected_fingerprint)
                        for message in decoded["messages"]), "nonaccepted message was forwarded to sink")
        for remote in (offender_peer, replacement_peer):
            require(not any(message["payload_sha256"] == digest
                            for _, decoded in _rpcs(victim, remote, protocol, transport, "write", expected_fingerprint)
                            for message in decoded["messages"]), "victim forwarded a nonaccepted message")
    return {"protocol": protocol, "profile": spec["profile"], "source": spec["source"],
            "destination": spec["destination"], "repaired_path": ["victim", replacement, sink]}
