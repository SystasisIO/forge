"""Registration and canonical launcher regressions, not live proof."""

import unittest
import ast
import copy
import json
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

import check_stage6_acceptance as checker
from check_stage6_acceptance import (
    EVIDENCE_CONTRACT_VALIDATORS, expected_launcher_transport,
    private_control_process_sources, validate_pnet_launchers,
)
from private_profile_acceptance import SCENARIOS, expected_contract, validate_requirements
from private_profile_cases import case_specs
from stage6_evidence_contract import evidence_contract_for


class PrivateProfileAcceptanceTests(unittest.TestCase):
    def required(self, suite):
        return {(SCENARIOS[s.contract], s.contract): expected_contract(s.contract) for s in case_specs(suite)}

    def test_exact_registration_and_mapping(self):
        for suite in ("private-profile", "inline-muxer"):
            self.assertEqual(validate_requirements(self.required(suite), suite), [])
            for spec in case_specs(suite):
                value = expected_contract(spec.contract)
                contract = evidence_contract_for(spec.contract)
                self.assertIn(contract, EVIDENCE_CONTRACT_VALIDATORS)
                self.assertEqual(expected_launcher_transport(value[2], value[3], contract), spec.transport)
                wrong = "native" if spec.private else "private_network"
                self.assertIsNone(expected_launcher_transport(wrong, value[3], contract))
        self.assertEqual(expected_launcher_transport("private_network", ("tcp", "pnet", "yamux"),
                                                     evidence_contract_for("pnet")), "tcp-pnet")

    def test_missing_direction_owner_dependency_or_status_is_rejected(self):
        for suite in ("private-profile", "inline-muxer"):
            for index, bad in ((0, {"forge_to_go"}), (1, "invented"), (2, "native"),
                               (3, ("quic",)), (4, "tcp_stage6/pnet"), (5, ()), (6, "label-only")):
                required = self.required(suite)
                key = next(k for k in required if k[1].endswith("private_pnet") or k[1] == "tcp_yamux_private_pnet")
                value = list(required[key])
                if value[index] == bad:
                    bad = ("wrong",)
                value[index] = bad
                required[key] = tuple(value)
                self.assertTrue(validate_requirements(required, suite), (suite, index))
            required = self.required(suite)
            key = next(iter(required))
            value = required.pop(key)
            required[("wrong.owner", key[1])] = value
            self.assertTrue(validate_requirements(required, suite))
            required = self.required(suite)
            required.pop(next(iter(required)))
            self.assertTrue(validate_requirements(required, suite))

    def test_no_spurious_requirement_when_old_suites_run(self):
        self.assertEqual(validate_requirements({}, "stage6"), [])
        self.assertEqual(validate_requirements({}, "autonat"), [])

    def test_runner_literal_inventory_covers_every_exact_helper_case(self):
        tree = ast.parse(Path(__file__).with_name("runner.py").read_text())
        tables = {statement.targets[0].id: ast.literal_eval(statement.value)
                  for statement in tree.body if isinstance(statement, ast.Assign)
                  and len(statement.targets) == 1 and isinstance(statement.targets[0], ast.Name)
                  and statement.targets[0].id in {"LIVE_SCENARIO_PROFILES", "CURRENT_ACCEPTANCE_SCENARIOS"}}
        profiles = tables["LIVE_SCENARIO_PROFILES"]
        registrations = tables["CURRENT_ACCEPTANCE_SCENARIOS"]
        for spec in case_specs("stage6"):
            with self.subTest(contract=spec.contract):
                profile, name = spec.runner_id.split("/", 1)
                self.assertIn(name, profiles[profile])
                self.assertEqual(registrations[spec.runner_id], (spec.contract,))

    def test_private_controls_use_exact_security_transport_and_closed_argv(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            fingerprint = "a" * 64
            key = str(root.parent / "not-read-positive.key")
            mismatch = str(root.parent / "not-read-other.key")

            def key_fingerprint(value, _root):
                return (fingerprint if value == key else "b" * 64 if value == mismatch else None, [])

            def argv(binary, mode, options):
                return [binary, mode, *[part for option in options.items() for part in option]]

            for spec in {s.contract: s for s in case_specs("stage6") if s.private}.values():
                record = {"acceptance_scenario_id": spec.contract,
                          "result": {"attempts": [{"command": ["/go-fixture", "dial"]}]},
                          "listener_process": {"command": ["/forge-fixture", "listen"]}}
                positive = {"--pnet-key-file": key, "--pnet-fingerprint": fingerprint}
                listener_options = dict(positive, **{"--features": "ping,identify"})
                result = {"pnet_fingerprint": fingerprint}
                for kind in ("missing_key", "mismatched_key"):
                    stem = root / kind
                    address = "/ip4/127.0.0.1/tcp/42000"
                    dial = {"--scenario": "pnet", "--peer-id": "listener-peer", "--addr": address,
                            "--result-file": str(stem) + ".dial.json", "--store-dir": str(stem) + ".dial-store",
                            "--transport": spec.transport, "--pnet-fingerprint": fingerprint,
                            "--pnet-control": kind, "--pnet-correlation": kind}
                    if kind == "mismatched_key":
                        dial["--pnet-key-file"] = mismatch
                    listen = {"--ready-file": str(stem) + ".ready.json", "--stop-file": str(stem) + ".stop",
                              "--store-dir": str(stem) + ".listen-store", "--features": "ping,identify",
                              "--transport": spec.transport, "--scenario": "pnet", "--result-file": str(stem) + ".listen.json",
                              "--pnet-key-file": key, "--pnet-fingerprint": fingerprint,
                              "--pnet-control": kind, "--pnet-correlation": kind}
                    record[kind] = {"result": {"correlation_token": kind, "expected_peer_id": "listener-peer",
                                                "attempts": [{"command": argv("/go-fixture", "dial", dial)}]},
                                    "listener_process": {"command": argv("/forge-fixture", "listen", listen),
                                                         "listen_addrs": [address]}}
                with patch("check_stage6_acceptance.pnet_fingerprint_for_launcher_key", side_effect=key_fingerprint):
                    def validate(value):
                        return validate_pnet_launchers(value, result, result, positive, listener_options, root)

                    self.assertEqual(validate(record), [], spec.contract)
                    for mode in ("dial", "listen"):
                        for flag, value in (("--transport", "tcp-pnet"), ("--transport", "tcp-pnet-tls" if spec.transport.endswith("noise") else "tcp-pnet-noise"),
                                            ("--security", "tls"), ("--pnet-correlation", "other"), ("--store-dir", "/outside-artifacts")):
                            mutated = copy.deepcopy(record)
                            control = mutated["missing_key"]
                            command = (control["result"]["attempts"][0] if mode == "dial" else control["listener_process"])["command"]
                            if flag in command:
                                command[command.index(flag) + 1] = value
                            else:
                                command.extend([flag, value])
                            self.assertTrue(validate(mutated), (spec.contract, mode, flag, value))
                    for bad in (None, [], [None], [{}], [{}, {}]):
                        mutated = copy.deepcopy(record)
                        mutated["mismatched_key"]["result"]["attempts"] = bad
                        self.assertTrue(validate(mutated), (spec.contract, bad))

    def test_rejection_counter_files_must_be_terminal_owned_and_correlated(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            record = {"dialer":"go", "listener":"forge", "owned_processes":[]}
            indexed = {}
            for kind in ("missing_key", "mismatched_key"):
                control = {}
                for number, mode in enumerate(("dial", "listen"), 1):
                    role = "dialer" if mode == "dial" else "listener"
                    implementation = "go" if mode == "dial" else "forge"
                    stem = root / f"{kind}-{mode}"
                    log = Path(str(stem) + ".log")
                    output = Path(str(stem) + ".json")
                    snapshot = Path(str(log) + ".result-file.json")
                    payload = {"status":"rejected", "implementation":implementation, "role":role,
                               "correlation_token":kind, "control_kind":kind}
                    output.write_text(json.dumps(payload)); snapshot.write_text(json.dumps(payload)); log.touch()
                    command = ["/fixture", mode, "--scenario", "pnet", "--result-file", str(output),
                               "--pnet-control", kind, "--pnet-correlation", kind]
                    if mode == "dial":
                        command += ["--peer-id", "listener-peer", "--addr", "/ip4/127.0.0.1/tcp/42000"]
                    owner = {"pid":number + 10 * len(record["owned_processes"]), "command":command,
                             "log_file":str(log), "terminal_status":{"exit_code":0, "termination":"graceful"},
                             "outputs":[{"argument":"--result-file", "path":str(output), "log_file":str(snapshot), "exists":True}],
                             "ready":{}}
                    for path in (log, output, snapshot): indexed[path] = path.name
                    if mode == "listen":
                        ready_path = Path(str(stem) + ".ready.json")
                        ready_snapshot = Path(str(log) + ".ready-file.json")
                        ready = {"status":"ready", "implementation":implementation, "role":"listener", "peer_id":"listener-peer",
                                 "listen_addrs":["/ip4/127.0.0.1/tcp/42000"]}
                        ready_path.write_text(json.dumps(ready)); ready_snapshot.write_text(json.dumps(ready))
                        owner["command"] += ["--ready-file", str(ready_path)]
                        owner["ready"] = ready
                        owner["outputs"].append({"argument":"--ready-file", "path":str(ready_path),
                                                 "log_file":str(ready_snapshot), "exists":True})
                        indexed[ready_snapshot] = ready_snapshot.name
                        control["listener_process"] = dict(owner, peer_id="listener-peer", listen_addrs=ready["listen_addrs"])
                        control["listener_result"] = dict(payload, result_file=str(output))
                    else:
                        control["result"] = dict(payload, result_file=str(output), attempts=[dict(owner)])
                    record["owned_processes"].append(owner)
                record[kind] = control
            self.assertEqual(private_control_process_sources(record, root, indexed, set()), [])
            snapshot = Path(record["missing_key"]["result"]["attempts"][0]["outputs"][0]["log_file"])
            snapshot.write_text(json.dumps({"status":"rejected", "correlation_token":"other"}))
            self.assertTrue(private_control_process_sources(record, root, indexed, set()))
            snapshot.write_text(json.dumps({key:value for key,value in record["missing_key"]["result"].items()
                                            if key not in {"result_file", "attempts"}}))
            record["owned_processes"][0]["terminal_status"] = {"exit_code":0, "termination":"terminated"}
            self.assertTrue(private_control_process_sources(record, root, indexed, set()))
            for field in ("listener_process", "result"):
                malformed = copy.deepcopy(record)
                malformed["missing_key"][field] = None
                self.assertTrue(private_control_process_sources(malformed, root, indexed, set()))


class FocusedPromotionControlFlowTests(unittest.TestCase):
    """Synthetic scope regressions; worker semantics/provenance have separate tests.

    The execution receipt, manifest, argv, indexed files and record selection run
    through validate() unchanged. No native process or compiler is executed.
    """

    def fixture(self, directory, suite):
        root = Path(directory) / "source"
        source = root / checker.CANONICAL_RUNNER.parent
        source.mkdir(parents=True)
        (root / checker.CANONICAL_RUNNER).write_text("# synthetic unit runner path\n")
        build = Path(directory) / "build"
        artifact_root = build / ("interop-run" if suite == "stage6" else f"{suite}-run")
        artifact_root.mkdir(parents=True)
        donors = Path(directory) / "donors"
        donors.mkdir()
        selected = "private-profile" if suite in {"stage6", "autonat"} else suite
        specs = case_specs(selected)
        capabilities = {}
        for spec in specs:
            entry = capabilities.setdefault(SCENARIOS[spec.contract], {"scenarios": []})
            if any(s["id"] == spec.contract for s in entry["scenarios"]):
                continue
            directions, status, profile, stack, runner, requires, contract = expected_contract(spec.contract)
            entry["scenarios"].append({
                "id": spec.contract, "runner_scenario_id": runner, "profile": profile,
                "transport_stack": list(stack), "activation": "enabled", "registration": "registered",
                "source_case_id": spec.contract, "evidence_contract": contract,
                "required_directions": sorted(directions), "expected_status": status,
                "requires_capabilities": list(requires),
            })
            if status == "limited":
                entry["limitation"] = {"scope": "synthetic Rust fallback unit requirement"}
        manifest = {"interop_acceptance_registry": {
            "artifact_schema": checker.ARTIFACT_SCHEMA,
            "evidence_contracts": sorted({evidence_contract_for(s.contract) for s in specs}),
            "capabilities": capabilities,
        }}
        manifest_path = source / "p2p_donor_capabilities.json"
        manifest_path.write_text(json.dumps(manifest))
        binaries = {}
        for implementation in ("forge", "go", "rust"):
            binary = build / f"{implementation}-fixture"
            binary.write_text(f"synthetic unit {implementation}, not executable\n")
            binaries[implementation] = {"path": str(binary), "sha256": checker.sha256_file(binary)}
        records = []
        for spec in specs:
            payload = {"status": "ok", "scenario": spec.contract}
            result = artifact_root / f"{spec.contract}-{spec.dialer}-{spec.listener}.json"
            result.write_text(json.dumps(payload))
            records.append({"dialer": spec.dialer, "listener": spec.listener,
                            "acceptance_scenario_id": spec.contract,
                            "result": dict(payload, result_file=str(result))})
        identity = {"head": "a" * 40, "worktree_sha256": "b" * 64, "dirty": False,
                    "exact_identity": "git:" + "a" * 40 + ";worktree-sha256:" + "b" * 64}
        inputs = {"suite": suite, "source_dir": str(source), "build_dir": str(build), "forge_root": str(root),
                  "donors_root": str(donors), "acceptance_manifest": str(manifest_path)}
        argv = [str(Path(sys.executable).resolve()), str(root / checker.CANONICAL_RUNNER)]
        values = {"--enabled": "1", "--forge-fixture": binaries["forge"]["path"],
                  **{"--" + key.replace("_", "-"): value for key, value in inputs.items() if key != "suite"}}
        for flag in checker.RUNNER_FLAGS:
            argv.extend([flag, values[flag]])
        if suite != "stage6":
            argv.extend(["--suite", suite])
        started = time.time() - 2
        artifact = {
            "schema_version": 2, "runner_argv": argv, "started_at_unix": started,
            "finished_at_unix": started + 1, "artifact_root": str(artifact_root),
            "acceptance_manifest": {"path": str(manifest_path), "sha256": checker.sha256_file(manifest_path)},
            "fixture_provenance": {
                "forge_worktree": {"start": identity, "end": identity, "changed_during_run": False},
                "fixture_build_info": {"schema_version": 2, "forge": identity,
                                       "compiler": {"path": "/unit/clang", "id": "Clang", "version": "22.1.8"},
                                       "build_profile": "Release"},
                "tools": {"python": {"path": str(Path(sys.executable).resolve()),
                                     "version_output": subprocess.check_output(
                                         [str(Path(sys.executable).resolve()), "--version"], text=True).strip()}},
                "binaries": binaries, "runner_inputs": inputs,
            },
            "artifacts": records, "failures": [], "evidence_index": [],
        }
        artifact_path = build / ("interop-artifacts.json" if suite == "stage6" else f"{suite}-artifacts.json")
        return root, manifest_path, artifact_path, artifact, identity

    def validate_fixture(self, fixture, *, with_receipt=True, receipt_updates=None):
        root, manifest, path, artifact, identity = fixture
        artifact["evidence_index"] = checker.build_evidence_index(Path(artifact["artifact_root"]), artifact["artifacts"])
        path.write_text(json.dumps(artifact))
        receipt = {
            "schema_version": 2, "runner_argv": artifact["runner_argv"], "returncode": 0,
            "started_at_unix": artifact["started_at_unix"] - 0.1,
            "finished_at_unix": artifact["finished_at_unix"] + 0.1,
            "invocation_directory": str(path.parent), "artifact_path": str(path),
            "artifact_sha256": checker.sha256_file(path),
        }
        receipt.update(receipt_updates or {})
        with patch.object(checker, "validate_git_state", return_value=(artifact["started_at_unix"] - 10, [])), \
                patch.object(checker, "worktree_identity", return_value=SimpleNamespace(as_json=lambda: identity)), \
                patch.object(checker, "validate_donor_provenance", return_value=[]), \
                patch.object(checker, "validate_successful_raw_record", return_value=[]) as worker, \
                patch.object(checker, "validate_autonat_suite", wraps=checker.validate_autonat_suite) as autonat:
            errors, limited = checker.validate(root, manifest, path, identity["head"],
                                               receipt if with_receipt else None,
                                               expected_suite=artifact["fixture_provenance"]["runner_inputs"]["suite"])
        return errors, limited, worker.call_count, autonat.call_count

    def test_focused_execution_receipts_do_not_require_autonat(self):
        for suite, count in (("private-profile", 30), ("inline-muxer", 16)):
            with self.subTest(suite=suite), tempfile.TemporaryDirectory() as directory:
                errors, limited, workers, autonat = self.validate_fixture(self.fixture(directory, suite))
                self.assertEqual(errors, [])
                self.assertEqual(limited, suite == "inline-muxer")
                self.assertEqual(workers, count)
                self.assertEqual(autonat, 0)

    def test_stage6_receipt_and_autonat_scope_still_require_all_41(self):
        for suite, with_receipt in (("stage6", True), ("autonat", False)):
            with self.subTest(suite=suite), tempfile.TemporaryDirectory() as directory:
                errors, _, _, autonat = self.validate_fixture(self.fixture(directory, suite), with_receipt=with_receipt)
                self.assertEqual(autonat, 1)
                self.assertTrue(any("AutoNAT receipt must cover all 41 cases" in error for error in errors), errors)

    def test_focused_receipt_hash_returncode_and_interval_are_still_checked(self):
        for suite in ("private-profile", "inline-muxer"):
            for change in ({"artifact_sha256": "0" * 64}, {"returncode": 1}, {"started_at_unix": time.time() + 60}):
                with self.subTest(suite=suite, receipt=change), tempfile.TemporaryDirectory() as directory:
                    errors, _, _, autonat = self.validate_fixture(self.fixture(directory, suite), receipt_updates=change)
                    self.assertTrue(any("promotion execution receipt does not bind" in error for error in errors), errors)
                    self.assertEqual(autonat, 0)

    def test_focused_suites_reject_every_unclaimed_record_without_result_file(self):
        extras = (None, {"status": "failed", "errors": ["synthetic failure"]},
                  {"acceptance_scenario_id": "unknown", "dialer": "forge", "listener": "go"})
        for suite in ("private-profile", "inline-muxer"):
            for extra in extras:
                for with_receipt in (False, True):
                    with self.subTest(suite=suite, extra=extra, receipt=with_receipt), tempfile.TemporaryDirectory() as directory:
                        fixture = self.fixture(directory, suite)
                        fixture[3]["artifacts"].append(copy.deepcopy(extra))
                        errors, _, _, _ = self.validate_fixture(fixture, with_receipt=with_receipt)
                        self.assertTrue(any("must cover every record exactly once" in error for error in errors), errors)

    def test_focused_suites_reject_missing_or_duplicate_required_record(self):
        for suite in ("private-profile", "inline-muxer"):
            for change in ("missing", "duplicate"):
                with self.subTest(suite=suite, change=change), tempfile.TemporaryDirectory() as directory:
                    fixture = self.fixture(directory, suite)
                    records = fixture[3]["artifacts"]
                    if change == "missing":
                        records.pop()
                    else:
                        records.append(copy.deepcopy(records[-1]))
                    errors, _, _, _ = self.validate_fixture(fixture)
                    self.assertTrue(any("lacks one canonical raw runner record" in error for error in errors), errors)

    def test_autonat_records_in_focused_suites_are_not_silenced(self):
        for suite in ("private-profile", "inline-muxer"):
            with self.subTest(suite=suite), tempfile.TemporaryDirectory() as directory:
                fixture = self.fixture(directory, suite)
                fixture[3]["artifacts"].append({"suite": "autonat"})
                errors, _, _, autonat = self.validate_fixture(fixture)
                self.assertEqual(autonat, 1)
                self.assertTrue(any("AutoNAT receipt must cover all 41 cases" in error for error in errors), errors)
                self.assertTrue(any("must cover every record exactly once" in error for error in errors), errors)

    def test_standalone_partial_stage6_without_receipt_stays_a_parser_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            errors, _, workers, autonat = self.validate_fixture(self.fixture(directory, "stage6"), with_receipt=False)
            self.assertEqual(errors, [])
            self.assertEqual(workers, 30)
            self.assertEqual(autonat, 0)


if __name__ == "__main__":
    unittest.main()
