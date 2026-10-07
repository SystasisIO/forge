"""Synthetic active-proof regressions; not live interop evidence."""

from copy import deepcopy
import json
import tempfile
import unittest
from dataclasses import asdict
from pathlib import Path
from unittest.mock import patch

from pubsub_cases import Case
from pubsub_evidence import validate_active_case, validate_case
from pubsub_quic_proof import SCOPE, needs_observer, validate_original, validate_split
from test_pubsub_acceptance import Index
from test_pubsub_evidence import synthetic_case
from provenance import sha256_file


def active_snapshots(value):
    snapshots = {}
    for row in value["shutdown_barrier"]["operations"][:4]:
        role = row["actor"]
        captured = deepcopy(value["raw"][role])
        done = next(event for event in captured["events"] if event.get("kind") == "command_done"
                    and event.get("command_kind") == "prepare_shutdown")
        captured["events"] = captured["events"][:done["sequence"]]
        captured.update(finalized=False, joined=False)
        snapshots[role] = {"schema_version": 1, "source": "python.fixture.native_prepare_snapshot",
                           "actor": role, "case_token": value["case_token"], "pid": value["processes"][role]["pid"],
                           "command_sequence": row["command_sequence"], "ack_event_sequence": row["ack_event_sequence"],
                           "result": captured}
    return snapshots


class ActiveProofTests(unittest.TestCase):
    def test_same_full_traffic_obligations_without_claiming_terminal_success(self):
        value = synthetic_case()
        snapshots = active_snapshots(value)
        expected = validate_case(value)
        value["raw"]["sink"]["error"] = "opaque original shutdown failure retained"
        value["processes"]["sink"]["returncode"] = 1
        value["errors"] = ["original shutdown not proven"]
        before = deepcopy(value)
        self.assertEqual(validate_active_case(value, snapshots), expected)
        self.assertEqual(value, before)
        with self.assertRaises(ValueError):
            validate_case(value)

    def test_no_foreign_owner_token_or_missing_actual_join(self):
        for mode in ("token", "pid", "ack", "forced", "signal", "duplicate", "terminal"):
            with self.subTest(mode=mode):
                value = synthetic_case()
                snapshots = active_snapshots(value)
                if mode == "token":
                    snapshots["sink"]["case_token"] = "b" * 32
                elif mode == "pid":
                    snapshots["sink"]["pid"] += 100
                elif mode == "ack":
                    snapshots["sink"]["ack_event_sequence"] += 1
                elif mode == "forced":
                    value["processes"]["sink"]["forced_termination"] = True
                elif mode == "signal":
                    value["processes"]["sink"]["returncode"] = -9
                elif mode == "duplicate":
                    value["processes"]["sink"]["pid"] = value["processes"]["offender"]["pid"]
                    snapshots["sink"]["pid"] = value["processes"]["sink"]["pid"]
                else:
                    snapshots["sink"]["result"].update(finalized=True, joined=True)
                with self.assertRaises(ValueError):
                    validate_active_case(value, snapshots)

    def test_missing_p4_or_repaired_delivery_cannot_be_replaced_by_ack(self):
        for mode in ("p4", "delivery", "prefix", "overflow", "sticky"):
            with self.subTest(mode=mode):
                value = synthetic_case()
                snapshots = active_snapshots(value)
                captured = snapshots["victim"]["result"]
                if mode == "p4":
                    snapshot = next(event for event in captured["events"] if event.get("label") == "penalized")
                    snapshot["peer_scores"][0]["value"] = 0
                elif mode == "delivery":
                    captured = snapshots["sink"]["result"]
                    event = next(event for event in captured["events"] if event.get("kind") == "delivery")
                    event["propagation_peer"] = "foreign"
                elif mode == "prefix":
                    captured["events"][0]["peer_id"] = "foreign"
                elif mode == "overflow":
                    captured["overflow"] = True
                else:
                    captured["error"] = "failure before prepare"
                with self.assertRaises(ValueError):
                    validate_active_case(value, snapshots)

    def test_observer_scope_is_exactly_four_rust_quic_cases(self):
        from pubsub_cases import case_specs
        selected = [spec for spec in case_specs() if needs_observer(spec)]
        self.assertEqual(len(selected), 4)
        self.assertTrue(all(spec.profile == "native_quic" and "rust" in (spec.source, spec.destination)
                            for spec in selected))
        self.assertFalse(needs_observer(Case("go", "forge", "1.1", "native_quic")))


class OriginalSourceBindingTests(unittest.TestCase):
    spec = Case("go", "forge", "1.1", "native_quic")

    def test_original_index_and_active_prefix_are_independently_loaded(self):
        index = Index()
        record = index.record(self.spec)
        expected = validate_case(record)
        facts, paths, pids = validate_original(record, self.spec, Path("/unit"), index.binaries, index.load)
        self.assertEqual(facts, expected)
        self.assertEqual(len(paths), 16)
        self.assertEqual(len(pids), 4)
        self.assertEqual(set(index.loads), set(index.payloads))

    def test_failure_diagnostic_remains_unmodified_and_is_not_a_shutdown_pass(self):
        index = Index()
        record = index.record(self.spec)
        role = "sink"
        owner = record["processes"][role]
        record["raw"][role]["error"] = "original failure after active preparation"
        record["status"] = "HARNESS_ERROR"
        record["errors"] = ["original shutdown NOT_PROVEN"]
        owner["returncode"] = 1
        owner["terminal_status"]["exit_code"] = 1
        attempt = next(value for value in record["attempts"] if value["pid"] == owner["pid"])
        attempt["exit_code"] = 1
        attempt["terminal_status"]["exit_code"] = 1
        index.payloads[owner["outputs"][1]["log_file"]] = deepcopy(record["raw"][role])
        before = deepcopy(record)
        validate_original(record, self.spec, Path("/unit"), index.binaries, index.load)
        self.assertEqual(record, before)
        with self.assertRaises(ValueError):
            validate_case(record)

    def test_original_binary_snapshot_or_owner_substitution_is_rejected(self):
        for mode in ("binary", "attempt", "snapshot", "ready", "forced", "missing", "extra_attempt"):
            with self.subTest(mode=mode):
                index = Index()
                record = index.record(self.spec)
                owner = record["processes"]["sink"]
                if mode == "binary":
                    owner["command"][0] = "/unit/observer-rust"
                elif mode == "attempt":
                    record["attempts"][-1]["pid"] += 100
                elif mode == "snapshot":
                    index.payloads[owner["outputs"][1]["log_file"]]["events"][0]["peer_id"] = "foreign"
                elif mode == "ready":
                    owner["ready"]["local_peer_id"] = "foreign"
                    index.payloads[owner["outputs"][0]["log_file"]] = deepcopy(owner["ready"])
                elif mode == "forced":
                    owner["forced_termination"] = True
                elif mode == "missing":
                    index.payloads.pop(owner["outputs"][1]["log_file"])
                else:
                    record["attempts"].append(deepcopy(record["attempts"][0]))
                with self.assertRaises((ValueError, OSError)):
                    validate_original(record, self.spec, Path("/unit"), index.binaries, index.load)


class SplitProofBindingTests(unittest.TestCase):
    """Mock only native causal checks to exercise cross-execution binding itself."""

    spec = Case("rust", "forge", "1.1", "native_quic")

    def check(self, mode):
        facts = {"protocol": "/meshsub/1.1.0"}
        original = {"case_token": "a" * 32}
        shutdown = {"case_token": "b" * 32, "processes": {"sink": {"pid": 22}}}
        record = {"schema_version": 1, "suite": "pubsub-scoring", "scenario_id": self.spec.identifier,
                  "acceptance_scenario_id": self.spec.scenario, "runner_scenario_id": self.spec.runner_id,
                  "case": asdict(self.spec), "proof_scope": SCOPE, "status": "observed", "errors": [],
                  "cleanup_errors": [], "original": original, "shutdown": shutdown,
                  "evidence": {"original_wire": facts, "instrumented_shutdown": facts,
                               "original_shutdown": "NOT_PROVEN"}}
        old_paths, new_paths, old_pids = {Path("/unit/original")}, {Path("/unit/observer")}, {11}
        if mode == "token":
            shutdown["case_token"] = original["case_token"]
        elif mode == "pid":
            shutdown["processes"]["sink"]["pid"] = 11
        elif mode == "path":
            new_paths = old_paths
        elif mode == "claim":
            record["evidence"]["original_shutdown"] = "PASS"
        elif mode == "scope":
            record["proof_scope"] = "unmodified_donor_shutdown"
        elif mode == "extra":
            record["legacy_cause"] = "borrowed"
        with patch("pubsub_quic_proof.validate_original", return_value=(facts, old_paths, old_pids)), \
                patch("pubsub_quic_proof.validate_case", return_value=facts):
            return validate_split(record, self.spec, Path("/unit"), {"rust-quic-observer": "/unit/rust-observer"},
                                  lambda _: {}, lambda *args: new_paths)

    def test_independent_bindings_and_declared_limitation(self):
        self.assertEqual(self.check("valid"), ({Path("/unit/original"), Path("/unit/observer")}, {11, 22}))

    def test_cross_run_borrowing_and_undeclared_claims_are_rejected(self):
        for mode in ("token", "pid", "path", "claim", "scope", "extra"):
            with self.subTest(mode=mode), self.assertRaises(ValueError):
                self.check(mode)


class DiagnosticEvidenceIndexTests(unittest.TestCase):
    """Real file/index validation with synthetic traffic, never interop proof."""

    def check(self, mode):
        from check_stage6_acceptance import validate_evidence_index
        spec = Case("go", "forge", "1.1", "native_quic")
        source = Index()
        original = source.record(spec)
        owner = original["processes"]["sink"]
        original["raw"]["sink"]["error"] = "opaque native shutdown failure"
        original.update(status="HARNESS_ERROR", errors=["original shutdown NOT_PROVEN"])
        owner["returncode"] = owner["terminal_status"]["exit_code"] = 1
        attempt = next(row for row in original["attempts"] if row["pid"] == owner["pid"])
        attempt["exit_code"] = attempt["terminal_status"]["exit_code"] = 1
        source.payloads[owner["outputs"][1]["log_file"]] = deepcopy(original["raw"]["sink"])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory).resolve()

            def relocate(value):
                if isinstance(value, dict):
                    return {key: relocate(nested) for key, nested in value.items()}
                if isinstance(value, list):
                    return [relocate(nested) for nested in value]
                return str(root) + value[5:] if isinstance(value, str) and value.startswith("/unit/") else value

            original = relocate(original)
            owners = original["processes"]
            for name, payload in source.payloads.items():
                path = Path(relocate(name))
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(json.dumps(relocate(payload)))
            for row in owners.values():
                Path(row["log_file"]).write_bytes(b"")
            record = {"proof_scope": SCOPE, "scenario_id": spec.identifier, "original": original}
            if mode == "unscoped":
                record.pop("proof_scope")
            elif mode == "forced":
                owners["sink"]["forced_termination"] = True
            elif mode == "binary":
                owners["sink"]["command"][0] = "/foreign/observer"
            elif mode == "snapshot":
                path = Path(owners["sink"]["outputs"][1]["log_file"])
                payload = json.loads(path.read_text())
                payload["events"][0]["peer_id"] = "foreign"
                path.write_text(json.dumps(payload))
            index = [{"path": str(path.relative_to(root)), "size": path.stat().st_size,
                      "sha256": sha256_file(path)} for path in root.rglob("*") if path.is_file()]
            if mode == "hash":
                next(row for row in index if row["path"].endswith("sink.log"))["sha256"] = "0" * 64
            # Only the matrix selection is doubled: every actual active-proof,
            # native owner, snapshot, PID, command and file hash check runs.
            with patch("pubsub_cases.case_specs", return_value=(spec,)), \
                    patch("pubsub_quic_proof.needs_observer", return_value=True):
                result = validate_evidence_index(root / "artifact.json", root, [record], index,
                                                {key: Path(relocate(value)) for key, value in source.binaries.items()})
            self.assertEqual(original["status"], "HARNESS_ERROR")
            self.assertEqual(original["raw"]["sink"]["error"], "opaque native shutdown failure")
            return result

    def test_joined_original_exit_one_empty_stdout_is_diagnostic_not_an_empty_proof(self):
        _, errors = self.check("valid")
        self.assertEqual(errors, [])

    def test_empty_stdout_cannot_bypass_original_owners_or_index_integrity(self):
        for mode in ("unscoped", "forced", "binary", "snapshot", "hash"):
            with self.subTest(mode=mode):
                _, errors = self.check(mode)
                self.assertTrue(errors)


if __name__ == "__main__":
    unittest.main()
