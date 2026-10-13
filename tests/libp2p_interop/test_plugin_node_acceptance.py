"""Synthetic validator/recipe tests; these never launch a donor or prove interop."""

from copy import deepcopy
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from plugin_node_acceptance import registered_cases, validate_build_commands, validate_matrix
from plugin_node_cases import Case, command
from plugin_node_evidence import load_json, rust_build_environment, validate_terminal_sources
from runner import GO_EXECUTION_POLICY, prepare_rust_fixture


def build_provenance(build):
    cargo, go = "/tools/cargo", "/tools/go"
    environment = {"CARGO_NET_OFFLINE": "true", "RUSTUP_OFFLINE": "true", "CARGO_BUILD_JOBS": "4",
                   "CARGO_INCREMENTAL": "0", "CARGO_PROFILE_DEV_DEBUG": "0", "CARGO_PROFILE_TEST_DEBUG": "0"}
    return {"tools": {"cargo": {"path": cargo}, "go": {"path": go}}, "binaries": {
        "go": {"path": str(build / "go_fixture/go_fixture")},
        "rust": {"path": str(build / "rust_fixture/target/release/forge-libp2p-rust-fixture")}}, "commands": [
        {"command": argv, "cwd": str(build / "go_fixture"), "environment": dict(GO_EXECUTION_POLICY)} for argv in (
            [go, "mod", "verify"], [go, "test", "-mod=readonly", "-count=1", "-timeout=60s", "."],
            [go, "build", "-mod=readonly", "-trimpath", "-o", str(build / "go_fixture/go_fixture"), "."])] + [
        {"command": argv, "cwd": str(build / "rust_fixture"), "environment": dict(environment)} for argv in (
            [cargo, "test", "--frozen"], [cargo, "build", "--release", "--frozen"])]}


def terminal_record(root, spec=None):
    """Minimal source-binding input, deliberately not a semantic acceptance record."""
    spec = spec or Case("mdbx", "tcp", "forge", "go")
    binaries = {"forge": Path("/hashed/forge"), "go": Path("/hashed/go"), "rust": Path("/hashed/rust")}
    address, peer = ("/ip4/127.0.0.1/udp/41002/quic-v1/p2p/listener" if spec.transport == "quic"
                     else "/ip4/127.0.0.1/tcp/41002/p2p/listener"), "listener"
    endpoints = {}
    for role, implementation, pid in (("dialer", spec.dialer, 1001), ("listener", spec.listener, 1002)):
        argv = command(spec, implementation, role, binaries[implementation], root, peer=peer, address=address)
        options = dict(zip(argv[2::2], argv[3::2]))
        payload = {"status": "ok"}
        if implementation == "forge":
            payload["accepted_configuration"] = {"document": {"plugins": {"db": {"store": {
                "stores": [{"path": options["--store-dir"]}]}}}}}
        path = Path(options["--result-file"])
        path.write_text(json.dumps(payload))
        captured = root / (role + ".result.capture.json")
        captured.write_text(json.dumps(payload))
        outputs = [{"argument": "--result-file", "path": str(path), "exists": True, "log_file": str(captured)}]
        ready = {}
        if "--ready-file" in options:
            ready = {"peer_id": peer, "listen_addrs": [address]} if role == "listener" else {
                "status": "exchange_observed", "final": False}
            ready_path = Path(options["--ready-file"])
            ready_path.write_text(json.dumps(ready))
            ready_capture = root / (role + ".ready.capture.json")
            ready_capture.write_text(json.dumps(ready))
            outputs.append({"argument": "--ready-file", "path": str(ready_path), "exists": True,
                            "log_file": str(ready_capture)})
        endpoints[role] = {"implementation": implementation, "result": payload, "result_file": str(path), "process": {
            "pid": pid, "command": argv, "ready": ready, "outputs": outputs,
            "terminal_status": {"exit_code": 0, "termination": "graceful"}}}
        if role == "dialer" and spec.completion_barrier:
            endpoints[role].update(exchange_witness=deepcopy(ready), exchange_witness_file=str(ready_path))
        if role == "listener" and spec.go_listener_completion:
            witness = {"status": "ok", "final": False}
            witness_path = Path(options["--echo-witness-file"])
            witness_path.write_text(json.dumps(witness))
            captured = root / "listener.echo.capture.json"
            captured.write_text(json.dumps(witness))
            outputs.append({"argument": "--echo-witness-file", "path": str(witness_path), "exists": True,
                            "log_file": str(captured)})
            endpoints[role].update(listener_witness=witness, listener_witness_file=str(witness_path))
    record = {"spec": vars(spec), "endpoints": endpoints, "peer_id": peer, "listen_address": address,
              "test_key_hashes": {"dialer": "", "listener": ""},
              "owned_processes": [deepcopy(endpoint["process"]) for endpoint in endpoints.values()]}
    return record, binaries


class MatrixRegistrationTests(unittest.TestCase):
    def records(self):
        return [{"scenario_id": scenario_id, "status": "observed"} for scenario_id in registered_cases(["mdbx"])]

    def test_initial_registry_only_and_no_memory_fallback(self):
        self.assertEqual(len(registered_cases(["mdbx"])), 42)
        self.assertEqual(len(registered_cases(["mdbx", "rocksdb"])), 84)
        for backends in ([], ["memory"], ["mdbx", "mdbx"], [True], [["mdbx"]]):
            with self.subTest(backends=backends), self.assertRaises(ValueError):
                registered_cases(backends)

    def test_closed_registry_not_a_live_matrix_proof(self):
        # Isolate registration from protocol evidence, which has separate tests.
        with patch("plugin_node_acceptance.validate_case", return_value=[]):
            self.assertEqual(validate_matrix(self.records(), ["mdbx"]), [])
            missing = self.records()[:-1]
            duplicate = self.records() + [self.records()[0]]
            foreign = self.records() + [{"scenario_id": "stage6/echo", "status": "observed"}]
            malformed = self.records() + [{"scenario_id": [], "status": "observed"}]
            not_observed = self.records()
            not_observed[0]["status"] = "ok"
            for records in (missing, duplicate, foreign, malformed, not_observed):
                with self.subTest(records=len(records)):
                    self.assertTrue(validate_matrix(records, ["mdbx"]))


class CompilationReceiptTests(unittest.TestCase):
    def test_actual_inherited_overrides_without_credentials_or_invented_defaults(self):
        build = Path("/unit/build")
        inherited = {**build_provenance(build)["commands"][-1]["environment"], "PATH": "/tools",
                     "CARGO_REGISTRIES_PRIVATE_TOKEN": "never-record-this", "OTHER_SECRET": "never-record-this"}
        original = dict(inherited)
        with patch("runner.refresh_rust_fixture_source"), patch("runner.run") as invoke:
            binary, receipts = prepare_rust_fixture(Path("/unit/source"), build, "/tools/cargo", inherited,
                                                    record_build_environment=True)
        self.assertEqual(binary, build / "rust_fixture/target/release/forge-libp2p-rust-fixture")
        self.assertEqual(inherited, original)
        self.assertEqual(invoke.call_count, 2)
        for receipt, call in zip(receipts, invoke.call_args_list):
            self.assertEqual(call.kwargs["env"], inherited)
            self.assertEqual(receipt["command"], call.args[0])
            self.assertEqual(receipt["cwd"], str(call.kwargs["cwd"]))
            self.assertEqual(receipt["environment"], rust_build_environment(call.kwargs["env"]))
            for name in ("CARGO_INCREMENTAL", "CARGO_BUILD_JOBS", "CARGO_PROFILE_DEV_DEBUG", "CARGO_PROFILE_TEST_DEBUG"):
                self.assertEqual(receipt["environment"][name], inherited[name])
            self.assertNotIn("CARGO_PROFILE_RELEASE_DEBUG", receipt["environment"])
            self.assertNotIn("never-record-this", json.dumps(receipt))

    def test_missing_or_false_actual_offline_environment_rejected(self):
        for environment in ({}, {"CARGO_NET_OFFLINE": "true", "RUSTUP_OFFLINE": "false"}):
            with self.subTest(environment=environment), self.assertRaises(ValueError):
                rust_build_environment(environment)

    def test_build_recipe_binds_tests_before_release_and_real_paths(self):
        build = Path("/unit/build")
        self.assertEqual(validate_build_commands(build_provenance(build), build), [])
        for mutation in (lambda p: p["commands"].pop(), lambda p: p["commands"].reverse(),
                         lambda p: p["commands"][-1]["environment"].update(CARGO_INCREMENTAL="1"),
                         lambda p: p["commands"][-1]["environment"].update(CARGO_NET_OFFLINE="false"),
                         lambda p: p["commands"][-1]["environment"].update(CARGO_REGISTRIES_PRIVATE_TOKEN="secret"),
                         lambda p: p["commands"][-1].update(cwd="/different/source"),
                         lambda p: p["commands"][0]["environment"].update(GOPROXY="online"),
                         lambda p: p["binaries"]["rust"].update(path="/other/prebuilt-fixture")):
            receipt = build_provenance(build)
            mutation(receipt)
            self.assertTrue(validate_build_commands(receipt, build))


class TerminalSourceTests(unittest.TestCase):
    def test_listener_eof_witness_has_its_own_owned_capture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            record, binaries = terminal_record(root, Case("mdbx", "quic", "forge", "go"))
            self.assertEqual(validate_terminal_sources(record, binaries, root, root / "source"), [])
            for change in (lambda e: e.pop("listener_witness"),
                           lambda e: e["listener_witness"].update(final=True),
                           lambda e: e.update(listener_witness_file=e["result_file"]),
                           lambda e: e["process"]["outputs"].pop()):
                broken = deepcopy(record)
                change(broken["endpoints"]["listener"])
                broken["owned_processes"][1] = deepcopy(broken["endpoints"]["listener"]["process"])
                self.assertTrue(validate_terminal_sources(broken, binaries, root, root / "source"))

    def test_barrier_witness_and_terminal_sources_are_separate_owned_captures(self):
        for donor in ("go", "rust"):
            with self.subTest(donor=donor), tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                record, binaries = terminal_record(root, Case("rocksdb", "quic", donor, "forge"))
                self.assertEqual(validate_terminal_sources(record, binaries, root, root / "source"), [])

    def test_barrier_rejects_missing_flag_witness_capture_or_changed_witness(self):
        def missing_flag(endpoint):
            argv = endpoint["process"]["command"]
            index = argv.index("--echo-completion-barrier")
            del argv[index:index + 2]

        for mutate in (missing_flag, lambda e: e.pop("exchange_witness"),
                       lambda e: e["exchange_witness"].update(status="ok"),
                       lambda e: e.update(exchange_witness_file=e["result_file"]),
                       lambda e: e["process"]["outputs"].pop()):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                record, binaries = terminal_record(root, Case("mdbx", "quic", "go", "forge"))
                mutate(record["endpoints"]["dialer"])
                record["owned_processes"][0] = deepcopy(record["endpoints"]["dialer"]["process"])
                self.assertTrue(validate_terminal_sources(record, binaries, root, root / "source"))

    def test_minimal_source_binding_not_semantic_protocol_acceptance(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            record, binaries = terminal_record(root)
            self.assertEqual(validate_terminal_sources(record, binaries, root, root / "source"), [])

    def test_sources_reject_relabel_missing_owner_duplicate_capture_and_ready_tamper(self):
        def ready_tamper(record):
            record["endpoints"]["listener"]["process"]["ready"]["peer_id"] = "invented-peer"
            record["owned_processes"][1] = deepcopy(record["endpoints"]["listener"]["process"])

        for mutation in (lambda r: r["owned_processes"].pop(),
                         lambda r: r["endpoints"]["dialer"]["process"]["command"].__setitem__(0, "/raw/node-fixture"),
                         lambda r: r["owned_processes"].__setitem__(1, deepcopy(r["owned_processes"][0])),
                         lambda r: r["endpoints"]["dialer"]["process"]["outputs"].append(
                             deepcopy(r["endpoints"]["dialer"]["process"]["outputs"][0])), ready_tamper):
            with tempfile.TemporaryDirectory() as directory:
                root = Path(directory).resolve()
                record, binaries = terminal_record(root)
                mutation(record)
                self.assertTrue(validate_terminal_sources(record, binaries, root, root / "source"))

    def test_terminal_capture_not_pending_inline_receipt(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            record, binaries = terminal_record(root)
            captured = Path(record["endpoints"]["dialer"]["process"]["outputs"][0]["log_file"])
            captured.write_text('{"status":"pending"}')
            self.assertTrue(validate_terminal_sources(record, binaries, root, root / "source"))

    def test_go_listener_scenario_is_bound_to_its_actual_command(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            record, binaries = terminal_record(root)
            process = record["endpoints"]["listener"]["process"]
            self.assertEqual(process["command"][process["command"].index("--scenario") + 1], "echo")
            process["command"][process["command"].index("--scenario") + 1] = "echo_large"
            record["owned_processes"][1] = deepcopy(process)
            self.assertTrue(validate_terminal_sources(record, binaries, root, root / "source"))

    def test_duplicate_and_nonfinite_json_never_validate(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.json"
            for contents in ('{"status":"ok","status":"failed"}', '{"count":NaN}'):
                path.write_text(contents)
                with self.subTest(contents=contents), self.assertRaises(ValueError):
                    load_json(path)


if __name__ == "__main__":
    unittest.main()
