import copy
import hashlib
import unittest

from autorelay_wire import varint
from path_wire import address_bytes, validate_dcutr_frame


def receipt(kind, addresses):
    payload = varint(8) + varint(kind)
    for address in addresses:
        encoded = address_bytes(address)
        payload += varint(18) + varint(len(encoded)) + encoded
    framed = varint(len(payload)) + payload
    return {
        "framed_hex": framed.hex(),
        "read": {
            "framed_bytes": len(framed),
            "framed_sha256": hashlib.sha256(framed).hexdigest(),
            "frames": 1,
            "complete_frames": True,
            "invalid_or_over_limit": False,
        },
    }


class PathWireTests(unittest.TestCase):
    def test_connect_and_sync(self):
        addresses = ["/ip4/127.0.0.1/tcp/4001", "/ip4/127.0.0.1/udp/4002/quic-v1"]
        validate_dcutr_frame(receipt(100, addresses), 100, addresses)
        validate_dcutr_frame(receipt(300, []), 300, [])

    def test_wrong_type_or_address(self):
        addresses = ["/ip4/127.0.0.1/tcp/4001"]
        with self.assertRaises(ValueError):
            validate_dcutr_frame(receipt(300, []), 100, addresses)
        with self.assertRaises(ValueError):
            validate_dcutr_frame(receipt(100, addresses), 100, ["/ip4/127.0.0.1/tcp/4002"])

    def test_nat_and_ipv6_candidates_use_exact_numeric_encoding(self):
        self.assertEqual(address_bytes("/ip4/11.0.0.2/udp/4001/quic-v1"),
                         bytes.fromhex("040b00000291020fa1cd03"))
        self.assertEqual(address_bytes("/ip6/2001:db8::1/tcp/4001"),
                         bytes.fromhex("2920010db8000000000000000000000001060fa1"))
        candidates = ["/ip4/11.0.0.2/udp/4001/quic-v1", "/ip6/2001:db8::1/tcp/4001"]
        validate_dcutr_frame(receipt(100, candidates), 100, candidates)

    def test_invalid_numeric_or_non_direct_candidate_is_rejected(self):
        for value in ("/ip4/0.0.0.0/tcp/4001", "/ip4/224.0.0.1/tcp/4001",
                      "/ip4/127.0.0.1/tcp/65536", "/ip4/127.0.0.1/udp/4001",
                      "/ip4/127.0.0.1/tcp/4001/quic-v1", "/ip6/127.0.0.1/tcp/4001",
                      "/ip6/2001:0DB8::1/tcp/4001", "/dns/example.com/tcp/4001",
                      "/ip4/127.0.0.1/tcp/4001/p2p-circuit", "x" * 513):
            with self.subTest(value=value), self.assertRaises(ValueError):
                address_bytes(value)

    def test_frame_hash_and_length(self):
        original = receipt(300, [])
        for field, value in (("framed_bytes", 0), ("framed_sha256", "0" * 64),
                             ("frames", 2), ("complete_frames", False),
                             ("invalid_or_over_limit", True)):
            modified = copy.deepcopy(original)
            modified["read"][field] = value
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_dcutr_frame(modified, 300, [])

    def test_candidate_bounds_and_sync_shape(self):
        address = "/ip4/127.0.0.1/tcp/4001"
        for kind, addresses in ((100, []), (100, [address, address]),
                                (300, [address]), (100, [address] * 33)):
            with self.subTest(kind=kind, addresses=addresses), self.assertRaises(ValueError):
                validate_dcutr_frame(receipt(kind, addresses), kind, addresses)

    def test_ambiguous_type_is_rejected(self):
        data = b"\x05\x08\x64\x08\xac\x02"
        # Bind the hash to the supplied bytes: malformed shape cannot be hidden
        # behind a matching digest or an asserted successful fixture status.
        forged = receipt(300, [])
        forged["framed_hex"] = data.hex()
        forged["read"]["framed_bytes"] = len(data)
        forged["read"]["framed_sha256"] = hashlib.sha256(data).hexdigest()
        with self.assertRaises(ValueError):
            validate_dcutr_frame(forged, 300, [])

    def test_untyped_or_incomplete_receipts(self):
        for value in (None, {}, {"framed_hex": "02"}, {"framed_hex": "020801", "read": []}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_dcutr_frame(value, 300, [])
        with self.assertRaises(ValueError):
            validate_dcutr_frame(receipt(300, []), 300.0, [])


if __name__ == "__main__":
    unittest.main()
