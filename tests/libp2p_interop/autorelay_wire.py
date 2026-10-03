"""Bounded Identify Push inspection, reusing existing wire/identity primitives."""

import hashlib
import re

from rust_upgrade_evidence import _peer, _varint


def varint(value):
    out = bytearray()
    while value >= 128:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)


def _fields(data):
    fields, offset = {}, 0
    while offset < len(data):
        tag, offset = _varint(data, offset)
        field, wire = tag >> 3, tag & 7
        if field == 0:
            raise ValueError("zero protobuf field")
        if wire == 0:
            value, offset = _varint(data, offset)
        elif wire in (1, 2, 5):
            if wire == 2:
                size, offset = _varint(data, offset)
            else:
                size = 8 if wire == 1 else 4
            value, offset = data[offset:offset + size], offset + size
            if len(value) != size:
                raise ValueError("truncated Push protobuf field")
        else:
            raise ValueError("unsupported Push protobuf wire type")
        fields.setdefault(field, []).append((wire, value))
    return fields


def address_bytes(value):
    """Only the numeric loopback grammar owned by this static fixture matrix."""
    if not isinstance(value, str) or len(value) > 512:
        raise ValueError("invalid Push multiaddr")
    match = re.fullmatch(r"/ip4/127\.0\.0\.1/(tcp|udp)/([1-9][0-9]{0,4})(/quic-v1)?"
                         r"(?:/p2p/([^/]+))?(?:/p2p-circuit/p2p/([^/]+))?", value)
    if not match:
        raise ValueError("Push address outside static numeric topology")
    transport, port, quic, peer, target = match.groups()
    if int(port) > 65535 or bool(quic) != (transport == "udp") or target and not peer:
        raise ValueError("Push address transport/peer mismatch")
    out = b"\x04\x7f\x00\x00\x01" + varint(6 if transport == "tcp" else 273) + int(port).to_bytes(2, "big")
    if quic:
        out += varint(461)
    if peer:
        encoded = _peer(peer)
        out += varint(421) + varint(len(encoded)) + encoded
    if target:
        encoded = _peer(target)
        out += varint(290) + varint(421) + varint(len(encoded)) + encoded
    return out


def _payload(receipt):
    wire = receipt.get("framed_hex")
    if not isinstance(wire, str) or not 0 < len(wire) <= 8196 or len(wire) % 2 or not re.fullmatch(r"[a-f0-9]+", wire):
        raise ValueError("missing bounded native Push bytes")
    data = bytes.fromhex(wire)
    body = receipt["read"]
    if type(body.get("framed_bytes")) is not int or len(data) != body["framed_bytes"] \
            or body.get("complete_frames") is not True or body.get("invalid_or_over_limit") is not False \
            or type(body.get("frames")) is not int or body["frames"] != 1 \
            or hashlib.sha256(data).hexdigest() != body["framed_sha256"]:
        raise ValueError("native Push bytes/hash mismatch")
    size, prefix = _varint(data, 0)
    if not 0 < size <= 4096 or len(data) != prefix + size:
        raise ValueError("native Push frame length mismatch")
    return _fields(data[prefix:])


def validate_push_frame(receipt, peer, addresses):
    fields = _payload(receipt)
    keys = fields.get(1)
    if not keys or len(keys) != 1 or keys[0][0] != 2:
        raise ValueError("native Push key missing/ambiguous")
    key = keys[0][1]
    public = _fields(key)
    if public.get(1) != [(0, 1)] or len(public.get(2, [])) != 1 or public[2][0][0] != 2 or len(public[2][0][1]) != 32:
        raise ValueError("native Push does not carry fixture Ed25519 identity")
    if b"\x00" + varint(len(key)) + key != _peer(peer):
        raise ValueError("native Push key differs from authenticated peer")
    if fields.get(2, []) != [(2, address_bytes(address)) for address in addresses]:
        raise ValueError("native Push bytes differ from consumed address snapshot")


def validate_reservation_frame(receipt, relay, peer):
    fields = _payload(receipt)
    if fields.get(1) != [(0, 2)] or fields.get(5) != [(0, 100)] or len(fields.get(3, [])) != 1 or fields[3][0][0] != 2:
        raise ValueError("native HOP frame is not successful reservation STATUS")
    reservation = _fields(fields[3][0][1])
    expiry = reservation.get(1, [])
    if len(expiry) != 1 or expiry[0][0] != 0 or receipt.get("expires_unix_ms") != expiry[0][1] * 1000:
        raise ValueError("native reservation expiry differs from wire")
    if reservation.get(2) != [(2, address_bytes(address)) for address in receipt["addresses"]]:
        raise ValueError("native reservation addresses differ from wire")
    vouchers = reservation.get(3, [])
    if type(receipt.get("voucher")) is not bool or receipt["voucher"] != bool(vouchers):
        raise ValueError("voucher presence differs from native wire")
    if not vouchers:
        return
    if len(vouchers) != 1 or vouchers[0][0] != 2:
        raise ValueError("ambiguous voucher bytes")
    envelope = _fields(vouchers[0][1])
    keys = envelope.get(1, [])
    if len(keys) != 1 or keys[0][0] != 2 or envelope.get(2) != [(2, b"\x03\x02")] \
            or len(envelope.get(3, [])) != 1 or envelope[3][0][0] != 2 \
            or len(envelope.get(5, [])) != 1 or envelope[5][0][0] != 2 or len(envelope[5][0][1]) != 64:
        raise ValueError("signed voucher envelope shape/payload type mismatch")
    key = keys[0][1]
    if b"\x00" + varint(len(key)) + key != _peer(relay):
        raise ValueError("voucher signing key differs from authenticated relay")
    signed = _fields(envelope[3][0][1])
    if signed.get(1) != [(2, _peer(relay))] or signed.get(2) != [(2, _peer(peer))] or signed.get(3) != expiry:
        raise ValueError("signed voucher payload does not bind reservation")
    if receipt.get("voucher_validated") is not True \
            or receipt.get("voucher_validation_basis") != "pinned_SignedEnvelope_signature_domain_payload_signer_peer_expiry" \
            or receipt.get("voucher_relay") != relay or receipt.get("voucher_peer") != peer \
            or receipt.get("voucher_expiration") != expiry[0][1]:
        raise ValueError("signed voucher lacks pinned donor verification receipt")
