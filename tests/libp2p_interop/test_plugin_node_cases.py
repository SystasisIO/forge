"""Synthetic registration/launcher tests only; not live interop evidence."""

import unittest
from pathlib import Path
import tempfile
from unittest.mock import Mock, patch

from plugin_node_cases import Case, PRIVATE_PAYLOAD, _address, _wait_result, case_specs, command, listener_socket, run_case, socket


class RegistrationTests(unittest.TestCase):
    def test_closed_initial_matrix(self):
        for backend in ("mdbx", "rocksdb"):
            specs = case_specs(backend)
            self.assertEqual(len(specs), 42)
            self.assertEqual(len({spec.scenario_id for spec in specs}), 42)
            self.assertEqual(sum(not s.private and not s.dns for s in specs), 12)
            self.assertEqual(sum(s.dns for s in specs), 6)
            self.assertEqual(sum(s.private and not s.control for s in specs), 8)
            self.assertEqual(sum(bool(s.control) for s in specs), 16)
            self.assertTrue(all("forge" in (s.dialer, s.listener) for s in specs))
            self.assertTrue(all(s.dialer == "forge" for s in specs if s.dns))
            self.assertTrue(all(s.scenario_id.startswith("plugin-node/") for s in specs))

    def test_no_memory_fallback(self):
        with self.assertRaises(ValueError):
            case_specs("memory")

    def test_pinned_public_binary_pattern(self):
        spec = Case("mdbx", "tcp", "go", "forge")
        self.assertEqual(spec.donor_scenario, "echo_large")
        self.assertEqual(len(spec.payload), 192 * 1024)
        self.assertEqual(spec.payload[:251], bytes(range(251)))
        self.assertIn(0, spec.payload)

    def test_private_argv_bound_and_honest_binary_subset(self):
        spec = Case("mdbx", "tcp-pnet-tls", "rust", "forge")
        self.assertNotIn("\0", PRIVATE_PAYLOAD)
        self.assertIn(1, spec.payload)
        self.assertIn(127, spec.payload)
        self.assertTrue(any(byte > 127 for byte in spec.payload))
        self.assertLess(len(spec.payload), 8192)
        self.assertEqual(spec.donor_scenario, "tls_identity_private_pnet")

    def test_backend_flag_only_on_official_fixture(self):
        spec = Case("rocksdb", "tcp", "forge", "go")
        for implementation in ("forge", "go", "rust"):
            argv = command(spec, implementation, "listener", Path("/fixture"), Path("/case"))
            self.assertEqual("--store-backend" in argv, implementation == "forge")
            self.assertEqual(argv[argv.index("--scenario") + 1], "echo" if implementation == "go" else "echo_large")
            self.assertNotIn("--allow-insecure-test-mode", argv)

    def test_only_public_go_tcp_listener_uses_existing_echo_receipt(self):
        for spec in case_specs("mdbx"):
            for role, implementation in (("dialer", spec.dialer), ("listener", spec.listener)):
                selected = spec.endpoint_scenario(implementation, role)
                expected = "echo" if implementation == "go" and role == "listener" and not spec.private \
                    and spec.transport != "quic" else spec.donor_scenario
                self.assertEqual(selected, expected)
            if not spec.private:
                self.assertEqual(len(spec.payload), 192 * 1024)
                self.assertEqual(spec.payload[:251], bytes(range(251)))
                self.assertIn(0, spec.payload)

    def test_control_uses_unchanged_donor_flags_and_correlation(self):
        spec = Case("mdbx", "tcp-pnet-noise", "forge", "rust", control="missing_key")
        argv = command(spec, "forge", "dialer", Path("/fixture"), Path("/case"), peer="peer", address="addr")
        self.assertNotIn("--pnet-key-file", argv)
        self.assertEqual(argv[argv.index("--scenario") + 1], "pnet")
        self.assertEqual(argv[argv.index("--pnet-correlation") + 1], spec.scenario_id)

    def test_no_donor_dns_reconfiguration(self):
        spec = Case("mdbx", "tcp", "forge", "go", dns=True)
        with self.assertRaises(ValueError):
            command(spec, "go", "dialer", Path("/fixture"), Path("/case"), dns_server="127.0.0.1:53")

    def test_completion_flag_is_only_the_declared_public_dial_lanes(self):
        selected_cases = [spec for spec in case_specs("mdbx") if spec.go_completion_barrier]
        self.assertEqual({spec.transport for spec in selected_cases}, {"quic", "tcp-tls"})
        self.assertEqual(len(selected_cases), 2)
        for spec in case_specs("mdbx"):
            for role, implementation in (("dialer", spec.dialer), ("listener", spec.listener)):
                argv = command(spec, implementation, role, Path("/fixture"), Path("/case"))
                selected = role == "dialer" and spec.completion_barrier
                self.assertEqual("--echo-completion-barrier" in argv, selected)
                if selected:
                    self.assertEqual(argv[argv.index("--echo-completion-barrier") + 1], "stop-file")
                    self.assertEqual(argv[argv.index("--ready-file") + 1], "/case/dialer.ready.json")
        self.assertEqual([(s.transport, s.dialer, s.listener) for s in case_specs("mdbx") if s.rust_completion_barrier],
                         [("quic", "rust", "forge")])

    def test_orderly_fin_only_for_public_go_quic_listener(self):
        for spec in case_specs("mdbx"):
            for role, implementation in (("dialer", spec.dialer), ("listener", spec.listener)):
                argv = command(spec, implementation, role, Path("/fixture"), Path("/case"))
                selected = role == "listener" and spec.go_listener_completion
                self.assertEqual("--echo-listener-completion" in argv, selected)
                self.assertEqual("--echo-witness-file" in argv, selected)
                if selected:
                    self.assertEqual(argv[argv.index("--echo-listener-completion") + 1], "orderly-fin")
                    self.assertEqual(argv[argv.index("--echo-witness-file") + 1], "/case/listener.echo.json")


class SocketTests(unittest.TestCase):
    def test_exact_numeric_socket_and_one_owned_peer_suffix(self):
        for quic in (False, True):
            numeric = "/ip4/127.0.0.1/udp/41002/quic-v1" if quic else "/ip4/127.0.0.1/tcp/41002"
            self.assertEqual(socket(numeric, quic), numeric)
            address = numeric + "/p2p/listener"
            self.assertEqual(listener_socket(address, "listener", quic), numeric)
            ready = {"peer_id": "listener", "listen_addrs": [address]}
            self.assertEqual(_address(ready, "quic" if quic else "tcp"), address)

    def test_duplicate_missing_nonterminal_or_different_peer_never_normalized(self):
        for quic in (False, True):
            numeric = "/ip4/127.0.0.1/udp/41002/quic-v1" if quic else "/ip4/127.0.0.1/tcp/41002"
            for address in (numeric, numeric + "/p2p/", numeric + "/p2p/other",
                            numeric + "/p2p/listener/p2p/listener", numeric + "/p2p/listener/tcp/1"):
                with self.subTest(address=address), self.assertRaises(ValueError):
                    listener_socket(address, "listener", quic)
                with self.subTest(ready=address), self.assertRaises(ValueError):
                    _address({"peer_id": "listener", "listen_addrs": [address]}, "quic" if quic else "tcp")

    def test_non_numeric_non_loopback_wrong_transport_and_invalid_port_rejected(self):
        for quic in (False, True):
            template = "/ip4/127.0.0.1/udp/{}/quic-v1" if quic else "/ip4/127.0.0.1/tcp/{}"
            numeric = template.format(41002)
            values = [template.format(port) for port in (0, "01", -1, 65536, "NaN")]
            values += [numeric.replace("127.0.0.1", "127.0.0.2"), numeric.replace("ip4", "dns4"),
                       numeric.replace("/udp/", "/tcp/") if quic else numeric.replace("/tcp/", "/udp/"),
                       numeric + "/p2p/listener", None, True]
            for value in values:
                with self.subTest(value=value), self.assertRaises(ValueError):
                    socket(value, quic)


class CompletionFailureTests(unittest.TestCase):
    def test_inbound_callback_failure_without_exchange_is_immediate_not_timeout(self):
        path = Mock()
        path.is_file.return_value = True
        owner = Mock()
        receipt = {"status": "error", "final": False, "exchange": None, "errors": [{
            "phase": "protocol.inbound_echo.stream_close", "error_category": "system", "error_code": 89}]}
        with patch("plugin_node_cases._load", return_value=receipt), \
                patch("plugin_node_cases.time.sleep") as sleep:
            with self.assertRaisesRegex(RuntimeError, "endpoint failed; redacted receipt"):
                _wait_result(path, owner, {"ok"})
            sleep.assert_not_called()
            owner.process.poll.assert_not_called()

    def test_interim_witness_cannot_satisfy_terminal_wait(self):
        path = Mock()
        path.is_file.return_value = True
        owner = Mock()
        owner.process.poll.return_value = None
        with patch("plugin_node_cases._load", return_value={"status": "exchange_observed", "final": False}), \
                patch("plugin_node_cases.time.monotonic", side_effect=[0, 0, 36]), \
                patch("plugin_node_cases.time.sleep"):
            with self.assertRaises(TimeoutError):
                _wait_result(path, owner, {"ok"})


class CompletionOrderingTests(unittest.TestCase):
    def test_go_listener_eof_and_forge_close_precede_both_host_stops(self):
        for fail in (False, True):
            with self.subTest(fail=fail), tempfile.TemporaryDirectory() as directory:
                spec = Case("mdbx", "quic", "forge", "go")
                server, client = Mock(), Mock()
                sequence = []
                for owner, name in ((client, "forge"), (server, "go")):
                    owner.close.return_value = []
                    owner.evidence.return_value = {}
                    owner.request_stop.side_effect = lambda name=name: sequence.append("stop-" + name)
                def forge_witness(*args, **kwargs):
                    self.assertEqual(kwargs, {"role": "dialer"})
                    sequence.append("forge-close")
                    return {"session": {}}
                def eof_witness(*args, **kwargs):
                    self.assertEqual(kwargs, {"final": False})
                    sequence.append("go-eof-close")
                    if fail:
                        raise ValueError("no actual EOF")
                ready = {"peer_id": "listener", "listen_addrs": ["/ip4/127.0.0.1/udp/41002/quic-v1/p2p/listener"]}
                with patch("plugin_node_cases.current_scope", return_value=object()), \
                        patch("plugin_node_cases.spawn_owned", side_effect=[server, client]), \
                        patch("plugin_node_cases._wait_result", return_value={}), \
                        patch("plugin_node_cases._load", return_value={}), \
                        patch("plugin_node_evidence.plugin_exchange_witness", side_effect=forge_witness), \
                        patch("plugin_node_evidence.go_listener_completion", side_effect=eof_witness):
                    def run():
                        return run_case(spec, {"forge": Path("/forge"), "go": Path("/go")}, Path(directory),
                                        key=None, mismatch=None, fingerprint="", wait_json=lambda *_: ready,
                                        command_attempt=lambda *_: {})
                    if fail:
                        with self.assertRaisesRegex(ValueError, "no actual EOF"):
                            run()
                    else:
                        run()
                self.assertEqual(sequence, ["forge-close", "go-eof-close"] + ([] if fail else ["stop-forge", "stop-go"]))

    def exercise(self, directory, *, invalid_close=False, invalid_terminal=False, transport="quic"):
        spec = Case("mdbx", transport, "go", "forge")
        server, client = Mock(), Mock()
        for owner in (server, client):
            owner.close.return_value = []
            owner.evidence.return_value = {}
        sequence = []
        client.request_stop.side_effect = lambda: sequence.append("stop-go")
        server.request_stop.side_effect = lambda: sequence.append("stop-forge")
        client.close.side_effect = lambda: sequence.append("join-go") or []
        server.close.side_effect = lambda: sequence.append("join-forge") or []
        witness, plugin = {"actual": "go-exchange"}, {"actual": "forge-close"}
        terminal = {"actual": "go-terminal"}

        def forge_close(payload, given_spec, peer):
            sequence.append("validate-forge-write-and-close")
            self.assertIs(payload, plugin)
            self.assertEqual(given_spec, spec)
            self.assertEqual(peer, "listener")
            if invalid_close:
                raise ValueError("Forge async_close failed")
            return {"session": {"actual": "native-session"}}

        def go_witness(payload, given_spec, session):
            sequence.append("correlate-go-witness")
            self.assertIs(payload, witness)
            self.assertEqual(given_spec, spec)
            self.assertEqual(session, {"actual": "native-session"})

        def go_terminal(endpoint, given_spec, session):
            sequence.append("validate-go-terminal")
            self.assertEqual(endpoint, {"exchange_witness": witness, "result": terminal})
            self.assertEqual(given_spec, spec)
            self.assertEqual(session, {"actual": "native-session"})
            if invalid_terminal:
                raise ValueError("Go terminal close failed")

        address = ("/ip4/127.0.0.1/udp/41002/quic-v1" if transport == "quic" else "/ip4/127.0.0.1/tcp/41002")
        ready = {"peer_id": "listener", "listen_addrs": [address + "/p2p/listener"]}
        with patch("plugin_node_cases.current_scope", return_value=object()), \
                patch("plugin_node_cases.spawn_owned", side_effect=[server, client]), \
                patch("plugin_node_cases._wait_result", side_effect=[witness, plugin]), \
                patch("plugin_node_cases._load", return_value=terminal), \
                patch("plugin_node_evidence.plugin_exchange_witness", side_effect=forge_close), \
                patch("plugin_node_evidence.go_completion_witness", side_effect=go_witness), \
                patch("plugin_node_evidence.go_completion_pair", side_effect=go_terminal):
            if invalid_close or invalid_terminal:
                with self.assertRaisesRegex(ValueError, "Forge async_close failed" if invalid_close else "Go terminal close failed"):
                    run_case(spec, {"forge": Path("/forge"), "go": Path("/go")}, Path(directory),
                             key=None, mismatch=None, fingerprint="", wait_json=lambda *_: ready,
                             command_attempt=lambda *_: {})
            else:
                run_case(spec, {"forge": Path("/forge"), "go": Path("/go")}, Path(directory),
                         key=None, mismatch=None, fingerprint="", wait_json=lambda *_: ready,
                         command_attempt=lambda *_: {})
        return sequence, server, client

    def test_stop_only_after_both_independent_completion_witnesses(self):
        for transport in ("quic", "tcp-tls"):
            with self.subTest(transport=transport), tempfile.TemporaryDirectory() as directory:
                sequence, server, client = self.exercise(directory, transport=transport)
                self.assertEqual(sequence, ["validate-forge-write-and-close", "correlate-go-witness",
                                            "stop-go", "join-go", "validate-go-terminal", "stop-forge", "join-forge"])
                server.close.assert_called_once()
                client.close.assert_called_once()

    def test_failed_go_terminal_never_releases_normal_forge_shutdown(self):
        for transport in ("quic", "tcp-tls"):
            with self.subTest(transport=transport), tempfile.TemporaryDirectory() as directory:
                sequence, server, client = self.exercise(directory, transport=transport, invalid_terminal=True)
                self.assertEqual(sequence, ["validate-forge-write-and-close", "correlate-go-witness",
                                            "stop-go", "join-go", "validate-go-terminal"])
                client.close.assert_called_once()
                server.request_stop.assert_not_called()
                server.close.assert_not_called()

    def test_failed_forge_close_never_releases_success_barrier(self):
        with tempfile.TemporaryDirectory() as directory:
            sequence, server, client = self.exercise(directory, invalid_close=True)
            self.assertEqual(sequence, ["validate-forge-write-and-close"])
            server.request_stop.assert_not_called()
            client.request_stop.assert_not_called()


if __name__ == "__main__":
    unittest.main()
