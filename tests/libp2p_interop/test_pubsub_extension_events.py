"""Synthetic schema regressions only, not actor execution or native wire proof."""

from copy import deepcopy
import hashlib
import unittest

from partial_fixture import Metadata, encode_part, group_id, part_bytes
from pubsub_extension_events import SCHEMAS, validate_extension_event
from pubsub_evidence import SOURCES, _events
from rust_upgrade_evidence import BASE58, _peer


TOKEN = "00112233445566778899aabbccddeeff"
TOPIC = "forge-pr11:" + TOKEN
GROUP = group_id(TOKEN).hex()
META = Metadata(1, 7, 0).encode().hex()
PARTS = [encode_part(i, part_bytes(TOKEN, i)).hex() for i in range(3)]
PAYLOAD = b"".join(part_bytes(TOKEN, i) for i in range(3))


def peer_id(index):
    value, result = int.from_bytes(b"\x12\x20" + bytes([index]) * 32, "big"), ""
    while value:
        value, digit = divmod(value, 58)
        result = BASE58[digit] + result
    return result


PEER, LOCAL = peer_id(2), peer_id(1)
MESSAGE = {
    "propagation_peer": PEER, "author_peer": PEER, "topic": TOPIC,
    "message_id": (_peer(PEER) + b"\0" * 7 + b"\1").hex(),
    "seqno_hex": "0000000000000001", "payload_sha256": "a" * 64,
}
GO_MESSAGE = MESSAGE | {"author_hex": _peer(PEER).hex(), "payload_bytes": 2048,
                         "message_id_observed": True, "message_id_basis": "native_pubsub_Message.ID"}
FORGE_MESSAGE = MESSAGE | {"payload_bytes": 2048,
                            "message_id_basis": "native_pubsub_codec_message_id_from_callback_message"}
LOCAL_STATE = {"group_id_hex": GROUP, "revision": 1, "have": 7, "want": 0}
RUST_STATE = LOCAL_STATE | {"received_have": 0, "metadata_hex": META,
                            "group_basis": "case_token_and_fixed_sequence_one_not_content_hash"}
RECONSTRUCTED = {"group_id_hex": GROUP, "payload_hex": PAYLOAD.hex(), "payload_bytes": len(PAYLOAD),
                 "payload_sha256": hashlib.sha256(PAYLOAD).hexdigest(),
                 "authority": "application_reconstruction_not_router_delivery_or_signature"}


def raw_actor(implementation, mode="partial"):
    raw = {"schema_version": 1, "implementation": implementation, "actor": "victim", "case_token": TOKEN,
            "local_peer_id": LOCAL, "extension": mode, "version": "1.2" if mode == "idontwant" else "1.3",
            "requests_partial": mode == "partial", "finalized": True, "joined": True, "overflow": False,
            "error": None, "events": []}
    if implementation == "forge":
        raw["extension_capture_error"] = None
    return raw


def row(implementation, kind, source, fields, mode="partial"):
    event = {"sequence": 30, "mono_ns": 30, "kind": kind, "source": source, **fields}
    if implementation == "go":
        event.update(extension_mode=mode, topic=TOPIC)
    return raw_actor(implementation, mode), event


def examples():
    """Hand-written shapes from the native actors' actual emit sites."""
    yield row("go", "validation_hold_armed", "go.fixture.extensions.command", {
        "command_sequence": 1, "payload_sha256": "a" * 64, "payload_bytes": 2048}, "idontwant")
    yield row("go", "validation_release", "go.fixture.extensions.command", {
        "command_sequence": 2, "held_observation_sequence": 3}, "idontwant")
    yield row("go", "validation_held", "go.fixture.extensions.ValidatorEx", GO_MESSAGE | {
        "command_sequence": 1, "committed": False}, "idontwant")
    yield row("go", "validation_resumed", "go.fixture.extensions.ValidatorEx", GO_MESSAGE | {
        "held_observation_sequence": 3, "committed": False, "released": True}, "idontwant")
    yield row("go", "partial_incoming", "go.pubsub.partialmessages.OnIncomingRPC", {
        "peer_id": PEER, "group_id_hex": GROUP, "body_present": False, "body_hex": "",
        "metadata_present": True, "metadata_hex": META,
        "authority": "native_application_callback_not_validation_or_delivery"})
    yield row("go", "partial_gossip", "go.pubsub.partialmessages.OnEmitGossip", {
        "group_id_hex": GROUP, "peer_ids": [PEER], "authority": "native_gossip_recipient_callback_not_wire_send"})
    yield row("go", "partial_offer", "go.fixture.extensions.application", LOCAL_STATE | {
        "group_basis": "case_token_and_sequence_not_content_hash"})
    yield row("go", "partial_rejected", "go.fixture.extensions.application", {
        "observation_sequence": 3, "peer_id": PEER, "group_id_hex": GROUP, "error": "invalid metadata",
        "scope": "application_only_no_router_validation_result"})
    yield row("go", "partial_applied", "go.fixture.extensions.application", {
        "observation_sequence": 3, "peer_id": PEER, "group_id_hex": GROUP, "metadata_hex": META,
        "body_applied": True, "authority": "application_checked_bytes_not_signed_message_validation"})
    yield row("go", "partial_reconstructed", "go.fixture.extensions.application", RECONSTRUCTED | {
        "observation_sequence": 5, "part_observation_sequences": [3, 4, 5]})
    yield row("go", "partial_publish_return", "go.pubsub.PublishPartial", {
        "group_id_hex": GROUP, "observation_sequence": 3, "command_sequence": 0, "error": None})
    yield row("go", "partial_publish_action", "go.pubsub.PublishPartial.actions", {
        "peer_id": PEER, "group_id_hex": GROUP, "observation_sequence": 3, "command_sequence": 0,
        "requests_partial": True, "gossip_metadata_only": False, "body_present": True, "body_hex": PARTS[0],
        "metadata_hex": META, "authority": "application_submission_not_wire_write"})
    yield row("go", "extension_stopped", "go.fixture.extensions.owned_worker_join", {
        "admission_closed": True, "worker_started": True, "worker_joined": True, "pending_work": 0,
        "active_validators": 0, "active_publish_callbacks": 0,
        "scope": "fixture_application_worker_and_admitted_callbacks_not_router_goroutines"})
    yield row("rust", "validation_hold_armed", "rust.fixture.extensions.application", {
        "command_sequence": 1, "payload_sha256": "a" * 64, "payload_bytes": 2048,
        "maximum_lifetime_ms": 10000, "committed": False}, "idontwant")
    yield row("rust", "validation_held", "rust.fixture.extensions.application", MESSAGE | {
        "command_sequence": 1, "committed": False}, "idontwant")
    yield row("rust", "validation_release_requested", "rust.fixture.extensions.application", MESSAGE | {
        "hold_command_sequence": 1, "command_sequence": 2, "cancellation": None,
        "validation_commit": False}, "idontwant")
    for kind, command, cancellation in (("validation_released", 2, None),
                                         ("validation_cancelled", None, "application_shutdown")):
        yield row("rust", kind, "rust.fixture.extensions.application", MESSAGE | {
            "hold_command_sequence": 1, "command_sequence": command, "cancellation": cancellation,
            "validation_commit": True, "error": None}, "idontwant")
    yield row("rust", "validation_not_committed", "rust.libp2p.gossipsub.public-Behaviour", MESSAGE | {
        "outcome": "ignore", "report_message_validation_result": False, "validation_commit": False})
    yield row("rust", "partial_incoming", "rust.libp2p.gossipsub.Event.Partial", {
        "peer_id": PEER, "topic": TOPIC, "group_id_hex": GROUP, "group_bytes": 20,
        "body_present": False, "body_hex": None, "body_bytes": None,
        "metadata_present": True, "metadata_hex": META, "metadata_bytes": 7,
        "authority": "native_application_event_not_signed_validation_or_delivery"})
    yield row("rust", "partial_application_closed", "rust.fixture.extensions.application", {
        "peer_id": PEER, "group_id_hex": GROUP, "admitted": False})
    yield row("rust", "partial_rejected", "rust.fixture.extensions.application", {
        "peer_id": PEER, "group_id_hex": GROUP, "error": "wrong mode/topic/group",
        "scope": "application_only_no_report_invalid_partial_or_full_validation"})
    yield row("rust", "partial_offer", "rust.fixture.extensions.application", RUST_STATE)
    yield row("rust", "partial_applied", "rust.fixture.extensions.application", RUST_STATE | {
        "peer_id": PEER, "body_applied": True, "incoming_sequence": 3,
        "authority": "application_checked_bytes_not_signed_message_validation"})
    yield row("rust", "partial_reconstructed", "rust.fixture.extensions.application", RECONSTRUCTED | {
        "parts_hex": PARTS, "part_incoming_sequences": [3, 4, 5], "part_peer_ids": [PEER] * 3})
    yield row("rust", "partial_publish_return", "rust.libp2p.gossipsub.Behaviour.publish_partial", {
        "group_id_hex": GROUP, "error": None, "authority": "native_public_API_return_not_wire_write",
        "off_mesh_basis": "only_native_same_group_state_recipients"})
    view = {"peer_id": None, "view": "local_advertisement", "group_id_hex": GROUP}
    yield row("rust", "partial_metadata_hook", "rust.libp2p.gossipsub.Metadata.update", view | {
        "metadata_hex": META, "metadata_bytes": 7, "changed": False, "error": None,
        "authority": "application_metadata_replacement_not_generic_router_validation"})
    yield row("rust", "partial_data_hook", "rust.libp2p.gossipsub.Metadata.update_from_data", view | {
        "body_hex": PARTS[0], "body_bytes": len(bytes.fromhex(PARTS[0])),
        "authority": "native_hook_observation_no_predicted_part_ownership"})
    yield row("rust", "partial_action_hook", "rust.libp2p.gossipsub.Partial.partial_action_from_metadata", {
        "peer_id": PEER, "group_id_hex": GROUP, "metadata_present": False, "metadata_hex": None,
        "metadata_bytes": None, "local_metadata_hex": META, "need": False, "body_hex": None, "error": None,
        "authority": "native_application_action_not_wire_write_or_signed_delivery"})
    yield row("rust", "extension_stopped", "rust.fixture.extensions.application", {
        "admission_closed": True, "pending_validations": 0, "pending_hooks": 0, "drain_error": None,
        "cancellation_error": None, "scope": "inline_application_only_not_native_close_or_transport_join"})
    yield row("forge", "validation_hold_armed", "forge.fixture.extensions.command", {
        "extension_mode": "idontwant", "topic": TOPIC, "command_sequence": 1,
        "payload_sha256": "a" * 64, "payload_bytes": 2048}, "idontwant")
    yield row("forge", "validation_release", "forge.fixture.extensions.command", {
        "extension_mode": "idontwant", "topic": TOPIC, "command_sequence": 2,
        "held_observation_sequence": 3}, "idontwant")
    yield row("forge", "validation_held", "forge.fixture.extensions.validation", FORGE_MESSAGE | {
        "extension_mode": "idontwant", "command_sequence": 1, "committed": False}, "idontwant")
    yield row("forge", "validation_resumed", "forge.fixture.extensions.validation", FORGE_MESSAGE | {
        "extension_mode": "idontwant", "held_observation_sequence": 3, "committed": False,
        "released": True}, "idontwant")
    yield row("forge", "publish", "forge.node.async_publish", {
        "command_sequence": 1, "topic": TOPIC, "payload_sha256": "a" * 64})
    yield row("forge", "partial_advertise_return", "forge.node.async_advertise_partial", {
        "command_sequence": 1, "topic": TOPIC, "group_id_hex": GROUP, "error": None,
        "authority": "local_group_registration_not_wire_send"})
    yield row("forge", "partial_offer", "forge.fixture.extensions.application", RUST_STATE | {
        "command_sequence": 1, "local_have": 7, "advertised": True})
    yield row("forge", "partial_incoming", "forge.pubsub.partial_handler", {
        "peer_id": PEER, "topic": TOPIC, "group_present": True, "group_id_hex": GROUP, "group_bytes": 20,
        "body_present": False, "body_hex": None, "body_bytes": None,
        "metadata_present": True, "metadata_hex": META, "metadata_bytes": 7,
        "authority": "native_application_callback_not_validation_or_delivery"})
    yield row("forge", "partial_rejected", "forge.fixture.extensions.application", {
        "peer_id": PEER, "group_id_hex": GROUP, "observation_sequence": 3, "error": "invalid metadata",
        "scope": "application_only_no_router_validation_result"})
    yield row("forge", "partial_applied", "forge.fixture.extensions.application", RUST_STATE | {
        "received_have": 1, "peer_id": PEER, "body_applied": True, "incoming_sequence": 3,
        "authority": "application_checked_bytes_not_signed_message_validation"})
    yield row("forge", "partial_gossip", "forge.pubsub.partial_gossip_handler", {
        "group_id_hex": GROUP, "peer_ids": [PEER], "authority": "native_gossip_recipient_callback_not_wire_send"})
    yield row("forge", "partial_publish_action", "forge.node.async_send_partial.application_submission", {
        "peer_id": PEER, "topic": TOPIC, "group_id_hex": GROUP, "observation_sequence": 3,
        "gossip_metadata_only": False, "body_present": True, "body_hex": PARTS[0], "metadata_hex": META,
        "authority": "application_submission_not_wire_write"})
    yield row("forge", "partial_publish_return", "forge.node.async_send_partial", {
        "peer_id": PEER, "topic": TOPIC, "group_id_hex": GROUP, "observation_sequence": 3, "error": None,
        "authority": "native_public_API_return_not_delivery"})
    yield row("forge", "partial_reconstructed", "forge.fixture.extensions.application", RECONSTRUCTED | {
        "parts_hex": PARTS, "part_incoming_sequences": [3, 4, 5], "part_peer_ids": [PEER] * 3})
    yield row("forge", "partial_unsubscribe_return", "forge.node.async_unsubscribe.partial_topic", {
        "topic": TOPIC, "admission_closed": True, "active_work": 0, "error": None,
        "scope": "scoped_unsubscribe_not_native_callback_join"})
    yield row("forge", "extension_stopped", "forge.fixture.extensions.application_drain", {
        "admission_closed": True, "active_work": 0, "active_validators": 0, "inputs": 3,
        "partial_registration_active": False, "drain_error": None,
        "scope": "fixture_admitted_work_not_native_callback_or_node_join"})


class ExtensionEventsTests(unittest.TestCase):
    def sample(self, implementation, kind):
        return next((raw, event) for raw, event in examples()
                    if raw["implementation"] == implementation and event["kind"] == kind)

    def reject(self, raw, event):
        with self.assertRaises(ValueError):
            validate_extension_event(raw, event)

    def test_actual_emitter_shapes_cover_exact_allowlist(self):
        seen = set()
        for raw, event in examples():
            with self.subTest(implementation=raw["implementation"], kind=event["kind"]):
                before = deepcopy((raw, event))
                self.assertTrue(validate_extension_event(raw, event))
                self.assertEqual((raw, event), before)
                self.assertNotIn(event["kind"], {"rpc", "protocol", "delivery", "validation"})
                if event["kind"] == "publish":
                    self.assertEqual((raw["implementation"], event["source"]), ("forge", "forge.node.async_publish"))
                else:
                    self.assertNotIn(event["source"], SOURCES[raw["implementation"]].get(event["kind"], set()))
                seen.add((raw["implementation"], event["kind"]))
        self.assertEqual(seen, set(SCHEMAS))

    def test_every_shape_rejects_wrong_source_and_unknown_or_missing_fields(self):
        for raw, event in examples():
            for bad in (event | {"source": event["source"] + ".forged"}, event | {"native_wire_proof": True},
                        event | {"source": raw["implementation"] + ".pubsub.native_stream.read"}):
                with self.subTest(kind=event["kind"], bad=bad):
                    self.reject(raw, bad)
            for name in set(event) - {"mono_ns"}:
                bad = dict(event)
                del bad[name]
                # mono/sequence ordering is separately enforced by _events. All shown schema fields are required,
                # except the all-or-none held-message identity on Rust release/cancellation diagnostics.
                if raw["implementation"] == "rust" and event["kind"] in {"validation_released", "validation_cancelled"}:
                    if name in MESSAGE and name != "payload_sha256":
                        self.reject(raw, bad)
                        continue
                self.reject(raw, bad)

    def test_extension_mode_version_and_legacy_crossmix_fail_closed(self):
        for raw, event in examples():
            mutations = ({"extension": None}, {"extension": ""}, {"extension": "future"},
                         {"version": "1.1"}, {"version": "1.4"}, {"version": True},
                         {"requests_partial": int(raw["requests_partial"])},
                         {"implementation": "go" if raw["implementation"] == "forge" else "forge"},
                         {"case_token": TOKEN.upper()}, {"case_token": "a" * 31})
            for mutation in mutations:
                if event["kind"] == "publish" and mutation in ({"extension": None}, {"extension": ""}, {"implementation": "go"}):
                    # Shared legacy/native kinds are dispatched to the original source gate, not this registry.
                    self.assertFalse(validate_extension_event(raw | mutation, event))
                    continue
                with self.subTest(kind=event["kind"], mutation=mutation):
                    self.reject(raw | mutation, event)
            for field in ("extension", "version", "requests_partial"):
                missing = dict(raw)
                del missing[field]
                if event["kind"] == "publish" and field == "extension":
                    self.assertFalse(validate_extension_event(missing, event))
                else:
                    self.reject(missing, event)

    def test_idw_events_cannot_run_in_partial_or_advertisement(self):
        for implementation in ("go", "rust", "forge"):
            _, event = self.sample(implementation, "validation_held")
            for mode in ("partial", "advertisement"):
                bad = event | ({"extension_mode": mode} if implementation == "go" else {})
                self.reject(raw_actor(implementation, mode), bad)

    def test_application_payload_never_becomes_rpc_protocol_or_delivery(self):
        for implementation in ("go", "rust", "forge"):
            raw, event = self.sample(implementation, "partial_incoming")
            for kind in ("rpc", "protocol", "delivery", "validation", "future_partial_kind"):
                self.reject(raw, event | {"kind": kind})
                native_source = {"go": "go.quic.native_stream.read", "rust": "rust.libp2p.passive-upgraded-stream-io",
                                 "forge": "forge.pubsub.native_stream"}[implementation]
                self.reject(raw, event | {"kind": kind, "source": native_source, "direction": "read"})

    def test_legacy_native_event_is_unchanged_and_unknown_source_is_not_allowed(self):
        raw = raw_actor("rust")
        for name in ("extension", "version", "requests_partial"):
            del raw[name]
        event = {"sequence": 1, "mono_ns": 1, "kind": "listen", "source": "rust.libp2p.Swarm.NewListenAddr"}
        self.assertFalse(validate_extension_event(raw, event))
        raw["events"] = [event]
        self.assertIs(_events(raw, "rust", TOKEN, "victim"), raw["events"])
        raw["events"] = [event | {"kind": "partial_new", "source": "rust.fixture.extensions.future"}]
        with self.assertRaises(ValueError):
            _events(raw, "rust", TOKEN, "victim")

    def test_events_hook_keeps_base_clock_and_native_authority_checks(self):
        raw, event = self.sample("rust", "partial_offer")
        event.update(sequence=1, mono_ns=1)
        raw["events"] = [event]
        self.assertEqual(_events(raw, "rust", TOKEN, "victim"), [event])
        for changes in ({"sequence": True}, {"sequence": 2}, {"mono_ns": True}, {"mono_ns": 0},
                        {"kind": "rpc", "source": "rust.fixture.extensions.application"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                _events(raw | {"events": [event | changes]}, "rust", TOKEN, "victim")

    def test_bytes_bounds_types_and_presence_are_exact(self):
        for implementation in ("go", "rust", "forge"):
            raw, event = self.sample(implementation, "partial_incoming")
            for changes in ({"peer_id": "not-a-peer"}, {"body_present": 0}, {"metadata_present": 1},
                            {"body_hex": "ab" * 261}, {"metadata_hex": "00" * 8},
                            {"metadata_hex": "ff "}, {"metadata_hex": "FF"}, {"metadata_hex": "f"},
                            {"group_id_hex": "aa" * 21}, {"body_hex": PARTS[0]}, {"metadata_present": False}):
                with self.subTest(implementation=implementation, changes=changes):
                    self.reject(raw, event | changes)
        raw, event = self.sample("rust", "partial_incoming")
        for changes in ({"group_bytes": True}, {"group_bytes": 19}, {"metadata_bytes": None},
                        {"metadata_bytes": 6}, {"metadata_bytes": 1024 * 1024 + 1}, {"body_bytes": 0}):
            self.reject(raw, event | changes)

    def test_bounded_malformed_incoming_is_diagnostic_not_applied(self):
        raw, event = self.sample("rust", "partial_incoming")
        event.update(group_id_hex="", group_bytes=0, topic="foreign-topic", metadata_hex="ff" * 7,
                     metadata_bytes=50, body_present=True, body_hex="aa" * 260, body_bytes=500)
        self.assertTrue(validate_extension_event(raw, event))
        self.reject(raw, event | {"kind": "partial_applied", "source": "rust.fixture.extensions.application"})
        raw, event = self.sample("go", "partial_incoming")
        event.update(body_present=True, body_hex="", metadata_present=True, metadata_hex="")
        self.assertTrue(validate_extension_event(raw, event))

    def test_owned_group_and_topic_cannot_borrow_foreign_state(self):
        for implementation in ("go", "rust", "forge"):
            raw, event = self.sample(implementation, "partial_offer")
            self.reject(raw, event | {"group_id_hex": group_id("b" * 32).hex()})
        raw, event = self.sample("go", "partial_offer")
        self.reject(raw, event | {"topic": "forge-pr11:" + "b" * 32})
        self.reject(raw, event | {"extension_mode": "advertisement"})

    def test_full_only_and_metadata_gossip_never_submit_body(self):
        raw, event = self.sample("go", "partial_publish_action")
        self.reject(raw, event | {"requests_partial": False})
        self.reject(raw, event | {"gossip_metadata_only": True})
        for changes in ({"requests_partial": False}, {"gossip_metadata_only": True}):
            self.assertTrue(validate_extension_event(raw, event | changes | {"body_present": False, "body_hex": ""}))
        self.reject(raw, event | {"body_hex": PARTS[0] + "00"})
        self.reject(raw, event | {"command_sequence": 1})
        self.reject(raw, event | {"observation_sequence": 0})

    def test_local_metadata_masks_and_application_reconstruction_are_checked(self):
        for implementation in ("go", "rust", "forge"):
            raw, event = self.sample(implementation, "partial_offer")
            for field, value in (("revision", True), ("revision", 0), ("revision", 2**32), ("have", 8), ("want", 1)):
                self.reject(raw, event | {field: value})
            raw, event = self.sample(implementation, "partial_reconstructed")
            for changes in ({"payload_hex": PAYLOAD.hex() + "00"}, {"payload_sha256": "0" * 64},
                            {"payload_bytes": True}, {"authority": "native_delivery"}):
                self.reject(raw, event | changes)
        raw, event = self.sample("rust", "partial_reconstructed")
        self.reject(raw, event | {"parts_hex": [PARTS[0], PARTS[0], PARTS[2]]})
        self.reject(raw, event | {"part_incoming_sequences": [3, 3, 5]})
        self.reject(raw, event | {"part_incoming_sequences": [3, 4, event["sequence"]]})
        for changes in ({"part_incoming_sequences": [3, 4]}, {"part_incoming_sequences": [True, 4, 5]},
                        {"part_peer_ids": [PEER, PEER]}, {"part_peer_ids": [PEER, PEER, "not-a-peer"]}):
            self.reject(raw, event | changes)

    def test_rust_hook_error_and_capture_shape_do_not_invent_success(self):
        raw, event = self.sample("rust", "partial_metadata_hook")
        self.reject(raw, event | {"changed": None})
        self.reject(raw, event | {"peer_id": PEER})
        self.reject(raw, event | {"metadata_bytes": 8})
        self.assertTrue(validate_extension_event(raw, event | {
            "metadata_bytes": 100, "metadata_hex": "ff" * 7, "changed": None, "error": "malformed input"}))
        raw, event = self.sample("rust", "partial_action_hook")
        self.reject(raw, event | {"error": "invalid metadata"})
        self.reject(raw, event | {"metadata_hex": META})
        self.assertTrue(validate_extension_event(raw, event | {"need": None, "error": "invalid metadata"}))
        self.reject(raw, event | {"need": None, "error": "invalid metadata", "body_hex": PARTS[0]})

    def test_message_identity_release_and_cancel_remain_observations(self):
        for implementation in ("go", "rust", "forge"):
            raw, event = self.sample(implementation, "validation_held")
            self.reject(raw, event | {"committed": True})
            self.reject(raw, event | {"message_id": "ab"})
            self.reject(raw, event | {"seqno_hex": "00" * 7})
        raw, event = self.sample("go", "validation_resumed")
        self.reject(raw, event | {"released": False})
        self.assertTrue(validate_extension_event(raw, event | {"released": False, "error": "context canceled"}))
        raw, event = self.sample("rust", "validation_cancelled")
        self.reject(raw, event | {"command_sequence": 2})
        absent = {key: value for key, value in event.items() if key not in MESSAGE or key == "payload_sha256"}
        absent["validation_commit"] = False
        self.assertTrue(validate_extension_event(raw, absent))
        self.reject(raw, absent | {"message_id": MESSAGE["message_id"]})
        self.assertTrue(validate_extension_event(raw, absent | {
            "kind": "validation_released", "command_sequence": 2, "cancellation": None}))
        self.reject(raw, absent | {"validation_commit": True})

    def test_lifecycle_scopes_are_exact_not_transport_join_claims(self):
        for implementation in ("go", "rust", "forge"):
            raw, event = self.sample(implementation, "extension_stopped")
            self.reject(raw, event | {"native_tasks_joined": True})
            self.reject(raw, event | {"scope": "all_router_goroutines"})
        raw, event = self.sample("go", "extension_stopped")
        self.reject(raw, event | {"worker_started": False})
        self.reject(raw, event | {"active_publish_callbacks": 1})
        self.reject(raw, event | {"pending_work": 33})

    def test_forge_capture_failure_is_never_an_application_success(self):
        for raw, event in examples():
            if raw["implementation"] != "forge":
                continue
            for error in ("capture failed", "", False, 0):
                with self.subTest(kind=event["kind"], error=error):
                    self.reject(raw | {"extension_capture_error": error}, event)
            missing = dict(raw)
            del missing["extension_capture_error"]
            self.reject(missing, event)
        raw = raw_actor("forge")
        native = {"kind": "publish", "source": "forge.node.async_publish", "sequence": 1, "mono_ns": 1,
                  "command_sequence": 1, "topic": TOPIC, "payload_sha256": "a" * 64}
        with self.assertRaises(ValueError):
            _events(raw | {"extension_capture_error": "capture failed", "events": [native]}, "forge", TOKEN, "victim")

    def test_forge_capture_presence_and_full_native_size_bounds(self):
        raw, event = self.sample("forge", "partial_incoming")
        for name, hex_name in (("group", "group_id_hex"), ("body", "body_hex"), ("metadata", "metadata_hex")):
            absent = event | {name + "_present": False, hex_name: None, name + "_bytes": None}
            empty = event | {name + "_present": True, hex_name: "", name + "_bytes": 0}
            self.assertTrue(validate_extension_event(raw, absent))
            self.assertTrue(validate_extension_event(raw, empty))
            for bad in (absent | {hex_name: ""}, absent | {name + "_bytes": 0},
                        empty | {hex_name: None}, empty | {name + "_bytes": None},
                        empty | {name + "_bytes": True}):
                with self.subTest(name=name, bad=bad):
                    self.reject(raw, bad)
        captured = event | {"body_present": True, "body_hex": "ab" * 260, "body_bytes": 4096}
        self.assertTrue(validate_extension_event(raw, captured))
        for changes in ({"body_bytes": 4097}, {"body_bytes": 259}, {"body_hex": "ab" * 259},
                        {"group_bytes": 21}, {"group_bytes": 19}, {"metadata_bytes": 8}, {"metadata_bytes": 6}):
            self.reject(raw, captured | changes)
        # Malformed callback inputs are observations, not successful application or owned group claims.
        foreign = event | {"group_id_hex": "ff" * 20, "metadata_hex": "ff" * 7}
        self.assertTrue(validate_extension_event(raw, foreign))
        self.reject(raw, foreign | {"topic": "foreign-topic"})
        self.reject(raw, foreign | {"kind": "partial_applied", "source": "forge.fixture.extensions.application"})
        _, rejected = self.sample("forge", "partial_rejected")
        self.assertTrue(validate_extension_event(raw, rejected | {"group_id_hex": None}))

    def test_forge_owned_topic_group_and_bounded_references(self):
        for raw, event in examples():
            if raw["implementation"] != "forge":
                continue
            if "topic" in event:
                self.reject(raw, event | {"topic": "forge-pr11:" + "b" * 32})
            if "group_id_hex" in event and event["kind"] not in {"partial_incoming", "partial_rejected"}:
                self.reject(raw, event | {"group_id_hex": group_id("b" * 32).hex()})
            for name in ("observation_sequence", "incoming_sequence", "held_observation_sequence"):
                if name in event:
                    for value in (0, True, 2049, event["sequence"], event["sequence"] + 1):
                        with self.subTest(kind=event["kind"], field=name, value=value):
                            self.reject(raw, event | {name: value})
            if "command_sequence" in event:
                for value in (0, True, 65):
                    self.reject(raw, event | {"command_sequence": value})
        raw, event = self.sample("forge", "partial_reconstructed")
        for changes in ({"parts_hex": [PARTS[0], PARTS[0], PARTS[2]]},
                        {"part_incoming_sequences": [3, 3, 5]}, {"part_incoming_sequences": [True, 4, 5]},
                        {"part_incoming_sequences": [3, 4, 30]}, {"part_peer_ids": [PEER] * 2},
                        {"part_peer_ids": [PEER, PEER, "foreign"]}):
            self.reject(raw, event | changes)

    def test_forge_registration_and_submission_never_claim_wire_or_delivery(self):
        raw, offer = self.sample("forge", "partial_offer")
        for changes in ({"local_have": 0}, {"advertised": False}, {"advertised": 1},
                        {"received_have": 8}, {"metadata_hex": Metadata(2, 7, 0).encode().hex()}):
            self.reject(raw, offer | changes)
        self.assertTrue(validate_extension_event(raw, offer | {"local_have": 0, "received_have": 7}))
        _, action = self.sample("forge", "partial_publish_action")
        for changes in ({"body_present": False}, {"body_hex": ""}, {"body_hex": PARTS[0] + "00"},
                        {"body_hex": encode_part(0, b"foreign").hex()}, {"gossip_metadata_only": True},
                        {"metadata_hex": Metadata(1, 0, 7).encode().hex()}, {"requests_partial": True}):
            self.reject(raw, action | changes)
        self.assertTrue(validate_extension_event(raw, action | {
            "body_present": False, "body_hex": "", "gossip_metadata_only": True}))
        for kind in ("partial_advertise_return", "partial_publish_return", "partial_unsubscribe_return"):
            _, event = self.sample("forge", kind)
            self.assertTrue(validate_extension_event(raw, event | {"error": "native API failed"}))
            for changes in ({"error": ""}, {"error": "x" * 513}, {"error": "bad\nerror"},
                            {"error": "non-ASCII: \u00e9"}, {"native_write_succeeded": True},
                            {"authority": "native_delivery"}):
                self.reject(raw, event | changes)
        _, gossip = self.sample("forge", "partial_gossip")
        self.assertTrue(validate_extension_event(raw, gossip | {"peer_ids": []}))
        for peers in ([PEER, PEER], ["foreign"], [peer_id(i + 1) for i in range(17)]):
            self.reject(raw, gossip | {"peer_ids": peers})

    def test_forge_held_validation_release_keeps_uncommitted_result(self):
        raw, event = self.sample("forge", "validation_resumed")
        for error in ("validation hold expired or cancelled", "validation hold woke without release"):
            self.assertTrue(validate_extension_event(raw, event | {"released": False, "error": error}))
            self.reject(raw, event | {"error": error})
        for changes in ({"released": False}, {"committed": True}, {"released": 1},
                        {"error": "other error", "released": False}, {"extension_mode": "partial"},
                        {"message_id_basis": "native_pubsub_Message.ID"}):
            self.reject(raw, event | changes)

    def test_forge_scoped_drain_modes_cannot_invent_callback_or_node_join(self):
        raw, stopped = self.sample("forge", "extension_stopped")
        for changes in ({"active_work": 1}, {"active_work": False}, {"active_validators": 1},
                        {"inputs": 65}, {"inputs": True}, {"partial_registration_active": True},
                        {"drain_error": "failed"}, {"native_callbacks_joined": True}):
            self.reject(raw, stopped | changes)
        for mode in ("advertisement", "idontwant"):
            no_callbacks = stopped | {"inputs": 0}
            self.assertTrue(validate_extension_event(raw_actor("forge", mode), no_callbacks))
            self.reject(raw_actor("forge", mode), stopped)
        _, unsubscribe = self.sample("forge", "partial_unsubscribe_return")
        for changes in ({"active_work": 1}, {"admission_closed": False},
                        {"scope": "native_callback_join"}):
            self.reject(raw, unsubscribe | changes)
        # Full-only advertisement installs no partial callbacks or scoped registration.
        for value, event in examples():
            if value["implementation"] == "forge" and event["kind"].startswith("partial_"):
                self.reject(raw_actor("forge", "advertisement"), event)

    def test_forge_publish_schema_does_not_change_legacy_native_dispatch(self):
        raw, event = self.sample("forge", "publish")
        for mode in ("advertisement", "idontwant", "partial"):
            self.assertTrue(validate_extension_event(raw_actor("forge", mode), event))
        for implementation in ("go", "rust", "forge"):
            source = next(iter(SOURCES[implementation]["publish"]))
            legacy = raw_actor(implementation)
            for field in ("extension", "version", "requests_partial", "extension_capture_error"):
                legacy.pop(field, None)
            native = event | {"source": source, "sequence": 1, "mono_ns": 1}
            self.assertFalse(validate_extension_event(legacy, native))
            legacy["events"] = [native]
            self.assertIs(_events(legacy, implementation, TOKEN, "victim"), legacy["events"])
        raw["events"] = [event | {"sequence": 1, "mono_ns": 1}]
        self.assertIs(_events(raw, "forge", TOKEN, "victim"), raw["events"])
        for changes in ({"payload_sha256": "a" * 63}, {"topic": "foreign"}, {"command_sequence": True},
                        {"source": "forge.fixture.extensions.application"}):
            with self.assertRaises(ValueError):
                _events(raw | {"events": [raw["events"][0] | changes]}, "forge", TOKEN, "victim")


if __name__ == "__main__":
    unittest.main()
