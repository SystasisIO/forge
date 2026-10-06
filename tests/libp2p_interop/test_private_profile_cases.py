"""Matrix and adapter regressions only, not executed donor acceptance."""

import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from types import SimpleNamespace
from unittest.mock import patch

from private_profile_cases import Case, case_specs, run_case
from private_profile_evidence import PRIVATE_CONTRACTS, INLINE_CONTRACTS


class PrivateProfileCasesTests(unittest.TestCase):
    def test_go_dialer_remains_owned_until_forge_reverse_stream_barrier(self):
        timeline = []
        dialer = SimpleNamespace(terminal_status={"exit_code": 0})
        server = SimpleNamespace(ready={"peer_id": "observed-peer",
                                       "listen_addrs": ["/ip4/127.0.0.1/tcp/4001/p2p/observed-peer"]},
                                 command=["forge", "listen"])
        def close(name):
            timeline.append(name)
            return []
        dialer.close = lambda: close("dialer-close")
        server.close = lambda: close("listener-close")
        server.evidence = lambda: {}
        def wait(path, timeout):
            if not timeline:
                timeline.append("dialer-result")
            elif timeline == ["dialer-result"]:
                self.assertEqual(path.name, "listener-result.json")
                self.assertFalse((path.parent / "dial.stop").exists())
                timeline.append("observed-reverse-stream")
            return {"status": "ok"}
        def control(*args, **kwargs):
            self.assertEqual(timeline[-2:], ["dialer-close", "listener-close"])
            return {}
        with TemporaryDirectory() as directory, patch("private_profile_cases.current_scope", return_value=object()), \
                patch("private_profile_cases.spawn_owned", return_value=dialer) as spawn:
            run_case(Case("tcp_yamux_private_pnet", "go", "forge"),
                     {"go": Path("go-fixture"), "forge": Path("forge-fixture")}, Path(directory),
                     key=Path("private-fixture"), mismatch=Path("mismatch-fixture"), fingerprint="a" * 64,
                     start_listener=lambda *args, **kwargs: server, wait_json=wait,
                     command_attempt=lambda *args: {}, run_control=control, effective_configuration=lambda *args: {})
            self.assertIn("--stop-file", spawn.call_args.args[0])
        self.assertEqual(timeline, ["dialer-result", "observed-reverse-stream", "dialer-close", "listener-close"])

    def test_exact_private_bilateral_matrix(self):
        specs = case_specs()
        self.assertEqual(len(specs), 30)
        self.assertEqual(len(set(specs)), 30)
        for name in PRIVATE_CONTRACTS:
            directions = {(s.dialer, s.listener) for s in specs if s.contract == name}
            expected = {("forge", "rust"), ("rust", "forge")}
            if name != "rendezvous_rust_private_tcp_yamux_pnet":
                expected |= {("forge", "go"), ("go", "forge")}
            self.assertEqual(directions, expected)

    def test_exact_inline_bilateral_matrix(self):
        specs = case_specs("inline-muxer")
        self.assertEqual(len(specs), 16)
        self.assertEqual(len(set(specs)), 16)
        for name in INLINE_CONTRACTS:
            donor = "rust" if name.startswith("inline_muxer_rust_") else "go"
            self.assertEqual({(s.dialer, s.listener) for s in specs if s.contract == name},
                             {("forge", donor), (donor, "forge")})
        self.assertEqual(len(case_specs("stage6")), 46)

    def test_exact_security_transport_not_an_implicit_flag(self):
        for spec in case_specs("stage6"):
            tls = "tls" in spec.contract
            expected = ("tcp-pnet-tls" if tls else "tcp-pnet-noise") if spec.private else ("tcp-tls" if tls else "tcp")
            self.assertEqual(spec.transport, expected)
            self.assertEqual(spec.runner_id, ("private_tcp_yamux_pnet/" if spec.private else "tcp_stage6/") + spec.contract)
        self.assertEqual(Case("tls_identity_private_pnet", "forge", "go").transport, "tcp-pnet-tls")

    def test_unknown_suite_cannot_expand_matrix(self):
        with self.assertRaises(ValueError):
            case_specs("typo")


if __name__ == "__main__":
    unittest.main()
