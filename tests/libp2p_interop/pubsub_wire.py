"""Bounded inspection of native GossipSub receipts, not configured protocols."""

import hashlib
import re

from autorelay_wire import _fields
from rust_upgrade_evidence import _varint

MAX_FRAME = 16 * 1024
MAX_ENTRIES = 128
PROTOCOLS = {f"/meshsub/1.{minor}.0" for minor in range(4)}
MAX_GROUP_ID = 256
MAX_PARTS_METADATA = 4096


def _one(fields, number, wire, *, required=False):
    values = fields.get(number, [])
    if not values and not required:
        return None
    if len(values) != 1 or values[0][0] != wire:
        raise ValueError("missing, ambiguous or wrong-type PubSub field")
    return values[0][1]


def _text(fields, number, *, required=False):
    value = _one(fields, number, 2, required=required)
    if value is None:
        return None
    try:
        out = value.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("invalid PubSub topic UTF-8") from error
    if not 0 < len(value) <= 255:
        raise ValueError("PubSub topic outside fixture bounds")
    return out


def _repeated(fields, number, wire=2):
    values = fields.get(number, [])
    if len(values) > MAX_ENTRIES or any(kind != wire for kind, _ in values):
        raise ValueError("PubSub repeated field outside fixture bounds")
    return [value for _, value in values]


def _flag(fields, number):
    value = _one(fields, number, 0)
    if value not in (None, 0, 1):
        raise ValueError("noncanonical PubSub fixture flag")
    return value


def _bounded_hex(fields, number, limit, *, required=False):
    value = _one(fields, number, 2, required=required)
    if value is None:
        return None
    if len(value) > limit or required and not value:
        raise ValueError("PubSub extension field outside fixture bounds")
    return value.hex()


def decode_rpc(body, protocol):
    if protocol not in PROTOCOLS or not isinstance(body, bytes) or len(body) > MAX_FRAME:
        raise ValueError("unsupported or oversized PubSub RPC")
    fields = _fields(body)
    modern = protocol in {"/meshsub/1.2.0", "/meshsub/1.3.0"}
    extensions = protocol == "/meshsub/1.3.0"
    subscriptions, messages = [], []
    for raw in _repeated(fields, 1):
        item = _fields(raw)
        subscribe = _one(item, 1, 0)
        if subscribe not in (None, 0, 1):
            raise ValueError("invalid PubSub subscription flag")
        subscription = {"topic": _text(item, 2, required=True), "subscribe": bool(subscribe)}
        if extensions:
            subscription.update(requests_partial=_flag(item, 3), supports_partial=_flag(item, 4))
        subscriptions.append(subscription)
    for raw in _repeated(fields, 2):
        item = _fields(raw)
        payload = _one(item, 2, 2, required=True)
        messages.append({
            "topic": _text(item, 4, required=True),
            "author_hex": (_one(item, 1, 2) or b"").hex(),
            "seqno_hex": (_one(item, 3, 2) or b"").hex(),
            "payload_sha256": hashlib.sha256(payload).hexdigest(),
            "payload_bytes": len(payload),
            "signature_hex": (_one(item, 5, 2) or b"").hex(),
        })
    controls = {"ihave": [], "iwant": [], "graft": [], "prune": []}
    if modern:
        controls["idontwant"] = []
    if extensions:
        controls.update(extensions=None, partial=None)
    raw_control = _one(fields, 3, 2)
    if raw_control is not None:
        control = _fields(raw_control)
        for raw in _repeated(control, 1):
            item = _fields(raw)
            controls["ihave"].append({"topic": _text(item, 1, required=True),
                                      "ids_hex": [value.hex() for value in _repeated(item, 2)]})
        for raw in _repeated(control, 2):
            item = _fields(raw)
            controls["iwant"].append({"ids_hex": [value.hex() for value in _repeated(item, 1)]})
        for raw in _repeated(control, 3):
            controls["graft"].append({"topic": _text(_fields(raw), 1, required=True)})
        for raw in _repeated(control, 4):
            item = _fields(raw)
            if protocol == "/meshsub/1.0.0" and (2 in item or 3 in item):
                raise ValueError("v1.0 PRUNE contains PX or backoff, including encoded zero")
            peers = _repeated(item, 2)
            backoff = _one(item, 3, 0)
            controls["prune"].append({"topic": _text(item, 1, required=True),
                                      "has_backoff": backoff is not None, "backoff": backoff,
                                      "has_px": bool(peers), "px_count": len(peers)})
        if modern:
            for raw in _repeated(control, 5):
                ids = _repeated(_fields(raw), 1)
                if any(not value or len(value) > 256 for value in ids):
                    raise ValueError("IDONTWANT message ID outside fixture bounds")
                controls["idontwant"].append({"ids_hex": [value.hex() for value in ids]})
        if extensions:
            advertised = _one(control, 6, 2)
            if advertised is not None:
                controls["extensions"] = {"partial_messages": _flag(_fields(advertised), 10)}
    if extensions:
        partial = _one(fields, 10, 2)
        if partial is not None:
            item = _fields(partial)
            controls["partial"] = {
                "topic": _text(item, 1, required=True),
                "group_hex": _bounded_hex(item, 2, MAX_GROUP_ID, required=True),
                "data_hex": _bounded_hex(item, 3, MAX_FRAME),
                "metadata_hex": _bounded_hex(item, 4, MAX_PARTS_METADATA),
            }
    return {"subscriptions": subscriptions, "messages": messages, **controls}


def validate_rpc_receipt(receipt, protocol, direction):
    if not isinstance(receipt, dict) or direction not in {"read", "write"}:
        raise ValueError("invalid PubSub receipt direction")
    wire = receipt.get("framed_hex")
    if not isinstance(wire, str) or not 0 < len(wire) <= 2 * (MAX_FRAME + 3) \
            or len(wire) % 2 or not re.fullmatch(r"[a-f0-9]+", wire):
        raise ValueError("missing bounded native PubSub bytes")
    data = bytes.fromhex(wire)
    observed = receipt.get(direction)
    if not isinstance(observed, dict) or type(observed.get("framed_bytes")) is not int \
            or len(data) != observed["framed_bytes"] \
            or observed.get("framed_sha256") != hashlib.sha256(data).hexdigest() \
            or observed.get("complete_frames") is not True \
            or observed.get("invalid_or_over_limit") is not False \
            or type(observed.get("frames")) is not int or observed["frames"] != 1:
        raise ValueError("PubSub actual I/O bytes/hash/completion mismatch")
    size, prefix = _varint(data, 0)
    if size > MAX_FRAME or prefix > 3 or len(data) != prefix + size:
        raise ValueError("PubSub framed length mismatch")
    return decode_rpc(data[prefix:], protocol)
