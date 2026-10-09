"""Synthetic helper regressions, not live extension acceptance."""

from copy import deepcopy
import unittest

from partial_fixture import Metadata, encode_part, group_id, part_bytes
from pubsub_extension_evidence import _rpc, advertisements, native_rpcs, partial_exchange, received_parts
from test_pubsub_evidence import synthetic_case
from test_pubsub_wire import field, receipt

TOKEN = "00112233445566778899aabbccddeeff"
TOPIC = "forge-pr11:" + TOKEN


def rpc(sequence, direction, peer, body, stream="one"):
    wire = receipt(body, direction)
    return _rpc(sequence, wire, "/meshsub/1.3.0", direction, peer, ("connection", stream, direction))


class ExtensionEvidenceTests(unittest.TestCase):
    def test_first_advertisement_is_per_stream_and_direction(self):
        body = field(3, field(6, field(10, 1)))
        views = [rpc(1, "read", "peer", body), rpc(2, "write", "peer", body),
                 rpc(3, "read", "peer", b""), rpc(4, "read", "peer", body, "replacement")]
        self.assertEqual(len(advertisements(views, True, {"peer": True})), 3)
        for changed in (views[:1], [*views, rpc(5, "read", "peer", body)],
                        [rpc(1, "read", "peer", b""), views[1]],
                        [views[0], rpc(2, "write", "foreign", body)]):
            with self.assertRaises(ValueError):
                advertisements(changed, True, {"peer": True})

    def test_absent_support_never_becomes_partial_support(self):
        empty = [rpc(1, "read", "peer", b""), rpc(2, "write", "peer", b"")]
        self.assertEqual(len(advertisements(empty, False, {"peer": False})), 2)
        with self.assertRaises(ValueError):
            advertisements(empty, True, {"peer": True})

    def test_parts_require_matching_real_frame_bytes_not_a_completion_flag(self):
        source, target = [], []
        for index in range(3):
            part = field(1, TOPIC.encode()) + field(2, group_id(TOKEN))
            part += field(3, encode_part(index, part_bytes(TOKEN, index)))
            part += field(4, Metadata(1, 7, 0).encode())
            source.append(rpc(index + 1, "write", "target", field(10, part)))
            target.append(rpc(index + 1, "read", "source", field(10, part)))
        value = received_parts(source, target, "source", "target", TOPIC, TOKEN)
        self.assertEqual(len(value["parts"]), 3)
        self.assertEqual(value["payload_hex"], b"".join(part_bytes(TOKEN, i) for i in range(3)).hex())
        for left, right in ((source[:2], target), (source, target[:2]), (target, source)):
            with self.assertRaises(ValueError):
                received_parts(left, right, "source", "target", TOPIC, TOKEN)
        wrong = [rpc(1, "read", "source", b""), *target[1:]]
        with self.assertRaises(ValueError):
            received_parts(source, wrong, "source", "target", TOPIC, TOKEN)

    def test_native_owners_are_not_inferred_from_helper_configuration(self):
        case = synthetic_case(lower_quic=False, version="1.3")
        events = case["raw"]["victim"]["events"]
        self.assertTrue(native_rpcs(events, "forge", "/meshsub/1.3.0", "tcp"))
        for mutation in (lambda value: value.update(protocol="/meshsub/1.2.0"),
                         lambda value: value.update(connection_id="missing"),
                         lambda value: value.update(source="go.pubsub.native_stream.read")):
            changed = deepcopy(events)
            mutation(next(event for event in changed if event["kind"] == "rpc"))
            with self.assertRaises(ValueError):
                native_rpcs(changed, "forge", "/meshsub/1.3.0", "tcp")

    def test_metadata_request_must_precede_parts_on_both_actors(self):
        def partial(metadata, index=None):
            value = field(1, TOPIC.encode()) + field(2, group_id(TOKEN)) + field(4, metadata.encode())
            if index is not None:
                value += field(3, encode_part(index, part_bytes(TOKEN, index)))
            return field(10, value)

        offer, request = partial(Metadata(1, 7, 0)), partial(Metadata(1, 0, 7))
        provider = [rpc(10, "write", "consumer", offer), rpc(20, "read", "consumer", request)]
        consumer = [rpc(3, "read", "provider", offer), rpc(4, "write", "provider", request)]
        for index in range(3):
            body = partial(Metadata(1, 7, 0), index)
            provider.append(rpc(30 + index, "write", "consumer", body))
            consumer.append(rpc(5 + index, "read", "provider", body))
        value = partial_exchange(provider, consumer, "provider", "consumer", TOPIC, TOKEN)
        self.assertEqual(value["request_read"], 20)
        for left, right in ((provider[1:], consumer), (provider, consumer[1:]),
                            ([provider[0], rpc(90, "read", "consumer", request), *provider[2:]], consumer),
                            (provider, [rpc(90, "read", "provider", offer), *consumer[1:]])):
            with self.assertRaises(ValueError):
                partial_exchange(left, right, "provider", "consumer", TOPIC, TOKEN)

    def test_validated_wire_snapshot_does_not_borrow_mutable_source(self):
        case = synthetic_case(lower_quic=False, version="1.3")
        events = case["raw"]["victim"]["events"]
        views = native_rpcs(events, "forge", "/meshsub/1.3.0", "tcp")
        first = views[0]
        original = first.frame
        event = events[first.sequence - 1]
        event["receipt"]["framed_hex"] = "00"
        event["sequence"] = 999
        event["peer_id"] = "foreign"
        self.assertEqual(first.frame, original)
        self.assertNotEqual(first.frame, b"\x00")
        self.assertNotEqual(first.sequence, event["sequence"])
        self.assertNotEqual(first.peer, event["peer_id"])
        with self.assertRaises(TypeError):
            first.value["extensions"] = {"partial_messages": 1}


if __name__ == "__main__":
    unittest.main()
