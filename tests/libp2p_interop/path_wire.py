"""Bounded DCUtR wire inspection for observed, not configured, exchanges."""

import ipaddress
import re

from autorelay_wire import _payload, varint
from rust_upgrade_evidence import _peer


def address_bytes(value):
    """Encode direct numeric candidates without importing loopback fixture policy."""
    if not isinstance(value, str) or len(value) > 512:
        raise ValueError("invalid DCUtR multiaddr")
    match = re.fullmatch(r"/(ip4|ip6)/([^/]+)/(tcp|udp)/([1-9][0-9]{0,4})(/quic-v1)?"
                         r"(?:/p2p/([^/]+))?", value)
    if not match:
        raise ValueError("DCUtR address outside bounded direct numeric grammar")
    family, host, transport, port, quic, peer = match.groups()
    address = ipaddress.ip_address(host)
    if address.version != (4 if family == "ip4" else 6) or str(address) != host \
            or address.is_unspecified or address.is_multicast or int(port) > 65535 \
            or bool(quic) != (transport == "udp"):
        raise ValueError("noncanonical DCUtR candidate")
    out = varint(4 if address.version == 4 else 41) + address.packed
    out += varint(6 if transport == "tcp" else 273) + int(port).to_bytes(2, "big")
    if quic:
        out += varint(461)
    if peer:
        encoded = _peer(peer)
        out += varint(421) + varint(len(encoded)) + encoded
    return out


def validate_dcutr_frame(receipt, kind, addresses):
    if type(kind) is not int or kind not in (100, 300) or not isinstance(receipt, dict):
        raise ValueError("invalid DCUtR receipt/type")
    if not isinstance(receipt.get("read"), dict):
        raise ValueError("missing DCUtR frame read receipt")
    if not isinstance(addresses, list) or len(addresses) > 32:
        raise ValueError("invalid DCUtR candidate bounds")
    if (kind == 100 and not addresses) or (kind == 300 and addresses):
        raise ValueError("invalid DCUtR CONNECT/SYNC candidates")
    if any(not isinstance(address, str) or "/p2p-circuit" in address for address in addresses):
        raise ValueError("DCUtR candidate is not a direct address")
    if len(set(addresses)) != len(addresses):
        raise ValueError("duplicate DCUtR candidates")
    try:
        fields = _payload(receipt)
    except (KeyError, TypeError, OverflowError) as error:
        raise ValueError("incomplete DCUtR frame receipt") from error
    if fields.get(1) != [(0, kind)]:
        raise ValueError("DCUtR wire type differs from observed transition")
    if fields.get(2, []) != [(2, address_bytes(address)) for address in addresses]:
        raise ValueError("DCUtR wire addresses differ from observed candidates")
