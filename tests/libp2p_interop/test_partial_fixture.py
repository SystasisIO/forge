import unittest

from partial_fixture import Metadata, decode_part, encode_part, group_id, part_bytes, reconstruct


class PartialFixtureTests(unittest.TestCase):
    token = "00112233445566778899aabbccddeeff"

    def test_golden_group_is_content_independent(self):
        self.assertEqual(group_id(self.token).hex(), "00112233445566778899aabbccddeeff00000001")
        self.assertNotEqual(group_id(self.token), group_id(self.token, 2))

    def test_golden_metadata(self):
        self.assertEqual(Metadata(0x01020304, 5, 2).encode().hex(), "01010203040502")
        self.assertEqual(Metadata.decode(bytes.fromhex("01000000020007")), Metadata(2, 0, 7))

    def test_golden_part(self):
        self.assertEqual(encode_part(2, b"abc").hex(), "01020003616263")
        self.assertEqual(decode_part(bytes.fromhex("01010003616263")), (1, b"abc"))

    def test_reconstruction_requires_distinct_verified_parts(self):
        parts = [encode_part(i, part_bytes(self.token, i)) for i in range(3)]
        expected = b"".join(part_bytes(self.token, i) for i in range(3))
        self.assertEqual(reconstruct(self.token, parts[::-1]), expected)
        for bad in (parts[:2], parts + parts[:1], [parts[0]] * 3,
                    [parts[0], parts[1], encode_part(2, b"corrupt")]):
            with self.subTest(parts=bad), self.assertRaises(ValueError):
                reconstruct(self.token, bad)

    def test_part_bounds_and_header(self):
        for bad in (b"", b"\x01\x00\x00\x00", b"\x02\x00\x00\x01x",
                    b"\x01\x03\x00\x01x", b"\x01\x00\x00\x02x",
                    b"\x01\x00\x01\x01" + b"x" * 257):
            with self.subTest(encoded=bad), self.assertRaises(ValueError):
                decode_part(bad)
        for index, data in ((True, b"a"), (-1, b"a"), (3, b"a"), (0, b""), (0, b"x" * 257), (0, "x")):
            with self.subTest(index=index, data=data), self.assertRaises(ValueError):
                encode_part(index, data)

    def test_metadata_rejects_ambiguous_or_unbounded_state(self):
        for value in (Metadata(0, 0, 0), Metadata(1 << 32, 0, 0), Metadata(True, 0, 0),
                      Metadata(1, 8, 0), Metadata(1, 0, 8), Metadata(1, 1, 1), Metadata(1, -1, 0)):
            with self.subTest(value=value), self.assertRaises(ValueError):
                value.encode()
        for bad in (b"", bytes.fromhex("02000000010007"), bytes.fromhex("01000000000102"),
                    bytes.fromhex("01000000010101"), bytes.fromhex("01000000010800"),
                    bytes.fromhex("0100000001000700")):
            with self.subTest(encoded=bad), self.assertRaises(ValueError):
                Metadata.decode(bad)

    def test_group_rejects_ambiguous_identifiers(self):
        for bad in ("", self.token.upper(), "x" * 32, "0" * 31, None):
            with self.subTest(token=bad), self.assertRaises(ValueError):
                group_id(bad)
        for sequence in (0, -1, True, 1 << 32):
            with self.subTest(sequence=sequence), self.assertRaises(ValueError):
                group_id(self.token, sequence)


if __name__ == "__main__":
    unittest.main()
