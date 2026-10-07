import copy
import hashlib
import unittest

from autorelay_wire import varint
from pubsub_wire import MAX_FRAME, decode_rpc, validate_rpc_receipt


def field(number, value):
    if isinstance(value, int):
        return varint(number << 3) + varint(value)
    return varint(number << 3 | 2) + varint(len(value)) + value


def receipt(body, direction="read"):
    framed = varint(len(body)) + body
    return {"framed_hex": framed.hex(), direction: {
        "framed_bytes": len(framed), "framed_sha256": hashlib.sha256(framed).hexdigest(),
        "frames": 1, "complete_frames": True, "invalid_or_over_limit": False}}


class PubSubWireTests(unittest.TestCase):
    def test_native_prune_topic_only_is_v10(self):
        body = field(3, field(4, field(1, b"topic")))
        value = validate_rpc_receipt(receipt(body), "/meshsub/1.0.0", "read")
        self.assertEqual(value["prune"], [{"topic": "topic", "has_backoff": False,
                                          "backoff": None, "has_px": False, "px_count": 0}])

    def test_v10_rejects_present_zero_and_px(self):
        for addition in (field(3, 0), field(3, 60), field(2, b"")):
            body = field(3, field(4, field(1, b"topic") + addition))
            with self.assertRaises(ValueError):
                validate_rpc_receipt(receipt(body), "/meshsub/1.0.0", "read")

    def test_v11_preserves_actual_backoff_presence(self):
        body = field(3, field(4, field(1, b"topic") + field(3, 0)))
        value = validate_rpc_receipt(receipt(body, "write"), "/meshsub/1.1.0", "write")
        self.assertTrue(value["prune"][0]["has_backoff"])
        self.assertEqual(value["prune"][0]["backoff"], 0)

    def test_message_receipt_binds_content_author_and_sequence(self):
        payload = b"unique-token"
        message = field(1, b"peer") + field(2, payload) + field(3, b"sequence") + field(4, b"topic")
        result = validate_rpc_receipt(receipt(field(2, message)), "/meshsub/1.1.0", "read")
        self.assertEqual(result["messages"][0]["payload_sha256"], hashlib.sha256(payload).hexdigest())
        self.assertEqual(result["messages"][0]["author_hex"], b"peer".hex())
        self.assertEqual(result["messages"][0]["seqno_hex"], b"sequence".hex())

    def test_all_four_control_types(self):
        control = field(1, field(1, b"topic") + field(2, b"id"))
        control += field(2, field(1, b"id")) + field(3, field(1, b"topic"))
        control += field(4, field(1, b"topic"))
        value = decode_rpc(field(3, control), "/meshsub/1.1.0")
        self.assertEqual(value["ihave"][0]["ids_hex"], [b"id".hex()])
        self.assertEqual(value["iwant"][0]["ids_hex"], [b"id".hex()])
        self.assertEqual(value["graft"], [{"topic": "topic"}])

    def test_receipt_is_not_a_configured_or_partial_claim(self):
        body = field(3, field(4, field(1, b"topic")))
        valid = receipt(body)
        mutations = [
            lambda item: item["read"].update(framed_sha256="0" * 64),
            lambda item: item["read"].update(framed_bytes=1),
            lambda item: item["read"].update(complete_frames=False),
            lambda item: item["read"].update(frames=True),
            lambda item: item["read"].update(invalid_or_over_limit=True),
            lambda item: item.update(framed_hex=item["framed_hex"] + "00"),
        ]
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                item = copy.deepcopy(valid)
                mutation(item)
                with self.assertRaises(ValueError):
                    validate_rpc_receipt(item, "/meshsub/1.1.0", "read")
        with self.assertRaises(ValueError):
            validate_rpc_receipt(valid, "/meshsub/1.1.0", "write")
        with self.assertRaises(ValueError):
            validate_rpc_receipt(valid, "/meshsub/1.3.0", "read")

    def test_malformed_duplicate_and_oversized_fields_fail_closed(self):
        examples = [b"\x00", b"\x1a\x80", b"\x1a\x02\x0a", field(3, b"") * 2,
                    field(2, field(4, b"topic") + field(2, b"a") + field(2, b"b")),
                    b"a" * (MAX_FRAME + 1)]
        for body in examples:
            with self.subTest(body=body[:16]):
                with self.assertRaises(ValueError):
                    decode_rpc(body, "/meshsub/1.1.0")


if __name__ == "__main__":
    unittest.main()
