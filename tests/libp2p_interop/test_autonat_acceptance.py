"""Synthetic parser receipts, not live evidence or promotion results.

Only external Git/donor discovery is mocked. The canonical checker still reads
real temporary raw files, hashes, process commands and namespace transcripts,
and calls the unmodified paired protocol validators for every one of 41 cases.
"""

import copy
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import autonat_acceptance as acceptance
import autonat_cases as cases
from autonat_network import IsolatedAutonatNetwork
import check_stage6_acceptance as checker
import check_p2p_feature_inventory as inventory
import process_lifecycle
import promote_stage6_acceptance as promotion
from provenance import WorktreeIdentity
import runner
from test_autonat_cases import Harness
from test_autonat_network import FakeCommandRunner
from test_upgrade_evidence import attach_terminal_owners, echo_receipt, paired_receipt


SOURCE = Path(__file__).resolve().parent


class AutonatAcceptanceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "tests/libp2p_interop"
        self.source.mkdir(parents=True)
        (self.source / "runner.py").write_text("# synthetic parser fixture, never executed\n")
        self.key = self.source / "fixtures/pnet/swarm.key"
        self.key.parent.mkdir(parents=True)
        self.key.write_bytes((SOURCE / "fixtures/pnet/swarm.key").read_bytes())
        self.manifest_path = self.source / "p2p_donor_capabilities.json"
        self.manifest = checker.load_json(SOURCE / "p2p_donor_capabilities.json")
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.build = self.root / "build"
        self.artifact_root = self.build / "autonat-run"
        self.artifact_root.mkdir(parents=True)
        self.artifact_path = self.build / "autonat-artifacts.json"
        self.donors = self.root / "donors"
        self.donors.mkdir()
        self.binaries = {impl: self.build / impl for impl in ("forge", "go", "rust")}
        for path in self.binaries.values():
            path.write_text("synthetic fixture binary, never executed\n")
        self.identity = WorktreeIdentity("a" * 40, "b" * 64, False)
        self.records = []
        fingerprint = runner.canonical_pnet_fingerprint(self.key)
        for index, spec in enumerate(cases.case_specs()):
            harness = Harness(spec)
            fake = FakeCommandRunner()
            network = IsolatedAutonatNetwork(
                spec.version == 2,
                command_runner=lambda command, fake=fake: fake([
                    "ip" if arg == "/unit/ip" else arg for arg in command]),
                system=lambda: "Linux", ip_lookup=lambda name: f"/unit/{name}",
                outer_namespace_isolated=lambda: True, namespace_token=f"case{index}",
            )
            fake.network = network
            with patch.object(cases, "IsolatedAutonatNetwork", return_value=network), \
                 patch.object(process_lifecycle.subprocess, "Popen", side_effect=harness.popen):
                record = cases.run_case(
                    spec, self.binaries, self.artifact_root, pnet_key=self.key,
                    pnet_fingerprint=fingerprint, wait_json=harness.wait_ready,
                    command_attempt=runner.command_attempt,
                )
            self.assertEqual(record["status"], "passed", record)
            name = acceptance.acceptance_id(spec)
            record["acceptance_scenario_ids"] = [] if name is None else [name]
            self.records.append(record)
        now = time.time() - 10
        identity = self.identity.as_json()
        self.artifact = {
            "schema_version": 2,
            "runner_argv": [
                str(Path(sys.executable).resolve()), str(self.source / "runner.py"),
                "--enabled", "1", "--forge-fixture", str(self.binaries["forge"]),
                "--source-dir", str(self.source), "--build-dir", str(self.build),
                "--forge-root", str(self.root), "--donors-root", str(self.donors),
                "--acceptance-manifest", str(self.manifest_path), "--suite", "autonat",
            ],
            "started_at_unix": now, "finished_at_unix": now + 1,
            "acceptance_manifest": {"path": str(self.manifest_path), "sha256": checker.sha256_file(self.manifest_path)},
            "artifact_root": str(self.artifact_root), "artifacts": self.records, "failures": [],
            "evidence_index": [],
            "fixture_provenance": {
                "forge_worktree": {"start": identity, "end": identity, "changed_during_run": False},
                "fixture_build_info": {"schema_version": 2, "forge": identity,
                                       "compiler": {"path": "/unit/clang", "id": "Clang", "version": "22"},
                                       "build_profile": "self-test"},
                "tools": {"python": {"path": str(Path(sys.executable).resolve()),
                                     "version_output": "synthetic-python-version"}},
                "binaries": {impl: {"path": str(path), "sha256": checker.sha256_file(path)}
                             for impl, path in self.binaries.items()},
                "runner_inputs": {"suite": "autonat", "source_dir": str(self.source),
                                  "build_dir": str(self.build), "forge_root": str(self.root),
                                  "donors_root": str(self.donors), "acceptance_manifest": str(self.manifest_path)},
            },
        }
        self.save(refresh_index=True)

    def save(self, refresh_index=False):
        if refresh_index:
            self.artifact["evidence_index"] = checker.build_evidence_index(self.artifact_root, self.records)
        self.artifact_path.write_text(json.dumps(self.artifact))

    def validate(self, receipt=None, expected_suite="autonat"):
        self.save()
        with patch.object(checker, "validate_git_state", return_value=(1.0, [])), \
             patch.object(checker, "worktree_identity", return_value=self.identity), \
             patch.object(checker, "validate_donor_provenance", return_value=[]), \
             patch.object(checker.subprocess, "check_output", return_value="synthetic-python-version\n"):
            errors, limited = checker.validate(
                self.root, self.manifest_path, self.artifact_path, self.identity.head,
                receipt, expected_suite=expected_suite,
            )
        self.assertFalse(limited)
        return errors

    def rejected(self, text):
        errors = self.validate()
        self.assertTrue(any(text in error for error in errors), errors)

    def rewrite_network(self, record):
        value = record["network"]
        Path(value["result_file"]).write_text(json.dumps({k: v for k, v in value.items() if k != "result_file"}))
        self.save(refresh_index=True)

    def full_stage6_fixture(self):
        """Synthetic ready base contract plus all real-schema AutoNAT records.

        This models the future all-implemented gate, not today's production
        readiness. No semantic validator or raw-evidence check is mocked.
        """
        old_root = self.artifact_root
        self.artifact_root = self.build / "interop-run"
        old_root.rename(self.artifact_root)

        def relocated(value):
            if isinstance(value, str):
                return value.replace(str(old_root), str(self.artifact_root))
            if isinstance(value, list):
                return [relocated(item) for item in value]
            if isinstance(value, dict):
                return {key: relocated(item) for key, item in value.items()}
            return value

        self.artifact = relocated(self.artifact)
        self.records = self.artifact["artifacts"]
        self.artifact_path = self.build / "interop-artifacts.json"
        self.artifact["fixture_provenance"]["runner_inputs"]["suite"] = "stage6"
        self.artifact["runner_argv"] = self.artifact["runner_argv"][:-2]
        registry = self.manifest["interop_acceptance_registry"]
        registry["capabilities"] = {owner: entry for owner, entry in registry["capabilities"].items()
                                    if owner in {value[0] for value in acceptance.SCENARIOS.values()}}
        base = checker.fixture_manifest("tcp_yamux")["interop_acceptance_registry"]
        registry["capabilities"].update(base["capabilities"])
        registry["evidence_contracts"] = sorted(acceptance.EVIDENCE_CONTRACTS) + base["evidence_contracts"]
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.artifact["acceptance_manifest"]["sha256"] = checker.sha256_file(self.manifest_path)
        for dialer, listener in (("forge", "go"), ("go", "forge")):
            work = self.artifact_root / f"base-{dialer}-{listener}"
            work.mkdir()
            if dialer == "forge":
                payload, listener_payload = paired_receipt(echo=True)
                payload["payload_bytes"] = 19
            else:
                payload = echo_receipt()
                listener_payload = {"implementation": listener, "role": "listener", "status": "ok"}
            result_file, listener_file = work / "dial.json", work / "listen.json"
            result_file.write_text(json.dumps(payload))
            listener_file.write_text(json.dumps(listener_payload))
            for role in ("dial", "listen"):
                (work / f"{role}.log").write_text("synthetic complete execution\n")
            dial_command = [str(self.binaries[dialer]), "dial", "--scenario", "echo",
                            "--peer-id", "remote", "--addr", "/ip4/127.0.0.1/tcp/1",
                            "--result-file", str(result_file), "--store-dir", str(work / "dial-store"),
                            "--transport", "tcp"]
            listen_command = [str(self.binaries[listener]), "listen", "--scenario", "echo",
                              "--ready-file", str(work / "ready.json"), "--stop-file", str(work / "stop"),
                              "--store-dir", str(work / "listen-store"), "--features", "ping",
                              "--transport", "tcp", "--result-file", str(listener_file)]
            self.records.append({
                "dialer": dialer, "listener": listener, "scenario": "echo",
                "runner_scenario_id": "tcp_noise/echo", "acceptance_scenario_id": "tcp_yamux",
                "profile": "native", "transport_stack": ["tcp", "yamux"], "transport": "tcp",
                "peer_id": "remote", "addr": "/ip4/127.0.0.1/tcp/1",
                "effective_configuration": {"activation": "enabled", "profile": "native",
                                            "transport_stack": ["tcp", "yamux"],
                                            "dialer": {"transport": "tcp"}, "listener": {"transport": "tcp"}},
                "result": payload | {"result_file": str(result_file), "attempts": [{
                    "kind": "dial", "scenario_id": "echo", "command": dial_command,
                    "exit_code": 0, "log_file": str(work / "dial.log"),
                }]},
                "listener_process": {"command": listen_command, "log_file": str(work / "listen.log"),
                                     "terminal_status": {"exit_code": 0, "termination": "graceful"}},
                "listener_result_file": str(listener_file), "listener_result": listener_payload,
            })
            attach_terminal_owners(self.records[-1], payload, listener_payload)
        self.save(refresh_index=True)

    def receipt(self):
        return {"schema_version": 2, "runner_argv": self.artifact["runner_argv"],
                "started_at_unix": self.artifact["started_at_unix"] - 1,
                "finished_at_unix": self.artifact["finished_at_unix"] + 1,
                "returncode": 0, "invocation_directory": str(self.build),
                "artifact_path": str(self.artifact_path), "artifact_sha256": checker.sha256_file(self.artifact_path)}

    def test_full_stage6_requires_and_accepts_base_plus_all_41_pairs(self):
        self.full_stage6_fixture()
        self.assertEqual(self.validate(self.receipt(), expected_suite="stage6"), [])
        complete = list(self.records)
        for index in range(41):
            with self.subTest(missing=complete[index]["scenario_id"]):
                self.records[:] = complete[:index] + complete[index + 1:]
                self.save(refresh_index=True)
                errors = self.validate(self.receipt(), expected_suite="stage6")
                self.assertTrue(any("all 41 cases" in error for error in errors), errors)
        self.records[:] = [record for record in complete if record.get("suite") != "autonat"]
        self.save(refresh_index=True)
        self.assertTrue(any("all 41 cases" in error for error in self.validate(self.receipt(), expected_suite="stage6")))

    def test_generic_autonat_stub_cannot_replace_a_required_native_pair(self):
        self.full_stage6_fixture()
        self.assertEqual(self.validate(self.receipt(), expected_suite="stage6"), [])
        self.records.pop(0)
        # Synthetic legacy-style receipt: opening a generic stream is not a
        # native paired probe, even when its files and execution are indexed.
        legacy = copy.deepcopy(self.records[-1])
        legacy.update(scenario="autonatv2", runner_scenario_id="quic_base/autonatv2")
        legacy.pop("acceptance_scenario_id")
        self.records.append(legacy)
        self.save(refresh_index=True)
        errors = self.validate(self.receipt(), expected_suite="stage6")
        self.assertTrue(any("all 41 cases" in error for error in errors), errors)

    def test_full_suite_accepts_silent_owned_stdout_for_base_and_autonat(self):
        self.full_stage6_fixture()
        for record in self.records:
            for owner in record["owned_processes"]:
                Path(owner["log_file"]).write_bytes(b"")
        self.save(refresh_index=True)
        self.assertEqual(self.validate(self.receipt(), expected_suite="stage6"), [])

    def test_duplicate_ping_claim_is_rejected_not_selected_by_transport(self):
        self.full_stage6_fixture()
        registry = self.manifest["interop_acceptance_registry"]
        ping = checker.fixture_manifest("ping")["interop_acceptance_registry"]
        registry["capabilities"].update(ping["capabilities"])
        registry["evidence_contracts"] += ping["evidence_contracts"]
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.artifact["acceptance_manifest"]["sha256"] = checker.sha256_file(self.manifest_path)
        for profile, transport in (("quic_base", "quic"), ("tcp_tls", "tcp-tls")):
            record = copy.deepcopy(self.records[-1])
            record.update(dialer="forge", listener="go", scenario="ping", acceptance_scenario_id="ping",
                          runner_scenario_id=f"{profile}/ping", transport=transport,
                          transport_stack=["quic"] if transport == "quic" else ["tcp", "yamux"])
            self.records.append(record)
        self.save(refresh_index=True)
        errors = self.validate(self.receipt(), expected_suite="stage6")
        self.assertTrue(any("ping/forge_to_go lacks one canonical raw runner record" in error
                            for error in errors), errors)

    def test_full_stage6_does_not_drop_base_contracts_or_hide_failed_pair(self):
        self.full_stage6_fixture()
        missing = self.records.pop()
        self.save(refresh_index=True)
        errors = self.validate(self.receipt(), expected_suite="stage6")
        self.assertTrue(any("lacks one canonical raw runner record" in error for error in errors), errors)
        self.records.append(missing)
        self.records[0]["status"] = "failed"
        self.save(refresh_index=True)
        errors = self.validate(self.receipt(), expected_suite="stage6")
        self.assertTrue(any("terminal verdict mismatch" in error for error in errors), errors)

    def test_promotion_cannot_remove_autonat_from_manifest_and_receipt(self):
        self.full_stage6_fixture()
        self.manifest = checker.fixture_manifest("tcp_yamux")
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.artifact["acceptance_manifest"]["sha256"] = checker.sha256_file(self.manifest_path)
        self.records[:] = [record for record in self.records if record.get("suite") != "autonat"]
        self.save(refresh_index=True)
        errors = self.validate(self.receipt(), expected_suite="stage6")
        self.assertTrue(any("all 41 cases" in error for error in errors), errors)

    def test_formal_control_source_cannot_move_to_secondary_even_with_matching_traces(self):
        record = next(r for r in self.records if r["client_implementation"] == "forge"
                      and r["server_implementation"] == "go" and r["version"] == 2 and r["transport"] == "tcp")
        for connection in record["client"]["actual_connections"]:
            if connection["direction"] == "outbound":
                connection["local_addr"] = connection["local_addr"].replace("/11.0.0.1/", "/11.0.0.3/")
        for connection in record["server"]["actual_connections"]:
            if connection["direction"] == "inbound":
                connection["remote_addr"] = connection["remote_addr"].replace("/11.0.0.1/", "/11.0.0.3/")
        record["server"]["service_completed_requests"][0]["dial_data_required"] = False
        # Same-IP requests remain valid outside the formal cross-IP matrix.
        self.assertEqual(acceptance.validate_autonat_evidence(
            record["client"], record["server"], version=2, transport="tcp",
            expected_service_peer=record["expected_service_peer"],
            expected_requested_address=record["expected_requested_address"],
            client_terminal_status=record["owned_processes"][1]["terminal_status"],
            server_terminal_status=record["owned_processes"][0]["terminal_status"],
        ), [])
        for role, owner in zip(("server", "client"), record["owned_processes"]):
            Path(owner["outputs"][-1]["log_file"]).write_text(json.dumps(record[role]))
        self.save(refresh_index=True)
        self.rejected("control source differs from primary namespace IP")

    def test_formal_rust_hidden_control_socket_requires_corroborated_primary_source(self):
        record = next(r for r in self.records if r["client_implementation"] == "rust"
                      and r["version"] == 2 and r["transport"] == "tcp")
        for connection in record["client"]["actual_connections"]:
            if connection["direction"] == "outbound":
                connection["local_addr"] = None
        Path(record["owned_processes"][1]["outputs"][-1]["log_file"]).write_text(json.dumps(record["client"]))
        self.save(refresh_index=True)
        self.assertEqual(self.validate(), [])
        record["client"]["observer_reported_control_addr"] = record["client"]["observer_reported_control_addr"].replace(
            "/11.0.0.1/", "/11.0.0.3/")
        Path(record["owned_processes"][1]["outputs"][-1]["log_file"]).write_text(json.dumps(record["client"]))
        self.save(refresh_index=True)
        self.rejected("control source lacks paired primary namespace IP evidence")

    def test_formal_quic_wildcard_bind_keeps_independent_primary_source(self):
        for record in self.records:
            if record["client_implementation"] not in ("forge", "go") or record["transport"] != "quic":
                continue
            for connection in record["client"]["actual_connections"]:
                if connection["direction"] == "outbound":
                    connection["local_addr"] = connection["local_addr"].replace("/11.0.0.1/", "/0.0.0.0/")
            if record["client_implementation"] == "go":
                record["client"]["control_local_addr"] = record["client"]["control_local_addr"].replace(
                    "/11.0.0.1/", "/0.0.0.0/")
            Path(record["owned_processes"][1]["outputs"][-1]["log_file"]).write_text(json.dumps(record["client"]))
        self.save(refresh_index=True)
        self.assertEqual(self.validate(), [])

    def test_complete_41_pair_receipt(self):
        self.assertEqual(self.validate(), [])
        claims = [name for record in self.records for name in record["acceptance_scenario_ids"]]
        self.assertEqual(len(claims), 24)
        self.assertEqual(set(claims), set(acceptance.SCENARIOS))

    def test_each_missing_case_including_deny_negative_and_tls_is_rejected(self):
        original = list(self.records)
        for index in range(41):
            with self.subTest(case=original[index]["scenario_id"]):
                self.records[:] = original[:index] + original[index + 1:]
                self.save(refresh_index=True)
                self.rejected("all 41 cases")
        self.records[:] = original

    def test_duplicate_case_does_not_replace_missing_case(self):
        self.records[-1] = copy.deepcopy(self.records[0])
        self.save(refresh_index=True)
        self.rejected("all 41 cases")

    def test_client_receipt_cannot_claim_service_or_both_roles(self):
        self.records[0]["acceptance_scenario_ids"] = ["autonat_v1_service_native_tcp_yamux"]
        self.rejected("different Forge role")
        self.records[0]["acceptance_scenario_ids"].append("autonat_v1_client_native_tcp_yamux")
        self.rejected("duplicate capability")

    def test_policy_denial_is_not_native_proof(self):
        record = next(r for r in self.records if r["expected_outcome"] == "policy_denied")
        record["acceptance_scenario_ids"] = ["autonat_v1_client_private_tcp_yamux_pnet"]
        self.rejected("different Forge role")

    def test_wrong_direction_and_wrong_expected_address(self):
        record = self.records[0]
        record["client_implementation"] = "go"
        self.rejected("pair identity")
        record["client_implementation"] = "forge"
        record["expected_requested_address"] = "/ip4/11.0.0.1/tcp/9"
        self.rejected("expected identity/address")

    def test_control_only_is_not_dialback_even_with_refreshed_hash(self):
        record = self.records[0]
        record["server"]["probe_connections"] = []
        output = record["owned_processes"][0]["outputs"][-1]
        Path(output["log_file"]).write_text(json.dumps(record["server"]))
        self.save(refresh_index=True)
        self.assertTrue(self.validate())

    def test_tampered_raw_log_and_hash(self):
        log = Path(self.records[0]["owned_processes"][0]["log_file"])
        log.write_text("tampered log\n")
        self.rejected("hash or size differs")

    def test_empty_stdout_log_is_hashed_but_empty_result_is_not_proof(self):
        owner = self.records[0]["owned_processes"][0]
        Path(owner["log_file"]).write_bytes(b"")
        self.save(refresh_index=True)
        self.assertEqual(self.validate(), [])
        Path(owner["outputs"][-1]["log_file"]).write_bytes(b"")
        self.save(refresh_index=True)
        self.rejected("empty non-process proof")

    def test_rust_v2_zero_payment_cannot_cover_secondary_listener(self):
        record = next(r for r in self.records if r["client_implementation"] == "rust"
                      and r["version"] == 2 and r["expected_outcome"] == "reachable")
        record["client"]["dial_data_bytes"] = 0
        record["client"]["donor_events"][-1]["bytes_sent"] = 0
        Path(record["owned_processes"][1]["outputs"][0]["log_file"]).write_text(json.dumps(record["client"]))
        self.save(refresh_index=True)
        self.assertTrue(self.validate())

    def test_missing_and_escaped_evidence_index(self):
        self.artifact["evidence_index"].pop()
        self.rejected("does not exactly cover")
        self.save(refresh_index=True)
        self.artifact["evidence_index"][0]["path"] = "../escape.json"
        self.rejected("path, size or SHA-256")

    def test_stale_payload_and_wrong_capture_path(self):
        self.records[0]["client"]["status"] = "forged"
        self.rejected("raw fixture result differs")
        output = self.records[1]["owned_processes"][1]["outputs"][0]
        output["path"] = str(self.artifact_root / "unrelated.json")
        self.rejected("output does not bind")

    def test_host_launcher_or_other_namespace_rejected(self):
        owner = self.records[0]["owned_processes"][1]
        original = list(owner["command"])
        owner["command"][:] = original[4:]
        self.rejected("owned namespace")
        owner["command"][:] = original
        owner["command"][3] = "autonat-c-other"
        self.rejected("owned namespace")

    def test_namespace_routes_and_unjoined_processes_rejected(self):
        record = self.records[0]
        command = next(c for c in record["network"]["commands"] if c["command"][-4:] == ["-j", "route", "show", "default"])
        command["stdout"] = '[{"dst":"default"}]'
        self.rewrite_network(record)
        self.rejected("default route")
        command["stdout"] = "[]"
        command = next(c for c in record["network"]["commands"] if c["command"][1:3] == ["netns", "pids"])
        command["stdout"] = "123\n"
        self.rewrite_network(record)
        self.rejected("namespace evidence")

    def test_negative_requires_both_closed_port_observations(self):
        record = self.records[-1]
        commands = record["network"]["commands"]
        index = next(i for i, c in enumerate(commands) if c["command"][-5:] == ["-H", "-lnt", "sport", "=", ":9"])
        commands.pop(index)
        self.rewrite_network(record)
        self.rejected("namespace command order")

    def test_timeout_or_forced_exit_zero_cannot_be_negative(self):
        attempt = self.records[-1]["attempts"][1]
        attempt["timeout_class"] = "fixture_timeout"
        self.rejected("failed attempt")
        del attempt["timeout_class"]
        self.records[-1]["owned_processes"][1]["terminal_status"]["termination"] = "terminated"
        self.rejected("graceful joined terminal")

    def test_binary_hash_and_clean_identity_remain_mandatory(self):
        self.binaries["forge"].write_text("changed binary")
        self.rejected("binary path or SHA-256")
        self.artifact["fixture_provenance"]["forge_worktree"]["start"]["dirty"] = True
        self.rejected("clean expected worktree")

    def test_suite_root_and_argv_cannot_be_relabeled(self):
        self.artifact["runner_argv"][-1] = "stage6"
        self.rejected("argv suite differs")
        self.artifact["runner_argv"][-1] = "autonat"
        self.artifact["fixture_provenance"]["runner_inputs"]["suite"] = "stage6"
        self.rejected("suite differs")

    def test_execution_receipt_is_bound_and_not_reusable(self):
        receipt = {"schema_version": 2, "runner_argv": self.artifact["runner_argv"],
                   "started_at_unix": self.artifact["started_at_unix"] - 1,
                   "finished_at_unix": self.artifact["finished_at_unix"] + 1,
                   "returncode": 0, "invocation_directory": str(self.build),
                   "artifact_path": str(self.artifact_path), "artifact_sha256": checker.sha256_file(self.artifact_path)}
        self.assertEqual(self.validate(receipt), [])
        receipt["artifact_sha256"] = "0" * 64
        self.assertTrue(any("receipt does not bind" in e for e in self.validate(receipt)))
        self.assertTrue(any("suite differs" in e for e in self.validate(expected_suite="stage6")))

    def test_registered_contracts_remain_staged_and_have_correct_roles(self):
        required, errors = checker.required_scenarios(self.manifest, "autonat")
        self.assertEqual(errors, [])
        self.assertEqual(len(required), 12)
        capabilities = {c["id"]: c for c in self.manifest["capabilities"]}
        for (owner, name), value in required.items():
            self.assertEqual(capabilities[owner]["decision"], "stage_6")
            self.assertEqual(value[0], acceptance.ROLE_DIRECTIONS[acceptance.SCENARIOS[name][1]])
        self.assertTrue(inventory.registered_runner_acceptance_pairs(SOURCE / "runner.py"))
        entry = self.manifest["interop_acceptance_registry"]["capabilities"]["protocol.autonat_v1_client"]["scenarios"][0]
        entry["required_directions"].append("go_to_forge")
        self.assertTrue(any("role directions" in e for e in checker.required_scenarios(self.manifest, "autonat")[1]))

    def test_matrix_bindings_cannot_make_controls_optional(self):
        specs = [s for s in cases.case_specs() if s.outcome == "reachable"]
        with patch.object(acceptance, "case_specs", return_value=specs):
            self.rejected("exactly 41 unique cases")

    def test_run_suite_assigns_claims_once_without_duplicate_launches(self):
        specs = cases.case_specs()
        with patch.object(cases, "run_case", side_effect=lambda *args, **kwargs: {}) as run:
            records = list(cases.run_suite(
                self.binaries, self.artifact_root, pnet_key=self.key, pnet_fingerprint="test",
                wait_json=None, command_attempt=None, claims_for_case=runner.autonat_claims))
        self.assertEqual(run.call_count, 41)
        self.assertEqual(len(records), len(specs))
        for spec, call in zip(specs, run.call_args_list):
            self.assertEqual(call.args[0], spec)
        self.assertEqual([r["acceptance_scenario_ids"] for r in records],
                         [runner.autonat_claims(spec) for spec in specs])


class OwnedStdoutEvidenceTests(unittest.TestCase):
    """Small synthetic ownership records, not edited live acceptance artifacts."""

    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.log = self.root / "quiet.log"
        self.log.write_bytes(b"")
        self.result = self.root / "result.json"
        self.result.write_text('{"status":"ok"}')
        self.binaries = {"forge": self.root / "forge"}
        self.owner = {
            "pid": 123, "command": [str(self.binaries["forge"]), "dial", "--scenario", "ping",
                                    "--result-file", str(self.result)],
            "log_file": str(self.log), "terminal_status": {"exit_code": 0, "termination": "graceful"},
        }
        self.records = [{"owned_processes": [self.owner], "result": {"result_file": str(self.result)}}]

    def validate_index(self, index=None):
        if index is None:
            index = checker.build_evidence_index(self.root, self.records)
        return checker.validate_evidence_index(self.root / "artifact.json", self.root, self.records,
                                               index, self.binaries)[1]

    def test_silent_stdout_requires_owner_not_filename_extension(self):
        self.assertEqual(self.validate_index(), [])
        self.log.rename(self.root / "stdout")
        self.log = self.root / "stdout"
        self.owner["log_file"] = str(self.log)
        self.assertEqual(self.validate_index(), [])
        self.records[0]["unowned"] = {"log_file": str(self.root / "unowned.log")}
        (self.root / "unowned.log").write_bytes(b"")
        self.assertIn("artifact evidence index has an empty non-process proof", self.validate_index())

    def test_nested_process_attempt_and_listener_ownership(self):
        for wrapper in (lambda owner: {"attempts": [owner]},
                        lambda owner: {"control": {"listener_process": owner}},
                        lambda owner: {"processes": {"seeker": owner}}):
            with self.subTest(wrapper=wrapper):
                self.records = [wrapper(self.owner)]
                self.assertEqual(self.validate_index(), [])

    def test_running_failed_forged_or_contradictory_owner_is_not_silent_proof(self):
        original = copy.deepcopy(self.owner)
        changes = ({"pid": True}, {"pid": None}, {"command": ["/unbound/binary", "dial"]},
                   {"terminal_status": {"exit_code": 0, "termination": "running"}},
                   {"terminal_status": {"exit_code": 0, "termination": "terminated"}},
                   {"terminal_status": {"exit_code": 1, "termination": "graceful"}},
                   {"exit_code": 1}, {"timeout_class": "dial_timeout"},
                   {"requested_log_file": str(self.result)})
        for change in changes:
            with self.subTest(change=change):
                self.owner.clear()
                self.owner.update(copy.deepcopy(original) | change)
                self.assertIn("artifact evidence index has an empty non-process proof", self.validate_index())
        self.owner.clear()
        self.owner.update(original)
        self.records[0]["result"]["attempts"] = [copy.deepcopy(self.owner) | {"pid": 456}]
        self.assertIn("artifact evidence index has an empty non-process proof", self.validate_index())

    def test_empty_json_cannot_borrow_stdout_ownership(self):
        for location in ("result_file", "snapshot", "dns"):
            with self.subTest(location=location):
                self.records = [{"owned_processes": [self.owner]}]
                if location == "result_file":
                    self.records[0]["result_file"] = str(self.log)
                elif location == "snapshot":
                    self.owner["outputs"] = [{"argument": "--result-file", "exists": True,
                                              "path": str(self.result), "log_file": str(self.log)}]
                else:
                    self.records[0]["dns_evidence"] = {"log_file": str(self.log)}
                self.assertIn("artifact evidence index has an empty non-process proof", self.validate_index())
                self.owner.pop("outputs", None)

    def test_empty_requested_json_stop_or_store_file_is_never_stdout(self):
        self.result.write_bytes(b"")
        self.assertIn("artifact evidence index has an empty non-process proof", self.validate_index())
        self.result.write_text('{"status":"ok"}')
        original = list(self.owner["command"])
        for flag, path in (("--ready-file", self.log), ("--stop-file", self.log), ("--store-dir", self.root)):
            with self.subTest(flag=flag):
                self.owner["command"] = original + [flag, str(path)]
                self.assertIn("artifact evidence index has an empty non-process proof", self.validate_index())

    def test_empty_stdout_still_requires_exact_hash_size_and_root(self):
        index = checker.build_evidence_index(self.root, self.records)
        next(entry for entry in index if entry["path"] == "quiet.log")["sha256"] = "0" * 64
        self.assertTrue(any("hash or size differs" in error for error in self.validate_index(index)))
        index = checker.build_evidence_index(self.root, self.records)
        next(entry for entry in index if entry["path"] == "quiet.log")["size"] = 1
        self.assertTrue(any("hash or size differs" in error for error in self.validate_index(index)))
        self.owner["log_file"] = str(self.root / ".." / "escaped.log")
        self.assertTrue(any("escapes artifact_root" in error for error in self.validate_index(index)))


class PromotionSuiteTests(unittest.TestCase):
    def test_wrapper_selects_exact_suite_and_owns_receipt(self):
        for suite in ("stage6", "autonat"):
            with self.subTest(suite=suite), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary).resolve()
                manifest = root / promotion.CANONICAL_ACCEPTANCE_MANIFEST
                argv = ["promote_stage6_acceptance.py", "--runner", str(root / "runner.py"),
                        "--forge-fixture", str(root / "forge"), "--source-dir", str(root / "source"),
                        "--build-dir", str(root / "build"), "--forge-root", str(root),
                        "--donors-root", str(root / "donors"), "--acceptance-manifest", str(manifest),
                        "--expected-head", "a" * 40, "--suite", suite]
                with patch.object(promotion.sys, "argv", argv), \
                     patch.object(promotion.subprocess, "run") as run, \
                     patch.object(promotion, "validate", return_value=(["synthetic rejection"], False)) as validate:
                    run.return_value.returncode = 0
                    self.assertEqual(promotion.main(), 1)
                command = run.call_args.args[0]
                self.assertEqual(command[-2:] if suite == "autonat" else command[-2],
                                 ["--suite", "autonat"] if suite == "autonat" else "--acceptance-manifest")
                self.assertEqual(validate.call_args.kwargs["expected_suite"], suite)
                receipt = validate.call_args.args[4]
                self.assertEqual(receipt["runner_argv"], command)
                self.assertEqual(receipt["returncode"], 0)
                self.assertEqual(Path(receipt["artifact_path"]).name,
                                 "autonat-artifacts.json" if suite == "autonat" else "interop-artifacts.json")
                self.assertTrue((Path(receipt["invocation_directory"]) / f"{suite}-promotion-receipt.json").is_file())


if __name__ == "__main__":
    unittest.main()
