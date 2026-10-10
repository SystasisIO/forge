"""Synthetic process/index tests, not native wire or live acceptance proof."""

import ast
from copy import deepcopy
from dataclasses import asdict
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import pubsub_acceptance as shared
import pubsub_extension_acceptance as acceptance
import pubsub_quic_proof as proof
from pubsub_extension_cases import Case, case_specs
from pubsub_evidence import DIRECTIONS, PROFILES
from stage6_evidence_contract import evidence_contract_for


class Index:
    """Process-only model. Native semantics and ACK bodies are tested separately."""

    def __init__(self, root=Path("/unit")):
        self.payloads, self.loads = {}, []
        self.root = root
        self.binaries = {name: f"/unit/{name}" for name in ("forge", "go", "rust", "rust-quic-observer")}

    def record(self, spec, *, base_pid=100, fingerprint="b" * 64, token="a" * 32):
        work = self.root / spec.identifier
        value = {"schema_version": 1, "suite": "pubsub-extensions", "case": asdict(spec),
                 "case_token": token, "scenario_id": spec.identifier, "acceptance_scenario_id": spec.scenario,
                 "runner_scenario_id": spec.runner_id, "status": "observed", "errors": [], "cleanup_errors": [],
                 "raw": {}, "processes": {}, "attempts": [], "evidence": {"unit_only": True},
                 "shutdown_barrier": {"operations": []}}
        for offset, role in enumerate(acceptance.ROLES):
            implementation, mode = spec.implementation(role), spec.mode(role)
            pid = base_pid + offset
            identity = {"schema_version": 1, "implementation": implementation, "actor": role,
                        "case_token": token, "local_peer_id": "unit-" + role,
                        "extension": mode, "requests_partial": mode == "partial"}
            raw = {**identity, "version": spec.version, "finalized": True, "joined": True,
                   "error": None, "overflow": False, "events": []}
            ready = {**identity, "peer_id": identity["local_peer_id"], "ready": True,
                     "subscription_created": True, "topic": "forge-pr11:" + token,
                     "listen_addrs": ["/ip4/127.0.0.1/tcp/4000"]}
            options = {"--version": spec.version, "--extension": mode, "--transport": PROFILES[spec.profile],
                       "--actor": role, "--case-token": token, "--store-dir": str(work / f"{role}.store")}
            options.update({f"--{name}-file": str(work / f"{role}.{name}")
                            for name in ("ready", "result", "stop", "control")})
            if spec.profile == "private_tcp_yamux":
                options.update({"--pnet-key-file": "/unit/pnet.key", "--pnet-fingerprint": fingerprint})
            owner = {"pid": pid, "command": [self.binaries[implementation], "pubsub-live",
                     *[part for pair in options.items() for part in pair]], "log_file": str(work / f"{role}.log"),
                     "terminal_status": {"exit_code": 0, "termination": "graceful"},
                     "ready": ready, "outputs": [], "returncode": 0, "forced_termination": False,
                     "stop_budget": {"native_close_seconds": 8, "post_stop_seconds": 0,
                                     "scheduler_allowance_seconds": 2, "seconds": 10}}
            for name, payload in (("ready", ready), ("result", raw)):
                path = owner["log_file"] + f".{name}-file.json"
                owner["outputs"].append({"argument": f"--{name}-file", "path": options[f"--{name}-file"],
                                         "exists": True, "log_file": path})
                self.payloads[path] = deepcopy(payload)
            value["raw"][role], value["processes"][role] = raw, owner
            value["attempts"].append({"kind": role, "scenario_id": spec.identifier, "attempt_id": 1,
                "command": deepcopy(owner["command"]), "requested_log_file": owner["log_file"],
                "timeout_seconds": 60, "exit_code": 0, "log_file": owner["log_file"], "pid": pid,
                "terminal_status": deepcopy(owner["terminal_status"]), "outputs": deepcopy(owner["outputs"])})
            path = str(work / f"{role}.prepare-result.json")
            value["shutdown_barrier"]["operations"].append({"actor": role, "evidence_file": path})
            self.payloads[path] = {"unit_only_prepare": role}
        return value

    def load(self, path):
        self.loads.append(path)
        if path not in self.payloads:
            raise OSError("missing indexed output")
        return deepcopy(self.payloads[path])

    def split(self, spec, *, base_pid=100):
        original = self.record(spec, base_pid=base_pid)
        original.pop("evidence")
        original["status"] = "captured"
        original["shutdown_barrier"]["source"] = "python.fixture.all_actor_prepare_barrier"
        for sequence, row in enumerate(original["shutdown_barrier"]["operations"], 1):
            role = row["actor"]
            row.update(sequence=sequence, kind="prepare_ack")
            captured = deepcopy(original["raw"][role])
            captured.update(finalized=False, joined=False)
            self.payloads[row["evidence_file"]] = {"result": captured}
        companion = Index(self.root / "quic-observer")
        companion.binaries["rust"] = self.binaries["rust-quic-observer"]
        shutdown = companion.record(spec, base_pid=base_pid + 4, token="c" * 32)
        self.payloads.update(companion.payloads)
        return {"schema_version": 1, "suite": "pubsub-extensions", "scenario_id": spec.identifier,
                "acceptance_scenario_id": spec.scenario, "runner_scenario_id": spec.runner_id,
                "case": asdict(spec), "proof_scope": proof.SCOPE, "original": original, "shutdown": shutdown,
                "status": "observed", "errors": [], "cleanup_errors": [],
                "evidence": {"original_wire": {"unit_only_active": True},
                             "instrumented_shutdown": {"unit_only": True}, "original_shutdown": "NOT_PROVEN"}}


def requirements():
    return {(acceptance.OWNERS[spec.extension], spec.scenario): (
        {a + "_to_" + b for a, b in DIRECTIONS}, "passed",
        "private_network" if spec.profile == "private_tcp_yamux" else "native",
        {"native_quic": ("quic",), "native_tcp_yamux": ("tcp", "yamux"),
         "private_tcp_yamux": ("tcp", "pnet", "yamux")}[spec.profile], spec.runner_id,
        ("security.private_network_psk",) if spec.profile == "private_tcp_yamux" else (),
        evidence_contract_for(spec.scenario)) for spec in case_specs()}


class ExtensionAcceptanceTests(unittest.TestCase):
    spec = Case("rust", "forge", "partial", "native_quic")

    def setUp(self):
        # Only isolate wire/ACK semantics; real shared process/index binding runs.
        semantics = patch.object(acceptance, "validate_capture", return_value={"unit_only": True})
        ack = patch.object(shared, "prepared_snapshot", return_value={})
        self.semantics, self.ack = semantics.start(), ack.start()
        self.addCleanup(semantics.stop)
        self.addCleanup(ack.stop)
        active = patch.object(acceptance, "validate_active_capture", return_value={"unit_only_active": True})
        self.active = active.start()
        self.addCleanup(active.stop)

    def check(self, record, index, spec=None, fingerprint="b" * 64):
        return acceptance.validate_record(record, spec or self.spec, Path("/unit"), index.binaries,
                                           index.load, Path("/unit/pnet.key"), fingerprint)

    def test_shared_authority_reads_all_eight_outputs_and_four_prepare_snapshots(self):
        index = Index()
        record = index.record(self.spec)
        before = deepcopy(record)
        with patch.object(shared, "_owned_output", wraps=shared._owned_output) as verifier:
            self.assertEqual(len(self.check(record, index)), 16)
            self.assertEqual(verifier.call_count, 4)
        self.assertEqual(len(index.loads), 12)
        self.assertEqual(set(index.loads), set(index.payloads))
        self.assertEqual(self.ack.call_count, 4)
        self.assertEqual(record, before)
        self.assertIsNone(self.semantics.call_args.kwargs["expected_fingerprint"])

    def test_private_scope_uses_exact_key_fingerprint_and_native_validation(self):
        spec = Case("forge", "go", "partial", "private_tcp_yamux")
        index = Index()
        record = index.record(spec)
        self.check(record, index, spec)
        self.assertEqual(self.semantics.call_args.kwargs["expected_fingerprint"], "b" * 64)
        with self.assertRaises(ValueError):
            self.check(record, index, spec, fingerprint="c" * 64)
        self.semantics.side_effect = ValueError("native fingerprint mismatch")
        with self.assertRaisesRegex(ValueError, "native fingerprint"):
            self.check(record, index, spec)

    def test_binary_flags_actor_attempt_and_terminal_ownership_cannot_be_relabeled(self):
        def flag(record, name, replacement):
            command = record["processes"]["replacement"]["command"]
            command[command.index(name) + 1] = replacement
            record["attempts"][2]["command"] = deepcopy(command)

        mutations = (
            lambda r: r["processes"]["replacement"]["command"].__setitem__(0, "/unit/rust-quic-observer"),
            lambda r: flag(r, "--version", "1.1"),
            lambda r: flag(r, "--extension", "advertisement"),
            lambda r: flag(r, "--actor", "victim"),
            lambda r: flag(r, "--control-file", "/foreign/control"),
            lambda r: r["processes"]["sink"].update(returncode=1),
            lambda r: r["processes"]["sink"].update(returncode=False),
            lambda r: r["processes"]["sink"].update(forced_termination=True),
            lambda r: r["processes"]["sink"]["stop_budget"].update(seconds=11),
            lambda r: r["attempts"][0].update(pid=999),
            lambda r: r["attempts"][0].update(exit_code=1),
            lambda r: r["attempts"][0].update(kind="sink"),
            lambda r: r["attempts"][0].update(timeout_seconds=600),
            lambda r: r["attempts"].append(deepcopy(r["attempts"][0])),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                index = Index()
                record = index.record(self.spec)
                mutation(record)
                with self.assertRaises(ValueError):
                    self.check(record, index)

    def test_indexed_payloads_missing_foreign_changed_or_failed_cannot_be_borrowed(self):
        for role in acceptance.ROLES:
            for name in ("ready", "result"):
                for key, replacement in (("implementation", "foreign"), ("actor", "foreign"),
                                         ("case_token", "c" * 32), ("extension", "idontwant"),
                                         ("requests_partial", None)):
                    with self.subTest(role=role, name=name, key=key):
                        index = Index()
                        record = index.record(self.spec)
                        output = record["processes"][role]["outputs"][0 if name == "ready" else 1]["log_file"]
                        index.payloads[output][key] = replacement
                        with self.assertRaises(ValueError):
                            self.check(record, index)
            for field, replacement in (("error", "actual native failure"), ("joined", False),
                                       ("finalized", False), ("overflow", True), ("extension", "foreign")):
                index = Index()
                record = index.record(self.spec)
                raw = record["raw"][role]
                raw[field] = replacement
                index.payloads[record["processes"][role]["outputs"][1]["log_file"]] = deepcopy(raw)
                with self.subTest(role=role, field=field), self.assertRaises(ValueError):
                    self.check(record, index)
        index = Index()
        record = index.record(self.spec)
        del index.payloads[record["processes"]["victim"]["outputs"][1]["log_file"]]
        with self.assertRaises(OSError):
            self.check(record, index)

    def test_prepare_path_loader_and_ack_validation_are_mandatory(self):
        for mode in ("missing", "foreign", "reused", "invalid_ack"):
            with self.subTest(mode=mode):
                index = Index()
                record = index.record(self.spec)
                rows = record["shutdown_barrier"]["operations"]
                if mode == "missing":
                    del index.payloads[rows[0]["evidence_file"]]
                elif mode == "foreign":
                    rows[0]["evidence_file"] = "/foreign/prepare.json"
                elif mode == "reused":
                    rows[1]["evidence_file"] = rows[0]["evidence_file"]
                else:
                    self.ack.side_effect = ValueError("foreign actual ACK")
                with self.assertRaises((ValueError, OSError)):
                    self.check(record, index)
                self.ack.side_effect = None

    def test_split_original_error_or_pr11_record_is_not_extension_proof(self):
        mutations = (
            lambda r: r.update(proof_scope="original_wire_and_instrumented_shutdown"),
            lambda r: r.update(observer={"status": "observed"}),
            lambda r: r.update(original_wire={"status": "observed"}),
            lambda r: r.update(suite="pubsub-scoring"),
            lambda r: r.update(status="captured"),
            lambda r: r.update(schema_version=True),
            lambda r: r.update(evidence={"status": "ok"}),
            lambda r: r.update(errors=["opaque original Rust QUIC cause"]),
        )
        for mutation in mutations:
            with self.subTest(mutation=mutation):
                index = Index()
                record = index.record(self.spec)
                mutation(record)
                with self.assertRaises(ValueError):
                    self.check(record, index)

    def test_completion_keeps_failed_raw_and_never_synthesizes_split(self):
        index = Index()
        record = index.record(self.spec)
        record.pop("evidence")
        record["status"] = "captured"
        before = deepcopy(record)
        self.semantics.side_effect = ValueError("original terminal failure")
        failed = acceptance.complete_case(record, self.spec)
        self.assertEqual(failed["status"], "HARNESS_ERROR")
        self.assertIn("original terminal failure", failed["errors"][0])
        self.assertEqual(failed["raw"], before["raw"])
        self.assertEqual(record, before)
        self.assertNotIn("proof_scope", failed)
        self.semantics.reset_mock()
        self.assertEqual(acceptance.complete_case(failed, self.spec), failed)
        self.semantics.assert_not_called()

    def test_closed_36_matrix_does_not_accept_partial_null_duplicate_or_unknown_records(self):
        index = Index()
        records = [(index.split if acceptance.needs_observer(spec) else index.record)(spec, base_pid=100 + 8 * i)
                   for i, spec in enumerate(case_specs())]
        def check(values, required=None):
            return acceptance.validate_suite(values, requirements() if required is None else required,
                Path("/unit"), index.binaries, index.load, pnet_key_file=Path("/unit/pnet.key"), pnet_fingerprint="b" * 64)
        self.assertEqual(check(records), [])
        for values in (records[:-1], records + [None], [None, *records[1:]],
                       [records[1], *records[1:]], [{**records[0], "scenario_id": "unknown"}, *records[1:]],
                       [{**records[0], "status": "HARNESS_ERROR"}, *records[1:]]):
            with self.subTest(records=len(values)):
                self.assertTrue(check(values))
        missing = requirements()
        missing.pop(next(iter(missing)))
        self.assertTrue(check(records, missing))
        changed = requirements()
        key = next(iter(changed))
        changed[key] = ({"forge_to_go"}, *changed[key][1:])
        self.assertTrue(check(records, changed))
        duplicate_owner = deepcopy(records)
        for role in acceptance.ROLES:
            pid = duplicate_owner[0]["processes"][role]["pid"]
            duplicate_owner[1]["processes"][role]["pid"] = pid
            next(row for row in duplicate_owner[1]["attempts"] if row["kind"] == role)["pid"] = pid
        self.assertTrue(any("reuse" in error for error in check(duplicate_owner)))

    def test_split_checks_both_original_and_companion_indexes_without_cross_run_equality(self):
        index = Index()
        record = index.split(self.spec)
        before = deepcopy(record)
        used, pids = acceptance.validate_split(record, self.spec, index.root, index.binaries, index.load)
        self.assertEqual(len(used), 32)
        self.assertEqual(len(pids), 8)
        self.assertEqual(len(index.loads), 24)
        self.assertEqual(record, before)
        self.assertNotEqual(record["evidence"]["original_wire"], record["evidence"]["instrumented_shutdown"])

    def test_split_rejects_original_or_companion_source_substitution(self):
        mutations = (
            lambda r, i: r.update(schema_version=True),
            lambda r, i: r["shutdown"].update(case_token=r["original"]["case_token"]),
            lambda r, i: r["evidence"].update(original_shutdown="PROVEN"),
            lambda r, i: r["evidence"].update(original_wire=r["evidence"]["instrumented_shutdown"]),
            lambda r, i: r["original"].update(suite="pubsub-scoring"),
            lambda r, i: r["shutdown"].update(status="HARNESS_ERROR"),
            lambda r, i: r["original"]["processes"]["sink"].update(forced_termination=True),
            lambda r, i: r["shutdown"]["processes"]["sink"].update(returncode=1),
            lambda r, i: r["original"]["processes"]["replacement"]["command"].__setitem__(0, "/unit/rust-quic-observer"),
            lambda r, i: i.binaries.update({"rust-quic-observer": i.binaries["rust"]}),
            lambda r, i: i.payloads.pop(r["original"]["shutdown_barrier"]["operations"][0]["evidence_file"]),
            lambda r, i: i.payloads.pop(r["shutdown"]["processes"]["victim"]["outputs"][1]["log_file"]),
        )
        for mutation in mutations:
            index = Index()
            record = index.split(self.spec)
            mutation(record, index)
            with self.subTest(mutation=mutation), self.assertRaises((ValueError, OSError)):
                acceptance.validate_split(record, self.spec, index.root, index.binaries, index.load)

    def test_individually_bound_runs_cannot_reuse_a_physical_process(self):
        index = Index()
        record = index.split(self.spec)
        original_pid = record["original"]["processes"]["sink"]["pid"]
        record["shutdown"]["processes"]["sink"]["pid"] = original_pid
        next(attempt for attempt in record["shutdown"]["attempts"] if attempt["kind"] == "sink")["pid"] = original_pid
        with self.assertRaisesRegex(ValueError, "reused physical owners"):
            acceptance.validate_split(record, self.spec, index.root, index.binaries, index.load)

    def test_exact_six_require_split_and_other_thirty_forbid_it(self):
        self.assertEqual(sum(acceptance.needs_observer(spec) for spec in case_specs()), 6)
        for spec in case_specs():
            if not acceptance.needs_observer(spec):
                index = Index()
                with self.subTest(spec=spec), self.assertRaises(ValueError):
                    acceptance.validate_split(index.split(spec), spec, index.root, index.binaries, index.load)
        index = Index()
        records = [index.record(spec, base_pid=100 + 4 * i) for i, spec in enumerate(case_specs())]
        errors = acceptance.validate_suite(records, requirements(), index.root, index.binaries, index.load,
            pnet_key_file=Path("/unit/pnet.key"), pnet_fingerprint="b" * 64)
        self.assertEqual(len(errors), 6)

    def test_completion_validates_original_before_launch_and_retains_failure(self):
        index = Index()
        original = index.split(self.spec)["original"]
        original["status"] = "HARNESS_ERROR"
        original["errors"] = ["actual pre-ACK error"]
        before = deepcopy(original)
        self.active.side_effect = ValueError("pre-ACK failure")
        with patch.object(proof, "_snapshot", side_effect=index.load), patch.object(acceptance, "run_case") as run:
            result = acceptance.complete_split_case(original, self.spec, index.binaries, index.root,
                                                     index.binaries["rust-quic-observer"])
        run.assert_not_called()
        self.assertEqual(result["status"], "HARNESS_ERROR")
        self.assertIsNone(result["shutdown"])
        self.assertEqual(result["original"], before)
        self.assertIn("original: actual pre-ACK error", result["errors"])

    def test_completion_runs_the_same_case_independently_and_requires_full_terminal_semantics(self):
        for failed in (False, True):
            index = Index()
            pair = index.split(self.spec)
            shutdown = deepcopy(pair["shutdown"])
            shutdown.pop("evidence")
            shutdown["status"] = "captured"
            before = deepcopy(pair["original"])
            self.semantics.side_effect = ValueError("failed native shutdown") if failed else None
            with patch.object(proof, "_snapshot", side_effect=index.load), \
                    patch.object(acceptance, "run_case", return_value=shutdown) as run:
                result = acceptance.complete_split_case(pair["original"], self.spec, index.binaries, index.root,
                                                        index.binaries["rust-quic-observer"])
            run.assert_called_once_with(self.spec, {**index.binaries, "rust": index.binaries["rust-quic-observer"]},
                                        index.root / "quic-observer", command_attempt=None)
            self.assertEqual(result["original"], before)
            self.assertEqual(result["status"], "HARNESS_ERROR" if failed else "observed")
            if not failed:
                self.assertEqual(result["evidence"], pair["evidence"])
            else:
                self.assertNotIn("evidence", result)


class ExtensionIntegrationTests(unittest.TestCase):
    def test_global_index_binds_every_raw_source_and_rejects_hash_size_and_coverage_drift(self):
        from check_stage6_acceptance import raw_evidence_paths, sha256_file, validate_evidence_index
        spec = Case("rust", "forge", "partial", "native_quic")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()
            model = Index(root)
            record = model.record(spec)
            entries = []
            for path in sorted(raw_evidence_paths(record)):
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(model.payloads.get(str(path), {"unit_only_stdout": True})))
                entries.append({"path": str(path.relative_to(root)), "size": path.stat().st_size,
                                "sha256": sha256_file(path)})
            def check(index):
                return validate_evidence_index(root / "artifact.json", root, [record], index,
                                                {key: Path(value) for key, value in model.binaries.items()})
            paths, errors = check(entries)
            self.assertEqual(errors, [])
            self.assertEqual(len(paths), 16)
            for changed in (entries[:-1], entries + [entries[0]],
                            [{**entries[0], "sha256": "f" * 64}, *entries[1:]],
                            [{**entries[0], "size": entries[0]["size"] + 1}, *entries[1:]],
                            [{**entries[0], "path": "../foreign.json"}, *entries[1:]]):
                with self.subTest(index=changed[0]):
                    self.assertTrue(check(changed)[1])

    def test_real_capture_without_native_semantics_stays_failed(self):
        spec = Case("rust", "forge", "partial", "native_quic")
        record = Index().record(spec)
        record["status"] = "captured"
        record.pop("evidence")
        failed = acceptance.complete_case(record, spec)
        self.assertEqual(failed["status"], "HARNESS_ERROR")
        self.assertTrue(failed["errors"])

    def test_registration_is_not_support_and_promotion_has_separate_scope(self):
        from check_stage6_acceptance import ACCEPTANCE_SUITES, FOCUSED_SCENARIOS, required_scenarios
        from promote_stage6_acceptance import PROMOTION_SCOPES
        self.assertIn("pubsub-extensions", ACCEPTANCE_SUITES)
        self.assertEqual(FOCUSED_SCENARIOS["pubsub-extensions"], acceptance.SCENARIOS)
        self.assertIn("six independent", PROMOTION_SCOPES["pubsub-extensions"])
        self.assertIn("original Rust QUIC shutdown NOT_PROVEN", PROMOTION_SCOPES["pubsub-extensions"])
        manifest = json.loads(Path(__file__).with_name("p2p_donor_capabilities.json").read_text())
        entries = manifest["interop_acceptance_registry"]["capabilities"]
        for owner in acceptance.OWNERS.values():
            for scenario in entries[owner]["scenarios"]:
                scenario["registration"] = "registered"
        required, errors = required_scenarios(manifest, "pubsub-extensions")
        self.assertEqual(errors, [])
        self.assertEqual(required, requirements())
        for owner in acceptance.OWNERS.values():
            entries[owner]["scenarios"][0]["registration"] = "planned"
        _, errors = required_scenarios(manifest, "pubsub-extensions")
        self.assertTrue(errors)

    def test_runner_dispatch_is_exact36_with_only_six_independent_companions(self):
        tree = ast.parse(Path(__file__).with_name("runner.py").read_text())
        branches = [node for node in ast.walk(tree) if isinstance(node, ast.If)
                    and any(isinstance(child, ast.For) and isinstance(child.iter, ast.Call)
                            and isinstance(child.iter.func, ast.Name) and child.iter.func.id == "pubsub_extension_specs"
                            for child in node.body)]
        self.assertEqual(len(branches), 1)
        calls, completions, companions = [], [], []
        def capture(spec, binaries, root, **kwargs):
            calls.append((spec, binaries, root, kwargs))
            return {"status": "captured", "scenario_id": spec.identifier}
        def complete(record, spec, *, fingerprint):
            completions.append((spec, fingerprint))
            return {**record, "status": "HARNESS_ERROR" if spec.source == "rust" else "observed",
                    "errors": ["actual original terminal failure"] if spec.source == "rust" else [], "cleanup_errors": []}
        def split(record, spec, binaries, root, observer_binary, **kwargs):
            companions.append(spec)
            self.assertEqual(observer_binary, "/unit/rust-observer")
            return complete(record, spec, fingerprint=None)
        namespace = {"pubsub_extension_specs": case_specs, "run_pubsub_extension_case": capture,
                     "complete_pubsub_extension_case": complete, "binaries": {"rust": "/unit/original-rust"},
                     "root": Path("/unit"), "pnet_key_file": Path("/unit/pnet.key"), "pnet_fingerprint": "b" * 64,
                     "extension_needs_quic_observer": acceptance.needs_observer,
                     "complete_pubsub_extension_split": split, "observer_binary": "/unit/rust-observer",
                     "command_attempt": object(), "artifacts": [], "failures": []}
        dispatch = ast.Module(body=branches[0].body, type_ignores=[])
        exec(compile(ast.fix_missing_locations(dispatch), "runner_extensions_dispatch", "exec"), namespace)
        self.assertEqual(tuple(row[0] for row in calls), case_specs())
        self.assertEqual(len(namespace["artifacts"]), 36)
        self.assertEqual(len(namespace["failures"]), 9)
        self.assertEqual(companions, [spec for spec in case_specs() if acceptance.needs_observer(spec)])
        self.assertEqual(namespace["pnet_fingerprint"], "b" * 64)
        for (spec, binaries, root, kwargs), completed in zip(calls, completions):
            self.assertIs(binaries, namespace["binaries"])
            self.assertEqual(root, namespace["root"])
            expected = "b" * 64 if spec.profile == "private_tcp_yamux" else None
            self.assertEqual(kwargs, {"key": Path("/unit/pnet.key"), "fingerprint": expected,
                                      "command_attempt": namespace["command_attempt"]})
            self.assertEqual(completed, (spec, expected))


if __name__ == "__main__":
    unittest.main()
