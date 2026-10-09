"""PR12 extension wire evidence layered on the existing native owner checks.

These helpers prove only their named wire properties. Process provenance, actual
joined shutdown and application validation remain the enclosing case's duties.
"""

from dataclasses import dataclass
from types import MappingProxyType
from collections.abc import Mapping

from partial_fixture import Metadata, group_id, reconstruct
from pubsub_evidence import SOURCES, _owner, _rpc_peer, require
from pubsub_wire import validate_rpc_receipt


@dataclass(frozen=True)
class Rpc:
    sequence: int
    frame: bytes
    value: Mapping
    peer: str
    stream: tuple


def _immutable(value):
    if isinstance(value, dict):
        return MappingProxyType({key: _immutable(item) for key, item in value.items()})
    if isinstance(value, list):
        return tuple(_immutable(item) for item in value)
    return value


def _rpc(sequence, wire, protocol, direction, peer, stream):
    value = validate_rpc_receipt(wire, protocol, direction)
    return Rpc(sequence, bytes.fromhex(wire["framed_hex"]), _immutable(value), peer, tuple(stream))


def native_rpcs(events, implementation, protocol, transport, fingerprint=None):
    """Retain exact frames and per-direction stream generations, never topic status."""
    result = []
    require(isinstance(events, list) and len(events) <= 2048, "unbounded extension events")
    for sequence, event in enumerate(events, 1):
        require(isinstance(event, dict) and type(event.get("sequence")) is int
                and event["sequence"] == sequence, "invalid extension event ordering")
        if event.get("kind") != "rpc":
            continue
        require(event.get("source") in SOURCES[implementation]["rpc"], "foreign extension RPC authority")
        direction = event.get("direction")
        require(direction in {"read", "write"}, "invalid extension RPC direction")
        if implementation == "go":
            prefix = "go.quic.native_stream." if transport == "quic" else "go.pubsub.native_stream."
            require(event["source"] == prefix + direction, "extension RPC carrier/source differs")
        peer = _rpc_peer(event)
        _owner(events, event, peer, protocol, transport, fingerprint)
        if event["source"].startswith("go.quic."):
            stream = (event["native_connection_id"], event["native_stream_id"], direction)
        else:
            stream = (event["connection_id"], event["stream_id"], direction)
        result.append(_rpc(sequence, event.get("receipt"), protocol, direction, peer, stream))
    require(bool(result), "extension evidence has no native RPC")
    return result


def advertisements(rpcs, local_support, remote_support):
    """Each actual v1.3 stream starts anew, including when peers reconnect."""
    require(type(local_support) is bool and isinstance(remote_support, dict)
            and remote_support and all(type(value) is bool for value in remote_support.values()),
            "missing exact advertisement expectations")
    first, directions = {}, set()
    for rpc in rpcs:
        require(rpc.peer in remote_support and "extensions" in rpc.value,
                "unexpected peer or non-v1.3 RPC in advertisement case")
        direction = rpc.stream[-1]
        require(direction in {"read", "write"}, "unknown advertisement direction")
        key = (rpc.peer, *rpc.stream)
        advertised = rpc.value["extensions"]
        if key in first:
            require(advertised is None, "extension advertised after first RPC of the same stream")
            continue
        expected = local_support if direction == "write" else remote_support[rpc.peer]
        actual = advertised is not None and advertised.get("partial_messages") == 1
        require(actual is expected, "first RPC lacks its exact local/remote extension support")
        first[key] = rpc.sequence
        directions.add((rpc.peer, direction))
    require(directions == {(peer, direction) for peer in remote_support for direction in ("read", "write")},
            "advertisement lacks actual incoming/outgoing first RPCs")
    return first


def _partial_pairs(source, target, source_peer, target_peer, topic, group):
    sent = [rpc for rpc in source if rpc.peer == target_peer and rpc.stream[-1] == "write"]
    received = [rpc for rpc in target if rpc.peer == source_peer and rpc.stream[-1] == "read"]
    pairs = []
    for outgoing in sent:
        value = outgoing.value.get("partial")
        if value is None or value["topic"] != topic or value["group_hex"] != group:
            continue
        for incoming in received:
            if outgoing.frame == incoming.frame:
                pairs.append((outgoing, incoming, value))
    return pairs


def received_parts(source, target, source_peer, target_peer, topic, token):
    """Reconstruct only independently checked bytes with matched real writes/reads."""
    from partial_fixture import decode_part

    require(source_peer != target_peer, "partial exchange is a self-query")
    group = group_id(token).hex()
    parts, evidence = {}, []
    for sent, received, value in _partial_pairs(source, target, source_peer, target_peer, topic, group):
        if value["metadata_hex"] is not None:
            Metadata.decode(bytes.fromhex(value["metadata_hex"]))
        if value["data_hex"] is None:
            continue
        body = bytes.fromhex(value["data_hex"])
        index, _ = decode_part(body)
        require(index not in parts or parts[index] == body, "conflicting part bytes in native receipts")
        if index in parts:
            continue
        parts[index] = body
        evidence.append({"index": index, "write_sequence": sent.sequence,
                         "read_sequence": received.sequence, "encoded_hex": value["data_hex"]})
    payload = reconstruct(token, list(parts.values()))
    return {"group_id_hex": group, "parts": sorted(evidence, key=lambda item: item["index"]),
            "payload_hex": payload.hex()}


def partial_exchange(provider, consumer, provider_peer, consumer_peer, topic, token):
    """Prove off-mesh group metadata -> request -> three received parts.

    Compare event order only within one actor. Different process clocks and event
    sequences are never used as a shared causal clock.
    """
    group = group_id(token).hex()
    offers = [(sent, received, Metadata.decode(bytes.fromhex(value["metadata_hex"])))
              for sent, received, value in _partial_pairs(provider, consumer, provider_peer, consumer_peer, topic, group)
              if value["data_hex"] is None and value["metadata_hex"] is not None]
    requests = [(sent, received, Metadata.decode(bytes.fromhex(value["metadata_hex"])))
                for sent, received, value in _partial_pairs(consumer, provider, consumer_peer, provider_peer, topic, group)
                if value["data_hex"] is None and value["metadata_hex"] is not None]
    chains = [(offer, request) for offer in offers for request in requests
              if offer[2].have == 7 and offer[2].want == 0 and request[2].have == 0 and request[2].want == 7
              and offer[1].sequence < request[0].sequence and offer[0].sequence < request[1].sequence]
    require(bool(chains), "partial exchange lacks matched metadata then remote request")
    for offer, request in chains:
        try:
            value = received_parts([rpc for rpc in provider if rpc.sequence > request[1].sequence],
                                   [rpc for rpc in consumer if rpc.sequence > request[0].sequence],
                                   provider_peer, consumer_peer, topic, token)
            return {**value, "metadata_write": offer[0].sequence, "metadata_read": offer[1].sequence,
                    "request_write": request[0].sequence, "request_read": request[1].sequence}
        except ValueError:
            continue
    raise ValueError("parts lack the preceding matched remote request")
