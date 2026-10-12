"""Synthetic receipt rejection tests, never a configured donor acceptance run."""

from copy import deepcopy
import hashlib
import unittest

from plugin_node_cases import Case
from plugin_node_evidence import (
    _go_dialer, _tcp_donor, accepted_configuration, counter, go_completion_pair,
    go_completion_witness, negative_pair, plugin_exchange, plugin_exchange_witness,
    go_listener_completion_pair,
    rust_completion_pair, rust_completion_witness,
)


def plugin_payload(spec, role="dialer"):
    receipt = {"bytes": len(spec.payload), "sha256": hashlib.sha256(spec.payload).hexdigest(), "contains_zero": 0 in spec.payload}
    return {"status": "ok", "final": True, "errors": [], "local_peer_id": "local", "lifecycle": {
        "plugins_joined": True, "scheduler_joined": True}, "exchange": {
        "source": "official-node.protocol-api.open" if role == "dialer" else "official-node.protocol-handler",
        "protocol": "/forge/interop/relay-echo/1", "stream_closed": True, "response_write_completed": True,
        "request": deepcopy(receipt), "response": deepcopy(receipt), "expected_peer_id": "remote",
        "session": {"source": "node.protocol-api.stream-and-node.diagnostics-api.session", "session_id": 1,
                    "local_peer_id": "local", "remote_peer_id": "remote", "identify_state": "identified", "path": "direct",
                    "direction": "outbound" if role == "dialer" else "inbound", "authentication": "noise",
                    "stream_authentication": "noise", "muxer": "/yamux/1.0.0", "local_address": "/ip4/127.0.0.1/tcp/41001",
                    "remote_address": "/ip4/127.0.0.1/tcp/41002"}}}


class GoOrderlyListenerTests(unittest.TestCase):
    def receipts(self):
        spec = Case("mdbx", "quic", "forge", "go")
        session = {"local_peer_id": "forge", "remote_peer_id": "go",
                   "local_address": "/ip4/127.0.0.1/udp/41001/quic-v1",
                   "remote_address": "/ip4/127.0.0.1/udp/41002/quic-v1"}
        witness = {"implementation": "go", "role": "listener", "scenario": "echo_large",
                   "protocol": "/forge/interop/relay-echo/1", "status": "ok", "final": False,
                   "payload_bytes": len(spec.payload), "payload_sha256": hashlib.sha256(spec.payload).hexdigest(),
                   "local_peer_id": "go", "authenticated_remote_peer_id": "forge",
                   "connection_local_addr": session["remote_address"], "connection_remote_addr": session["local_address"],
                   "application_connection_id": "conn", "application_stream_id": "stream",
                   "echo_listener_completion": {"source": "go-fixture.echo-listener-completion.v1", "mode": "orderly-fin",
                                                "request_eof_observed": True, "stream_close_completed": True,
                                                "host_close_completed": False}}
        terminal = deepcopy(witness)
        terminal["final"] = True
        terminal["echo_listener_completion"]["host_close_completed"] = True
        return {"listener_witness": witness, "result": terminal}, spec, session

    def test_synthetic_orderly_completion_not_live_evidence(self):
        endpoint, spec, session = self.receipts()
        go_listener_completion_pair(endpoint, spec, session)

    def test_missing_eof_close_or_host_join_cannot_pass(self):
        endpoint, spec, session = self.receipts()
        for side in ("listener_witness", "result"):
            for field in ("request_eof_observed", "stream_close_completed", "host_close_completed"):
                for bad in (None, 1, "true"):
                    broken = deepcopy(endpoint)
                    broken[side]["echo_listener_completion"][field] = bad
                    with self.subTest(side=side, field=field, bad=bad), self.assertRaises(ValueError):
                        go_listener_completion_pair(broken, spec, session)
        for field in ("request_eof_observed", "stream_close_completed"):
            broken = deepcopy(endpoint)
            broken["listener_witness"]["echo_listener_completion"][field] = False
            with self.assertRaises(ValueError):
                go_listener_completion_pair(broken, spec, session)

    def test_reset_or_substituted_connection_payload_is_not_completion(self):
        endpoint, spec, session = self.receipts()
        for side in ("listener_witness", "result"):
            for field, bad in (("error", "reset"), ("payload_bytes", 1), ("payload_sha256", "0" * 64),
                               ("local_peer_id", "other"), ("application_connection_id", "other"),
                               ("application_stream_id", "other"), ("status", "error"), ("final", None)):
                broken = deepcopy(endpoint)
                broken[side][field] = bad
                with self.subTest(side=side, field=field), self.assertRaises(ValueError):
                    go_listener_completion_pair(broken, spec, session)


class RustCompletionTests(unittest.TestCase):
    def receipts(self):
        spec = Case("rocksdb", "quic", "rust", "forge")
        session = {"local_peer_id": "forge", "remote_peer_id": "rust",
                   "local_address": "/ip4/127.0.0.1/udp/41002/quic-v1"}
        digest = {"bytes": len(spec.payload), "sha256": hashlib.sha256(spec.payload).hexdigest()}
        witness = {"implementation": "rust", "role": "dialer", "scenario": "echo_large", "status": "exchange_observed",
                   "final": False, "protocol": "/forge/interop/relay-echo/1", "echo_ok": True,
                   "payload_bytes": len(spec.payload),
                   "exchange": {"source": "rust-fixture.actual-echo-io.v1", "write": deepcopy(digest), "read": deepcopy(digest)},
                   "connection": {"source": "rust-libp2p.SwarmEvent.ConnectionEstablished", "connection_id": "1",
                                  "local_peer_id": "rust", "remote_peer_id": "forge", "direction": "outbound",
                                  "local_address": None, "remote_address": session["local_address"] + "/p2p/forge"},
                   "echo_completion": {"source": "rust-fixture.echo-completion-barrier.v1", "mode": "stop-file",
                                       "phase": "await_stop", "stop_observed": False, "stream_close_completed": False,
                                       "swarm_dropped": False, "fixture_owned_tasks_joined": False,
                                       "exchange_observations": 1, "stop_observations": 0, "stream_close_completions": 0}}
        terminal = deepcopy(witness)
        terminal.update(final=True, status="ok", fixture_task_lifecycle={
            "scope": "public_swarm_executor_and_fixture_echo_handler",
            "shutdown_mode": "close_admission_abort_join_after_swarm_drop", "fixture_owned_tasks_joined": True,
            "overflow": False, "errors": [], "tasks": [{"task_id": "1", "kind": "swarm_connection",
                                                         "abort_requested": True, "terminal": "cancelled_by_owner"}]})
        terminal["echo_completion"].update(phase="joined_after_swarm_drop", stop_observed=True,
                                           stream_close_completed=True, swarm_dropped=True, fixture_owned_tasks_joined=True,
                                           stop_observations=1, stream_close_completions=1)
        return {"exchange_witness": witness, "result": terminal}, spec, session

    def test_actual_exchange_and_scoped_tasks_not_native_shutdown_claim(self):
        endpoint, spec, session = self.receipts()
        rust_completion_pair(endpoint, spec, session)
        self.assertIsNone(endpoint["result"]["connection"]["local_address"])
        self.assertNotIn("native_shutdown_proven", endpoint["result"])

    def test_failure_partial_io_wrong_connection_or_early_terminal_rejected(self):
        endpoint, spec, session = self.receipts()
        for mutation in (lambda p: p.update(error="closed"), lambda p: p.update(final=True),
                         lambda p: p["exchange"]["write"].update(bytes=1),
                         lambda p: p["exchange"]["read"].update(sha256="0" * 64),
                         lambda p: p["connection"].update(connection_id="other"),
                         lambda p: p["connection"].update(remote_peer_id="other"),
                         lambda p: p["connection"].update(local_address="invented"),
                         lambda p: p["connection"].update(remote_address="/ip4/127.0.0.1/udp/4000/quic-v1"),
                         lambda p: p["echo_completion"].update(stream_close_completed=True),
                         lambda p: p["echo_completion"].update(exchange_observations=True)):
            broken = deepcopy(endpoint)
            mutation(broken["exchange_witness"])
            with self.assertRaises(ValueError):
                rust_completion_pair(broken, spec, session)
        with self.assertRaises(ValueError):
            rust_completion_witness(endpoint["result"], spec, session)

    def test_failed_or_missing_task_join_cannot_repair_the_witness(self):
        endpoint, spec, session = self.receipts()
        for mutation in (lambda p: p.pop("fixture_task_lifecycle"),
                         lambda p: p["fixture_task_lifecycle"].update(fixture_owned_tasks_joined=False),
                         lambda p: p["fixture_task_lifecycle"].update(errors=["join failed"]),
                         lambda p: p["fixture_task_lifecycle"].update(overflow=True),
                         lambda p: p["fixture_task_lifecycle"].update(tasks=[]),
                         lambda p: p["echo_completion"].update(stream_close_completed=False),
                         lambda p: p["echo_completion"].update(stop_observations=0)):
            broken = deepcopy(endpoint)
            mutation(broken["result"])
            with self.assertRaises(ValueError):
                rust_completion_pair(broken, spec, session)


def negative_record(spec):
    plugin_role = "dialer" if spec.dialer == "forge" else "listener"
    donor_role = "listener" if plugin_role == "dialer" else "dialer"
    donor_name = spec.listener if plugin_role == "dialer" else spec.dialer
    plugin = {"status": "rejection_candidate" if plugin_role == "dialer" else "rejected", "final": True, "errors": [],
              "exchange": None, "control_kind": spec.control, "correlation_token": spec.scenario_id,
              "lifecycle": {"plugins_joined": True, "scheduler_joined": True}, "rejection": {
                  "source": "node.connect-and-diagnostics-api", "error_category": "forge.net.p2p", "error_code": 4,
                  "path_direct_attempts": 1, "direct_failures": 1, "handshakes_completed": 0,
                  "sessions_opened": 0, "protocol_streams_opened": 0}}
    if plugin_role == "listener":
        plugin["rejection"] = {"source": "node.diagnostics-api-and-configured-gater", "attempted_connections": 1,
            "handshakes_failed": 1, "secured_callbacks": 0, "handshakes_completed": 0, "sessions_opened": 0,
            "protocol_streams_opened": 0, "protocol_streams_accepted": 0, "inbound_connections": 0}
    donor = {"status": "rejected", "control_kind": spec.control, "correlation_token": spec.scenario_id,
             "counter_source": "go-libp2p.connection-gater" if donor_name == "go" else "rust-libp2p.swarm-events",
             "attempted_connections": 1, "established_connections": 0, "identify_streams": 0,
             "application_streams": 0, "rejected_before_identify": True,
             "expected_peer_id": "listener" if donor_role == "dialer" else ""}
    return {"peer_id": "listener", "endpoints": {
        plugin_role: {"implementation": "forge", "result": plugin}, donor_role: {"implementation": donor_name, "result": donor}}}


class PluginReceiptTests(unittest.TestCase):
    def setUp(self):
        self.spec = Case("mdbx", "tcp", "forge", "go")
        self.payload = plugin_payload(self.spec)

    def test_synthetic_valid_plugin_half_only(self):
        plugin_exchange(self.payload, self.spec, "dialer", "remote")

    def test_pending_or_lost_join_is_not_proof(self):
        for field in ("final", "plugins_joined", "scheduler_joined"):
            value = deepcopy(self.payload)
            (value if field == "final" else value["lifecycle"])[field] = False
            with self.subTest(field=field), self.assertRaises(ValueError):
                plugin_exchange(value, self.spec, "dialer", "remote")

    def test_actual_bytes_not_echo_label(self):
        for change in (lambda p: p["exchange"]["response"].update(sha256="0" * 64),
                       lambda p: p["exchange"]["request"].update(bytes=1),
                       lambda p: p["exchange"]["response"].update(contains_zero=False)):
            value = deepcopy(self.payload)
            change(value)
            with self.assertRaises(ValueError):
                plugin_exchange(value, self.spec, "dialer", "remote")

    def test_completed_echo_label_cannot_hide_callback_close_failure(self):
        spec = Case("rocksdb", "quic", "go", "forge")
        payload = plugin_payload(spec, "listener")
        payload["exchange"]["session"].update(authentication="quic_tls", stream_authentication="quic_tls", muxer="",
            local_address="/ip4/127.0.0.1/udp/41002/quic-v1", remote_address="/ip4/127.0.0.1/udp/41001/quic-v1")
        plugin_exchange(payload, spec, "listener", "local")
        for phase in ("response_write", "stream_close"):
            value = deepcopy(payload)
            value["errors"] = [{"phase": "protocol.inbound_echo." + phase,
                                "error_category": "system", "error_code": 89}]
            with self.subTest(phase=phase), self.assertRaises(ValueError):
                plugin_exchange(value, spec, "listener", "local")
        for field in ("response_write_completed", "stream_closed"):
            value = deepcopy(payload)
            value["exchange"][field] = False
            with self.subTest(field=field), self.assertRaises(ValueError):
                plugin_exchange(value, spec, "listener", "local")

    def test_no_insecure_or_unidentified_relabel(self):
        for field, bad in (("authentication", "test_insecure"), ("stream_authentication", "quic_tls"),
                           ("identify_state", "identifying"), ("session_id", True), ("path", "relay"),
                           ("direction", "inbound"), ("muxer", "configured-yamux")):
            value = deepcopy(self.payload)
            value["exchange"]["session"][field] = bad
            with self.subTest(field=field), self.assertRaises(ValueError):
                plugin_exchange(value, self.spec, "dialer", "remote")

    def test_raw_node_receipt_cannot_be_accepted_configuration(self):
        with self.assertRaises(ValueError):
            accepted_configuration({"fixture_kind": "raw-node", "configuration_accepted": True}, self.spec, "dialer", "")

    def test_boolean_is_not_an_operational_counter(self):
        self.assertFalse(counter(True))
        self.assertFalse(counter(-1))
        self.assertFalse(counter(2**64))


class NegativeEvidenceTests(unittest.TestCase):
    def test_single_attempt_timeout_and_completed_failure_have_distinct_attribution(self):
        spec = Case("mdbx", "tcp-pnet-noise", "forge", "go", control="missing_key")
        for code, attempts, failures in ((12, 1, 0), (4, 1, 1), (14, 1, 1)):
            record = negative_record(spec)
            record["endpoints"]["dialer"]["result"]["rejection"].update(
                error_code=code, path_direct_attempts=attempts, direct_failures=failures)
            negative_pair(record, spec, "dialer")
            record["endpoints"]["listener"]["result"]["attempted_connections"] = 0
            with self.subTest(code=code), self.assertRaises(ValueError):
                negative_pair(record, spec, "dialer")

    def test_wrong_attribution_or_unrelated_native_error_is_not_rejection(self):
        spec = Case("rocksdb", "tcp-pnet-tls", "forge", "rust", control="mismatched_key")
        for outcome in ((12, 1, 1), (4, 1, 0), (15, 1, 1), (14, 0, 0), (14, 1, 0), (4, 0, 1),
                        (12, 2, 0), (12, 1, False), (4, True, 1), (4, 1, True), ("12", 1, 0)):
            record = negative_record(spec)
            record["endpoints"]["dialer"]["result"]["rejection"].update(
                zip(("error_code", "path_direct_attempts", "direct_failures"), outcome))
            with self.subTest(outcome=outcome), self.assertRaises(ValueError):
                negative_pair(record, spec, "dialer")

    def test_attributable_close_still_requires_clean_pre_auth_joined_owners(self):
        spec = Case("rocksdb", "tcp-pnet-tls", "forge", "go", control="mismatched_key")
        for change in (
            lambda p: p["rejection"].update(error_code=13),
            lambda p: p["rejection"].update(handshakes_completed=1),
            lambda p: p["rejection"].update(sessions_opened=1),
            lambda p: p["rejection"].update(protocol_streams_opened=1),
            lambda p: p["lifecycle"].update(plugins_joined=False),
            lambda p: p["lifecycle"].update(scheduler_joined=False),
            lambda p: p.update(errors=[{"phase": "forced_cleanup"}]),
        ):
            record = negative_record(spec)
            payload = record["endpoints"]["dialer"]["result"]
            payload["rejection"]["error_code"] = 14
            change(payload)
            with self.assertRaises(ValueError):
                negative_pair(record, spec, "dialer")

    def test_synthetic_pair_primitive_not_live_acceptance(self):
        for source, target in (("forge", "go"), ("rust", "forge")):
            spec = Case("mdbx", "tcp-pnet-noise", source, target, control="missing_key")
            negative_pair(negative_record(spec), spec, "dialer" if source == "forge" else "listener")

    def test_arbitrary_failed_connect_or_no_contact_never_passes(self):
        spec = Case("mdbx", "tcp-pnet-tls", "forge", "rust", control="mismatched_key")
        for endpoint, key, bad in (("listener", "attempted_connections", 0), ("listener", "attempted_connections", True),
                                   ("listener", "attempted_connections", 5), ("listener", "established_connections", 1),
                                   ("listener", "identify_streams", 1), ("listener", "application_streams", 1),
                                   ("listener", "correlation_token", "other-case")):
            record = negative_record(spec)
            record["endpoints"][endpoint]["result"][key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                negative_pair(record, spec, "dialer")
        for key, bad in (("error_code", 7), ("error_category", "unrelated"), ("direct_failures", 0),
                         ("handshakes_completed", 1), ("sessions_opened", 1)):
            record = negative_record(spec)
            record["endpoints"]["dialer"]["result"]["rejection"][key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                negative_pair(record, spec, "dialer")

    def test_inbound_resource_or_secure_transition_never_escapes(self):
        spec = Case("rocksdb", "tcp-pnet-noise", "go", "forge", control="missing_key")
        for key, bad in (("handshakes_failed", 0), ("secured_callbacks", 1), ("inbound_connections", 1),
                         ("sessions_opened", 1), ("protocol_streams_accepted", 1)):
            record = negative_record(spec)
            record["endpoints"]["listener"]["result"]["rejection"][key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                negative_pair(record, spec, "listener")


class GoPublishedReceiptTests(unittest.TestCase):
    def receipt(self, transport):
        spec = Case("mdbx", transport, "go", "forge")
        quic = transport == "quic"
        local = "/ip4/127.0.0.1/udp/41001/quic-v1" if quic else "/ip4/127.0.0.1/tcp/41001"
        remote = "/ip4/127.0.0.1/udp/41002/quic-v1" if quic else "/ip4/127.0.0.1/tcp/41002"
        session = {"local_peer_id": "forge-peer", "remote_peer_id": "go-peer",
                   "local_address": remote, "remote_address": local}
        payload = {"implementation": "go", "role": "dialer", "scenario": "echo_large", "status": "ok",
                   "echo_ok": True, "payload_bytes": len(spec.payload), "protocol": "/forge/interop/relay-echo/1",
                   "application_protocol": "/forge/interop/relay-echo/1", "application_connection_id": "connection-1",
                   "application_stream_id": "stream-1", "local_peer_id": "go-peer",
                   "authenticated_remote_peer_id": "forge-peer", "connection_local_addr": local,
                   "connection_remote_addr": remote, "negotiated_transport": "/quic-v1" if quic else "tcp",
                   "negotiated_security": "" if quic else "/tls/1.0.0" if transport == "tcp-tls" else "/noise",
                   "negotiated_muxer": "" if quic else "/yamux/1.0.0",
                   "identify_event_basis": "automatic_identify_separate_exchange_same_connection",
                   "identify_event_connection_id": "connection-1", "signed_peer_record": True}
        if spec.go_completion_barrier:
            payload.update(final=True, echo_completion={
                "source": "go-fixture.echo-completion-barrier.v1", "mode": "stop-file", "phase": "host_closed",
                "stop_observed": True, "stream_close_completed": True, "host_close_completed": True})
        return payload, spec, session

    def test_synthetic_published_facts_not_a_fabricated_upgrade_trace(self):
        for transport in ("tcp", "tcp-tls", "quic"):
            payload, spec, session = self.receipt(transport)
            self.assertNotIn("upgrade_observation", payload)
            _go_dialer(payload, spec, session)
            if transport != "quic":
                _tcp_donor(payload, spec, "dialer", session)

    def test_identity_socket_stream_and_identify_connection_must_correlate(self):
        mutations = (("local_peer_id", "other"), ("authenticated_remote_peer_id", "other"),
                     ("connection_local_addr", "/ip4/127.0.0.1/tcp/41002"),
                     ("connection_remote_addr", "/ip4/127.0.0.1/tcp/41999"),
                     ("identify_event_connection_id", "another-connection"),
                     ("identify_event_basis", "configured-identify"), ("signed_peer_record", False),
                     ("application_connection_id", ""), ("application_stream_id", ""),
                     ("application_stream_id", "connection-1"), ("application_protocol", "/ipfs/id/1.0.0"),
                     ("protocol", "/ipfs/id/1.0.0"), ("payload_bytes", True), ("payload_bytes", 1),
                     ("echo_ok", False), ("status", "pending"), ("role", "listener"),
                     ("scenario", "echo"), ("implementation", "rust"), ("upgrade_observation", {"complete": True}))
        for transport in ("tcp", "tcp-tls", "quic"):
            for key, bad in mutations:
                payload, spec, session = self.receipt(transport)
                payload[key] = bad
                with self.subTest(transport=transport, key=key), self.assertRaises(ValueError):
                    _go_dialer(payload, spec, session)

    def test_transport_security_and_muxer_are_actual_not_configured_labels(self):
        for transport in ("tcp", "tcp-tls", "quic"):
            for key, bad in (("negotiated_transport", "udp"), ("negotiated_security", "configured-tls"),
                             ("negotiated_muxer", "configured-yamux")):
                payload, spec, session = self.receipt(transport)
                payload[key] = bad
                with self.subTest(transport=transport, key=key), self.assertRaises(ValueError):
                    _go_dialer(payload, spec, session)

    def test_go_listener_still_needs_real_upgrade_observation(self):
        spec = Case("mdbx", "tcp", "forge", "go")
        _, _, session = self.receipt("tcp")
        payload = {"implementation": "go", "role": "listener", "scenario": "echo", "status": "ok"}
        with self.assertRaises(ValueError):
            _tcp_donor(payload, spec, "listener", session)


class GoCompletionBarrierTests(unittest.TestCase):
    def pair(self, transport="quic"):
        terminal, spec, session = GoPublishedReceiptTests().receipt(transport)
        witness = deepcopy(terminal)
        witness.update(status="exchange_observed", final=False, echo_completion={
            "source": "go-fixture.echo-completion-barrier.v1", "mode": "stop-file", "phase": "await_stop",
            "stop_observed": False, "stream_close_completed": False, "host_close_completed": False})
        return {"result": terminal, "exchange_witness": witness}, spec, session

    def test_tls_barrier_checks_both_stream_and_host_close(self):
        endpoint, spec, session = self.pair("tcp-tls")
        go_completion_pair(endpoint, spec, session)
        for field in ("stream_close_completed", "host_close_completed"):
            invalid = deepcopy(endpoint)
            invalid["result"]["echo_completion"][field] = False
            with self.subTest(field=field), self.assertRaises(ValueError):
                go_completion_pair(invalid, spec, session)

    def test_interim_actual_exchange_is_not_terminal_success(self):
        endpoint, spec, session = self.pair()
        go_completion_witness(endpoint["exchange_witness"], spec, session)
        go_completion_pair(endpoint, spec, session)
        with self.assertRaises(ValueError):
            _go_dialer(endpoint["exchange_witness"], spec, session)
        with self.assertRaises(ValueError):
            go_completion_witness(endpoint["result"], spec, session)

    def test_missing_stop_close_failure_and_relabel_never_complete(self):
        for mutate in (lambda p: p["echo_completion"].update(stop_observed=False),
                       lambda p: p["echo_completion"].update(stop_observed=1),
                       lambda p: p["echo_completion"].update(stream_close_completed=False),
                       lambda p: p["echo_completion"].update(stream_close_completed=1),
                       lambda p: p["echo_completion"].update(host_close_completed=False),
                       lambda p: p["echo_completion"].update(host_close_completed=1),
                       lambda p: p["echo_completion"].update(phase="await_stop"),
                       lambda p: p.update(final=False), lambda p: p.update(status="error"),
                       lambda p: p.update(error="close failed"), lambda p: p.pop("echo_completion")):
            endpoint, spec, session = self.pair()
            mutate(endpoint["result"])
            with self.assertRaises(ValueError):
                go_completion_pair(endpoint, spec, session)

    def test_numeric_zero_cannot_relabel_witness_lifecycle_flags(self):
        for field in ("stop_observed", "stream_close_completed", "host_close_completed"):
            endpoint, spec, session = self.pair()
            endpoint["exchange_witness"]["echo_completion"][field] = 0
            with self.subTest(field=field), self.assertRaises(ValueError):
                go_completion_pair(endpoint, spec, session)

    def test_witness_identity_socket_and_connection_must_match_native_and_terminal(self):
        for key, bad in (("local_peer_id", "other-peer"), ("authenticated_remote_peer_id", "other-peer"),
                         ("connection_local_addr", "/ip4/127.0.0.1/udp/41999/quic-v1"),
                         ("connection_remote_addr", "/ip4/127.0.0.1/udp/41999/quic-v1"),
                         ("application_stream_id", "another-stream"),
                         ("application_connection_id", "another-connection"),
                         ("identify_event_connection_id", "another-connection"), ("signed_peer_record", False)):
            endpoint, spec, session = self.pair()
            endpoint["exchange_witness"][key] = bad
            with self.subTest(key=key), self.assertRaises(ValueError):
                go_completion_pair(endpoint, spec, session)

    def test_plugin_pre_stop_close_witness_never_replaces_joined_receipt(self):
        spec = Case("mdbx", "quic", "go", "forge")
        payload = plugin_payload(spec, "listener")
        payload["exchange"]["session"].update(authentication="quic_tls", stream_authentication="quic_tls", muxer="",
            local_address="/ip4/127.0.0.1/udp/41002/quic-v1", remote_address="/ip4/127.0.0.1/udp/41001/quic-v1")
        payload.update(final=False, configuration_accepted=True,
                       lifecycle={"plugins_joined": False, "scheduler_joined": False})
        plugin_exchange_witness(payload, spec, "local")
        with self.assertRaises(ValueError):
            plugin_exchange(payload, spec, "listener", "local")
        for field in ("stream_closed", "response_write_completed"):
            value = deepcopy(payload)
            value["exchange"][field] = False
            with self.subTest(field=field), self.assertRaises(ValueError):
                plugin_exchange_witness(value, spec, "local")


if __name__ == "__main__":
    unittest.main()
