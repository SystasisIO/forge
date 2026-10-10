"""Exact PR12 application-observation schemas, never native wire or delivery proof.

This module validates individual rows only. References, command causality,
reconstruction inputs and owner shutdown must still be checked by the case gate.
"""

import hashlib
import re

from partial_fixture import Metadata, decode_part, group_id, part_bytes, reconstruct
from rust_upgrade_evidence import _peer


BASE = {"sequence", "mono_ns", "kind", "source"}
VERSIONS = {"idontwant": "1.2", "partial": "1.3", "advertisement": "1.3"}
GO_APP = "go.fixture.extensions.application"
FORGE_APP = "forge.fixture.extensions.application"
RUST_APP = "rust.fixture.extensions.application"
RUST_BEHAVIOUR = "rust.libp2p.gossipsub.public-Behaviour"


def _require(value, message):
    if not value:
        raise ValueError("PubSub extension event: " + message)


def _integer(low, high):
    return lambda value: type(value) is int and low <= value <= high


def _text(low, high):
    return lambda value: type(value) is str and low <= len(value.encode("utf-8")) <= high and "\0" not in value


def _hex(low, high):
    return lambda value: (type(value) is str and 2 * low <= len(value) <= 2 * high
                          and len(value) % 2 == 0 and re.fullmatch(r"[0-9a-f]*", value) is not None)


def _literal(expected):
    return lambda value: type(value) is type(expected) and value == expected


def _choice(*values):
    return lambda value: type(value) is str and value in values


def _nullable(check):
    return lambda value: value is None or check(value)


def _array(check, low, high):
    return lambda value: type(value) is list and low <= len(value) <= high and all(check(item) for item in value)


BOOL = lambda value: type(value) is bool
PEER = lambda value: _peer(value) is not None
ERROR = _nullable(_text(1, 4096))
REF = _integer(1, 2048)
OPTIONAL_REF = _integer(0, 2048)
COMMAND = _integer(1, 64)
GROUP = _hex(20, 20)
METADATA = _hex(7, 7)
HASH = _hex(32, 32)
MESSAGE = {
    "propagation_peer": PEER, "author_peer": PEER, "topic": _text(1, 256),
    "message_id": _hex(1, 256), "seqno_hex": _hex(8, 8), "payload_sha256": HASH,
}
GO_MESSAGE = MESSAGE | {
    "author_hex": _hex(1, 64), "payload_bytes": _integer(1, 4096),
    "message_id_observed": _literal(True), "message_id_basis": _literal("native_pubsub_Message.ID"),
}
FORGE_MESSAGE = MESSAGE | {
    "payload_bytes": _integer(1, 4096),
    "message_id_basis": _literal("native_pubsub_codec_message_id_from_callback_message"),
}
LOCAL = {
    "group_id_hex": GROUP, "revision": _integer(1, 2**32 - 1),
    "have": _integer(0, 7), "want": _integer(0, 7),
}
RUST_LOCAL = LOCAL | {
    "received_have": _integer(0, 7), "metadata_hex": METADATA,
    "group_basis": _literal("case_token_and_fixed_sequence_one_not_content_hash"),
}
PAYLOAD = {"payload_hex": _hex(1, 768), "payload_sha256": HASH, "payload_bytes": _integer(1, 768)}
SCHEMAS = {}


def _schema(implementation, kind, source, fields, *, modes=("partial",), optional=None):
    SCHEMAS[implementation, kind] = source, frozenset(modes), fields, optional or {}


_schema("go", "validation_hold_armed", "go.fixture.extensions.command", {
    "command_sequence": COMMAND, "payload_sha256": HASH, "payload_bytes": _integer(1, 4096),
}, modes=("idontwant",))
_schema("go", "validation_release", "go.fixture.extensions.command", {
    "command_sequence": COMMAND, "held_observation_sequence": REF,
}, modes=("idontwant",))
_schema("go", "validation_held", "go.fixture.extensions.ValidatorEx", GO_MESSAGE | {
    "command_sequence": COMMAND, "committed": _literal(False),
}, modes=("idontwant",))
_schema("go", "validation_resumed", "go.fixture.extensions.ValidatorEx", GO_MESSAGE | {
    "held_observation_sequence": REF, "committed": _literal(False), "released": BOOL,
}, modes=("idontwant",), optional={"error": _text(1, 4096)})
_schema("go", "partial_incoming", "go.pubsub.partialmessages.OnIncomingRPC", {
    "peer_id": PEER, "group_id_hex": GROUP, "body_present": BOOL, "body_hex": _hex(0, 260),
    "metadata_present": BOOL, "metadata_hex": _hex(0, 7),
    "authority": _literal("native_application_callback_not_validation_or_delivery"),
}, modes=("partial", "advertisement"))
_schema("go", "partial_gossip", "go.pubsub.partialmessages.OnEmitGossip", {
    "group_id_hex": GROUP, "peer_ids": _array(PEER, 0, 16),
    "authority": _literal("native_gossip_recipient_callback_not_wire_send"),
})
_schema("go", "partial_offer", GO_APP, LOCAL | {
    "group_basis": _literal("case_token_and_sequence_not_content_hash"),
})
_schema("go", "partial_rejected", GO_APP, {
    "observation_sequence": REF, "peer_id": PEER, "group_id_hex": GROUP, "error": _text(1, 4096),
    "scope": _literal("application_only_no_router_validation_result"),
})
_schema("go", "partial_applied", GO_APP, {
    "observation_sequence": REF, "peer_id": PEER, "group_id_hex": GROUP, "metadata_hex": METADATA,
    "body_applied": BOOL, "authority": _literal("application_checked_bytes_not_signed_message_validation"),
})
_schema("go", "partial_reconstructed", GO_APP, PAYLOAD | {
    "observation_sequence": REF, "group_id_hex": GROUP, "part_observation_sequences": _array(REF, 3, 3),
    "authority": _literal("application_reconstruction_not_router_delivery_or_signature"),
})
_schema("go", "partial_publish_return", "go.pubsub.PublishPartial", {
    "group_id_hex": GROUP, "observation_sequence": OPTIONAL_REF, "command_sequence": _integer(0, 64), "error": ERROR,
})
_schema("go", "partial_publish_action", "go.pubsub.PublishPartial.actions", {
    "peer_id": PEER, "group_id_hex": GROUP, "observation_sequence": OPTIONAL_REF,
    "command_sequence": _integer(0, 64), "requests_partial": BOOL, "gossip_metadata_only": BOOL,
    "body_present": BOOL, "body_hex": _hex(0, 260), "metadata_hex": METADATA,
    "authority": _literal("application_submission_not_wire_write"),
})
_schema("go", "extension_stopped", "go.fixture.extensions.owned_worker_join", {
    "admission_closed": _literal(True), "worker_started": BOOL, "worker_joined": BOOL,
    "pending_work": _integer(0, 32), "active_validators": _literal(0), "active_publish_callbacks": _literal(0),
    "scope": _literal("fixture_application_worker_and_admitted_callbacks_not_router_goroutines"),
}, modes=tuple(VERSIONS))

_schema("rust", "validation_hold_armed", RUST_APP, {
    "command_sequence": COMMAND, "payload_sha256": HASH, "payload_bytes": _integer(1, 4096),
    "maximum_lifetime_ms": _literal(10000), "committed": _literal(False),
}, modes=("idontwant",))
_schema("rust", "validation_held", RUST_APP, MESSAGE | {
    "command_sequence": COMMAND, "committed": _literal(False),
}, modes=("idontwant",))
for _kind in ("validation_released", "validation_cancelled"):
    _schema("rust", _kind, RUST_APP, {
        "hold_command_sequence": COMMAND, "command_sequence": _nullable(COMMAND), "payload_sha256": HASH,
        "cancellation": _nullable(_choice("ten_second_deadline", "application_shutdown")),
        "validation_commit": BOOL, "error": ERROR,
    }, modes=("idontwant",), optional=MESSAGE)
_schema("rust", "validation_release_requested", RUST_APP, MESSAGE | {
    "hold_command_sequence": COMMAND, "command_sequence": COMMAND, "cancellation": _literal(None),
    "validation_commit": _literal(False),
}, modes=("idontwant",))
_schema("rust", "validation_not_committed", RUST_BEHAVIOUR, MESSAGE | {
    "outcome": _choice("accept", "reject", "ignore"),
    "report_message_validation_result": _literal(False), "validation_commit": _literal(False),
}, modes=tuple(VERSIONS))
_schema("rust", "partial_incoming", "rust.libp2p.gossipsub.Event.Partial", {
    "peer_id": PEER, "topic": _text(0, 256), "group_id_hex": _hex(0, 20), "group_bytes": _integer(0, 1024 * 1024),
    "body_present": BOOL, "body_hex": _nullable(_hex(0, 260)), "body_bytes": _nullable(_integer(0, 1024 * 1024)),
    "metadata_present": BOOL, "metadata_hex": _nullable(_hex(0, 7)), "metadata_bytes": _nullable(_integer(0, 1024 * 1024)),
    "authority": _literal("native_application_event_not_signed_validation_or_delivery"),
}, modes=("partial", "advertisement"))
_schema("rust", "partial_application_closed", RUST_APP, {
    "peer_id": PEER, "group_id_hex": GROUP, "admitted": _literal(False),
}, modes=("partial", "advertisement"))
_schema("rust", "partial_rejected", RUST_APP, {
    "peer_id": PEER, "group_id_hex": _hex(0, 20), "error": _text(1, 4096),
    "scope": _literal("application_only_no_report_invalid_partial_or_full_validation"),
}, modes=("partial", "advertisement"))
_schema("rust", "partial_offer", RUST_APP, RUST_LOCAL)
_schema("rust", "partial_applied", RUST_APP, RUST_LOCAL | {
    "peer_id": PEER, "body_applied": BOOL, "incoming_sequence": REF,
    "authority": _literal("application_checked_bytes_not_signed_message_validation"),
})
_schema("rust", "partial_reconstructed", RUST_APP, PAYLOAD | {
    "group_id_hex": GROUP, "parts_hex": _array(_hex(5, 260), 3, 3),
    "part_incoming_sequences": _array(REF, 3, 3), "part_peer_ids": _array(PEER, 3, 3),
    "authority": _literal("application_reconstruction_not_router_delivery_or_signature"),
})
_schema("rust", "partial_publish_return", "rust.libp2p.gossipsub.Behaviour.publish_partial", {
    "group_id_hex": GROUP, "error": ERROR, "authority": _literal("native_public_API_return_not_wire_write"),
    "off_mesh_basis": _literal("only_native_same_group_state_recipients"),
})
_view = {"peer_id": _nullable(PEER), "view": _choice("local_advertisement", "remote_peer_metadata"), "group_id_hex": GROUP}
_schema("rust", "partial_metadata_hook", "rust.libp2p.gossipsub.Metadata.update", _view | {
    "metadata_hex": _hex(0, 7), "metadata_bytes": _integer(0, 1024 * 1024),
    "changed": _nullable(BOOL), "error": ERROR,
    "authority": _literal("application_metadata_replacement_not_generic_router_validation"),
})
_schema("rust", "partial_data_hook", "rust.libp2p.gossipsub.Metadata.update_from_data", _view | {
    "body_hex": _hex(0, 260), "body_bytes": _integer(0, 1024 * 1024),
    "authority": _literal("native_hook_observation_no_predicted_part_ownership"),
})
_schema("rust", "partial_action_hook", "rust.libp2p.gossipsub.Partial.partial_action_from_metadata", {
    "peer_id": PEER, "group_id_hex": GROUP, "metadata_present": BOOL,
    "metadata_hex": _nullable(_hex(0, 7)), "metadata_bytes": _nullable(_integer(0, 1024 * 1024)),
    "local_metadata_hex": METADATA, "need": _nullable(BOOL), "body_hex": _nullable(_hex(5, 260)), "error": ERROR,
    "authority": _literal("native_application_action_not_wire_write_or_signed_delivery"),
})
_schema("rust", "extension_stopped", RUST_APP, {
    "admission_closed": _literal(True), "pending_validations": _literal(0), "pending_hooks": _integer(0, 64),
    "drain_error": ERROR, "cancellation_error": ERROR,
    "scope": _literal("inline_application_only_not_native_close_or_transport_join"),
}, modes=tuple(VERSIONS))

_schema("forge", "validation_hold_armed", "forge.fixture.extensions.command", {
    "extension_mode": _literal("idontwant"), "topic": _text(1, 256), "command_sequence": COMMAND,
    "payload_sha256": HASH, "payload_bytes": _integer(1, 4096),
}, modes=("idontwant",))
_schema("forge", "validation_release", "forge.fixture.extensions.command", {
    "extension_mode": _literal("idontwant"), "topic": _text(1, 256), "command_sequence": COMMAND,
    "held_observation_sequence": REF,
}, modes=("idontwant",))
_schema("forge", "validation_held", "forge.fixture.extensions.validation", FORGE_MESSAGE | {
    "extension_mode": _literal("idontwant"), "command_sequence": COMMAND, "committed": _literal(False),
}, modes=("idontwant",))
_schema("forge", "validation_resumed", "forge.fixture.extensions.validation", FORGE_MESSAGE | {
    "extension_mode": _literal("idontwant"), "held_observation_sequence": REF,
    "committed": _literal(False), "released": BOOL,
}, modes=("idontwant",), optional={"error": _choice(
    "validation hold expired or cancelled", "validation hold woke without release")})
_schema("forge", "publish", "forge.node.async_publish", {
    "command_sequence": COMMAND, "topic": _text(1, 256), "payload_sha256": HASH,
}, modes=tuple(VERSIONS))
_schema("forge", "partial_advertise_return", "forge.node.async_advertise_partial", {
    "command_sequence": COMMAND, "topic": _text(1, 256), "group_id_hex": GROUP,
    "error": _nullable(_text(1, 512)), "authority": _literal("local_group_registration_not_wire_send"),
})
_schema("forge", "partial_offer", FORGE_APP, RUST_LOCAL | {
    "command_sequence": COMMAND, "local_have": _integer(0, 7), "advertised": _literal(True),
})
_schema("forge", "partial_incoming", "forge.pubsub.partial_handler", {
    "peer_id": PEER, "topic": _text(0, 256), "group_present": BOOL,
    "group_id_hex": _nullable(_hex(0, 20)), "group_bytes": _nullable(_integer(0, 20)),
    "body_present": BOOL, "body_hex": _nullable(_hex(0, 260)), "body_bytes": _nullable(_integer(0, 4096)),
    "metadata_present": BOOL, "metadata_hex": _nullable(_hex(0, 7)), "metadata_bytes": _nullable(_integer(0, 7)),
    "authority": _literal("native_application_callback_not_validation_or_delivery"),
})
_schema("forge", "partial_rejected", FORGE_APP, {
    "peer_id": PEER, "group_id_hex": _nullable(_hex(0, 20)), "observation_sequence": REF,
    "error": _text(1, 512), "scope": _literal("application_only_no_router_validation_result"),
})
_schema("forge", "partial_applied", FORGE_APP, RUST_LOCAL | {
    "peer_id": PEER, "body_applied": BOOL, "incoming_sequence": REF,
    "authority": _literal("application_checked_bytes_not_signed_message_validation"),
})
_schema("forge", "partial_gossip", "forge.pubsub.partial_gossip_handler", {
    "group_id_hex": GROUP, "peer_ids": _array(PEER, 0, 16),
    "authority": _literal("native_gossip_recipient_callback_not_wire_send"),
})
_schema("forge", "partial_publish_action", "forge.node.async_send_partial.application_submission", {
    "peer_id": PEER, "topic": _text(1, 256), "group_id_hex": GROUP, "observation_sequence": REF,
    "gossip_metadata_only": BOOL, "body_present": BOOL, "body_hex": _hex(0, 260), "metadata_hex": METADATA,
    "authority": _literal("application_submission_not_wire_write"),
})
_schema("forge", "partial_publish_return", "forge.node.async_send_partial", {
    "peer_id": PEER, "topic": _text(1, 256), "group_id_hex": GROUP, "observation_sequence": REF,
    "error": _nullable(_text(1, 512)), "authority": _literal("native_public_API_return_not_delivery"),
})
_schema("forge", "partial_reconstructed", FORGE_APP, PAYLOAD | {
    "group_id_hex": GROUP, "parts_hex": _array(_hex(5, 260), 3, 3),
    "part_incoming_sequences": _array(REF, 3, 3), "part_peer_ids": _array(PEER, 3, 3),
    "authority": _literal("application_reconstruction_not_router_delivery_or_signature"),
})
_schema("forge", "partial_unsubscribe_return", "forge.node.async_unsubscribe.partial_topic", {
    "topic": _text(1, 256), "admission_closed": _literal(True), "active_work": _literal(0),
    "error": _nullable(_text(1, 512)), "scope": _literal("scoped_unsubscribe_not_native_callback_join"),
})
_schema("forge", "extension_stopped", "forge.fixture.extensions.application_drain", {
    "admission_closed": _literal(True), "active_work": _literal(0), "active_validators": _literal(0),
    "inputs": _integer(0, 64), "partial_registration_active": _literal(False), "drain_error": _literal(None),
    "scope": _literal("fixture_admitted_work_not_native_callback_or_node_join"),
}, modes=tuple(VERSIONS))

# Full-message publish also exists in PR11 and remains under its original source gate there.
KINDS = frozenset(kind for _, kind in SCHEMAS if kind != "publish")
APPLICATION_SOURCES = frozenset(schema[0] for (_, kind), schema in SCHEMAS.items() if kind != "publish") - {RUST_BEHAVIOUR}
APPLICATION_FIELDS = {"extension_mode", "group_id_hex", "part_observation_sequences", "part_incoming_sequences",
                      "held_observation_sequence", "hold_command_sequence"}


def _captured(event, name, cap, *, present=None):
    encoded, size = event[name + "_hex"], event[name + "_bytes"]
    if present is False:
        _require(encoded is None and size is None, "absent " + name + " has bytes")
    else:
        _require(encoded is not None and type(size) is int
                 and len(encoded) == 2 * min(size, cap), "inconsistent captured " + name)


def _relations(raw, event):
    implementation, kind = raw["implementation"], event["kind"]
    token = raw["case_token"]
    if implementation == "rust" and kind in {"validation_released", "validation_cancelled"}:
        identity_fields = set(MESSAGE) - {"payload_sha256"}
        _require(not identity_fields.intersection(event) or identity_fields <= set(event),
                 "incomplete held-message identity")
    if kind in {"partial_offer", "partial_applied", "partial_reconstructed", "partial_gossip",
                "partial_publish_return", "partial_publish_action", "partial_application_closed",
                "partial_metadata_hook", "partial_data_hook", "partial_action_hook", "partial_advertise_return"}:
        _require(event["group_id_hex"] == group_id(token).hex(), "foreign owned application group")
    if kind in {"validation_held", "validation_resumed", "validation_not_committed"} or "message_id" in event:
        _require(event["topic"] == "forge-pr11:" + token, "foreign held-message topic")
        if implementation == "go":
            author = _peer(event["author_peer"])
            _require(bytes.fromhex(event["author_hex"]) == author, "message author bytes mismatch")
        else:
            author = _peer(event["author_peer"])
        _require(event["message_id"] == (author + bytes.fromhex(event["seqno_hex"])).hex(), "message ID mismatch")
    if kind in {"partial_offer", "partial_applied"}:
        metadata = Metadata.decode(bytes.fromhex(event["metadata_hex"])) if "metadata_hex" in event else Metadata(
            event["revision"], event["have"], event["want"])
        metadata.encode()
        _require(metadata.want == 7 ^ metadata.have, "invalid local complement mask")
        if implementation in {"rust", "forge"}:
            _require((metadata.revision, metadata.have, metadata.want) ==
                     (event["revision"], event["have"], event["want"])
                     and event["received_have"] & ~event["have"] == 0, "inconsistent local application state")
    if kind == "partial_reconstructed":
        payload = bytes.fromhex(event["payload_hex"])
        _require(event["payload_bytes"] == len(payload) and hashlib.sha256(payload).hexdigest() == event["payload_sha256"]
                 and payload == b"".join(part_bytes(token, i) for i in range(3)), "reconstruction bytes/hash mismatch")
        if implementation in {"rust", "forge"}:
            _require(reconstruct(token, [bytes.fromhex(part) for part in event["parts_hex"]]) == payload,
                     "reconstruction parts mismatch")
    for name in ("observation_sequence", "held_observation_sequence", "incoming_sequence"):
        if name in event:
            _require(event[name] < event["sequence"], "observation reference is not earlier")
    for name in ("part_observation_sequences", "part_incoming_sequences"):
        if name in event:
            _require(len(set(event[name])) == 3 and all(ref < event["sequence"] for ref in event[name]),
                     "invalid part observation references")
    if implementation == "go":
        if kind in {"partial_incoming", "partial_publish_action"}:
            _require(event["body_present"] or event["body_hex"] == "", "absent body has bytes")
        if kind == "partial_incoming":
            _require(event["metadata_present"] or event["metadata_hex"] == "", "absent metadata has bytes")
        if kind in {"partial_publish_action", "partial_publish_return"}:
            _require(bool(event["observation_sequence"]) != bool(event["command_sequence"]), "publication origin is ambiguous")
        if kind == "partial_publish_action":
            Metadata.decode(bytes.fromhex(event["metadata_hex"]))
            _require(not event["body_present"] or event["requests_partial"] and not event["gossip_metadata_only"],
                     "partial body sent to a full-only/gossip recipient")
            if event["body_present"]:
                index, data = decode_part(bytes.fromhex(event["body_hex"]))
                _require(data == part_bytes(token, index), "wrong submitted part bytes")
        if kind == "partial_gossip":
            _require(len(set(event["peer_ids"])) == len(event["peer_ids"]), "duplicate gossip recipient")
        if kind == "validation_resumed":
            _require(event["released"] == ("error" not in event), "release/error contradiction")
        if kind == "extension_stopped":
            _require(event["worker_started"] == event["worker_joined"], "invented worker join")
    elif implementation == "forge":
        if "topic" in event:
            _require(event["topic"] == "forge-pr11:" + token, "foreign application topic")
        if kind == "partial_incoming":
            _captured({"group_hex": event["group_id_hex"], "group_bytes": event["group_bytes"]},
                      "group", 20, present=event["group_present"])
            _captured(event, "body", 260, present=event["body_present"])
            _captured(event, "metadata", 7, present=event["metadata_present"])
        if kind == "partial_offer":
            _require(event["have"] == event["local_have"] | event["received_have"], "invented local availability")
        if kind == "partial_applied":
            _require(not event["body_applied"] or event["received_have"] != 0, "applied body lacks received state")
        if kind == "partial_publish_action":
            metadata = Metadata.decode(bytes.fromhex(event["metadata_hex"]))
            _require(metadata.want == 7 ^ metadata.have, "invalid submitted local complement mask")
            _require(event["body_present"] or event["body_hex"] == "", "absent body has bytes")
            _require(not event["gossip_metadata_only"] or not event["body_present"], "gossip submitted a body")
            if event["body_present"]:
                index, data = decode_part(bytes.fromhex(event["body_hex"]))
                _require(data == part_bytes(token, index) and metadata.have & (1 << index), "wrong submitted part bytes/state")
        if kind == "partial_gossip":
            _require(len(set(event["peer_ids"])) == len(event["peer_ids"]), "duplicate gossip recipient")
        if kind == "validation_resumed":
            _require(event["released"] == ("error" not in event), "release/error contradiction")
        if isinstance(event.get("error"), str):
            _require(all(32 <= ord(c) <= 126 for c in event["error"]), "unsanitized application error")
        if kind == "extension_stopped" and raw["extension"] != "partial":
            _require(event["inputs"] == 0, "partial work claimed without callbacks")
    else:
        if kind == "partial_incoming":
            _require(len(event["group_id_hex"]) == 2 * min(event["group_bytes"], 20), "group capture size mismatch")
            _captured(event, "body", 260, present=event["body_present"])
            _captured(event, "metadata", 7, present=event["metadata_present"])
        if kind in {"partial_metadata_hook", "partial_action_hook"}:
            _captured(event, "metadata", 7, present=event.get("metadata_present", True))
        if kind == "partial_data_hook":
            _captured(event, "body", 260)
        if kind in {"partial_metadata_hook", "partial_data_hook"}:
            _require((event["peer_id"] is None) == (event["view"] == "local_advertisement"), "foreign metadata view")
        if kind == "partial_metadata_hook":
            _require((event["changed"] is None) == (event["error"] is not None), "hook result/error contradiction")
            if event["error"] is None:
                _require(event["metadata_bytes"] == 7, "successful metadata hook has truncated input")
                Metadata.decode(bytes.fromhex(event["metadata_hex"]))
        if kind == "partial_action_hook":
            local = Metadata.decode(bytes.fromhex(event["local_metadata_hex"]))
            _require((event["need"] is None) == (event["error"] is not None)
                     and (event["error"] is None or event["body_hex"] is None), "action result/error contradiction")
            if event["error"] is None and event["metadata_present"]:
                _require(event["metadata_bytes"] == 7, "successful action hook has truncated input")
                Metadata.decode(bytes.fromhex(event["metadata_hex"]))
            if event["body_hex"] is not None:
                index, data = decode_part(bytes.fromhex(event["body_hex"]))
                _require(data == part_bytes(token, index) and local.have & (1 << index), "wrong submitted action part")
        if kind in {"validation_released", "validation_cancelled"}:
            _require(event["validation_commit"] == ("message_id" in event and event["error"] is None),
                     "invalid validation commit observation")
            if kind == "validation_released":
                _require(event["command_sequence"] is not None and event["cancellation"] is None,
                         "release lacks command or invents cancellation")
            else:
                _require(event["command_sequence"] is None and event["cancellation"] is not None, "invalid cancellation")


def validate_extension_event(raw, event):
    """Return True only for a validated exact application kind/source, else False.

    A claimed extension kind/source with malformed fields raises ValueError;
    callers must not use False as a fallback for a malformed recognized event.
    """
    _require(type(raw) is dict and type(event) is dict, "expected objects")
    implementation, kind, source = raw.get("implementation"), event.get("kind"), event.get("source")
    _require(type(kind) is str and type(source) is str, "invalid kind/source type")
    if implementation == "forge" and raw.get("extension") not in (None, ""):
        _require("extension_capture_error" in raw and raw["extension_capture_error"] is None,
                 "missing or failed Forge extension capture")
    schema = SCHEMAS.get((implementation, kind)) if type(implementation) is str else None
    if implementation == "forge" and kind == "publish" and raw.get("extension") in (None, ""):
        schema = None
    if schema is None:
        _require(kind not in KINDS and source not in APPLICATION_SOURCES and not APPLICATION_FIELDS.intersection(event),
                 "unsupported or spoofed application authority")
        return False
    expected_source, modes, fields, optional = schema
    mode = raw.get("extension")
    _require(source == expected_source and type(mode) is str and mode in modes
             and raw.get("version") == VERSIONS[mode] and type(raw.get("version")) is str,
             "wrong source, extension mode or version")
    _require(raw.get("requests_partial") is (mode == "partial"), "wrong partial topic mode")
    _require(type(raw.get("case_token")) is str and re.fullmatch(r"[0-9a-f]{32}", raw["case_token"]) is not None,
             "invalid case token")
    common = BASE | ({"topic", "extension_mode"} if implementation == "go" else set())
    _require((common | fields.keys()) <= event.keys() and event.keys() <= (common | fields.keys() | optional.keys()),
             "missing or unknown fields")
    for name, check in (fields | optional).items():
        if name in event:
            _require(check(event[name]), "invalid " + name)
    if implementation == "go":
        _require(event["extension_mode"] == mode and event["topic"] == "forge-pr11:" + raw["case_token"],
                 "foreign application topic/mode")
    # The caller owns monotonicity/contiguity; a local reference still needs a bounded integer anchor.
    _require(REF(event["sequence"]), "invalid event reference anchor")
    _relations(raw, event)
    return True
