"""PR12 application format, independent of the GossipSub router and protobuf codec."""

from dataclasses import dataclass
import re
import struct


PARTS = 3
MAX_PART = 256
MASK = (1 << PARTS) - 1


def _token(value):
    if not isinstance(value, str) or re.fullmatch(r"[0-9a-f]{32}", value) is None:
        raise ValueError("partial fixture requires a canonical case token")
    return bytes.fromhex(value)


def group_id(token, sequence=1):
    """Available before any content: case namespace plus application sequence."""
    if type(sequence) is not int or not 1 <= sequence <= 0xffffffff:
        raise ValueError("partial fixture group sequence is out of range")
    return _token(token) + struct.pack(">I", sequence)


def part_bytes(token, index):
    _token(token)
    if type(index) is not int or not 0 <= index < PARTS:
        raise ValueError("partial fixture index is out of range")
    return f"forge-pr12:{token}:part-{index}".encode("ascii")


def encode_part(index, data):
    if (type(index) is not int or not 0 <= index < PARTS or not isinstance(data, bytes)
            or not 1 <= len(data) <= MAX_PART):
        raise ValueError("invalid partial fixture part")
    return struct.pack(">BBH", 1, index, len(data)) + data


def decode_part(encoded):
    if not isinstance(encoded, bytes) or not 5 <= len(encoded) <= MAX_PART + 4:
        raise ValueError("partial fixture part size is out of range")
    version, index, size = struct.unpack(">BBH", encoded[:4])
    if version != 1 or index >= PARTS or size != len(encoded) - 4:
        raise ValueError("invalid partial fixture part header")
    return index, encoded[4:]


@dataclass(frozen=True)
class Metadata:
    revision: int
    have: int
    want: int

    def encode(self):
        if (type(self.revision) is not int or not 1 <= self.revision <= 0xffffffff
                or type(self.have) is not int or not 0 <= self.have <= MASK
                or type(self.want) is not int or not 0 <= self.want <= MASK
                or self.have & self.want):
            raise ValueError("invalid partial fixture metadata")
        return struct.pack(">BIBB", 1, self.revision, self.have, self.want)

    @classmethod
    def decode(cls, encoded):
        if not isinstance(encoded, bytes) or len(encoded) != 7:
            raise ValueError("partial fixture metadata must have seven bytes")
        version, revision, have, want = struct.unpack(">BIBB", encoded)
        if version != 1:
            raise ValueError("unknown partial fixture metadata version")
        value = cls(revision, have, want)
        value.encode()
        return value


def reconstruct(token, parts):
    """Check all parts, not only a donor's callback or a claimed completed flag."""
    _token(token)
    if not isinstance(parts, (tuple, list)) or len(parts) != PARTS:
        raise ValueError("partial fixture requires exactly three received parts")
    decoded = {}
    for encoded in parts:
        index, data = decode_part(encoded)
        if index in decoded or data != part_bytes(token, index):
            raise ValueError("duplicate or corrupt partial fixture part")
        decoded[index] = data
    return b"".join(decoded[index] for index in range(PARTS))
