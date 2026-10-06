import unittest
import copy
from dataclasses import asdict
from pathlib import Path
from unittest.mock import patch

from path_acceptance import (DIRECTIONS, _actor_launch, _sources, claims_for, validate_matrix,
                             validate_record, validate_suite)
from path_cases import RUNNER_SCENARIO_ID, case_specs
from path_evidence import RUST_SOURCE_WAVE_SCOPE, rust_source_wave_case, validate_case
from test_path_evidence import TOKEN, unit_record, unit_source_wave

REQUIRED = {("relay.dcutr", "dcutr"): (DIRECTIONS, "passed", "native", ("quic",),
            RUNNER_SCENARIO_ID, (), "forge.p2p.evidence.dcutr.v1")}

def source_doubles(root, *, actual_wave_schema=False):
    """Immutable-index unit doubles, not actual processes or live acceptance."""
    records, indexed = [], {}
    binaries = {impl: Path("/unit") / (impl + "-path-fixture") for impl in ("forge", "go", "rust")}
    for case_index, spec in enumerate(case_specs(), 1):
        record = unit_source_wave(spec) if actual_wave_schema and rust_source_wave_case(asdict(spec)) else unit_record()
        record.pop("test_double")
        record.update(case=asdict(spec), scenario_id=spec.identifier, runner_scenario_id=RUNNER_SCENARIO_ID,
                      acceptance_scenario_ids=claims_for(spec), elapsed_seconds=2,
                      application_actor="destination" if spec.source == "forge" else "source", processes=[], attempts=[])
        if spec.outcome == "cancelled":
            record.setdefault("cancellation", {})
        if rust_source_wave_case(asdict(spec)):
            record["terminal_scope"] = RUST_SOURCE_WAVE_SCOPE
            record.setdefault("native_wave_failure", {})
        work = root / spec.identifier
        for role, actor in record["raw"].items():
            actor["ready"]["implementation"] = getattr(spec, role)
            impl, log, options = _actor_launch(spec, role, work, record)
            command = ["/unit/ip", "netns", "exec", "path-" + role + "-" + "0" * 12,
                       str(binaries[impl]), "path-live"]
            for flag, value in options.items():
                command.extend((flag, value))
            outputs = [{"argument": flag, "path": options[flag], "exists": True,
                        "log_file": str(log) + "." + flag[2:] + ".json"}
                       for flag in ("--ready-file", "--result-file")]
            owner = {"pid": case_index * 10 + len(record["processes"]), "command": command, "log_file": str(log),
                     "ready": actor["ready"], "outputs": outputs,
                     "terminal_status": {"exit_code": 0, "termination": "graceful"},
                     "stop_budget": {"native_close_seconds": 8, "post_stop_seconds": 0,
                                     "scheduler_allowance_seconds": 2, "seconds": 10}}
            actor["process"] = owner
            record["processes"].append(owner)
            record["attempts"].append({"kind": role, "scenario_id": spec.identifier, "attempt_id": 1,
                                      "requested_log_file": str(log), "timeout_seconds": 55, "exit_code": 0,
                                      **{k: owner[k] for k in ("pid", "command", "log_file", "terminal_status", "outputs")}})
            for output, key in zip(outputs, ("ready", "result")):
                indexed[output["log_file"]] = copy.deepcopy(actor[key])
        records.append(record)
    return records, binaries, indexed


class PathAcceptanceTests(unittest.TestCase):
    def test_closed_matrix_covers_all_four_directions_and_negative_controls(self):
        specs = list(case_specs())
        self.assertEqual(len(specs), 12)
        self.assertEqual(len({spec.identifier for spec in specs}), 12)
        for spec in specs:
            self.assertEqual((spec.transport, spec.profile), ("quic", "native"))
            self.assertEqual(claims_for(spec), ["dcutr"] if spec.outcome == "success" else [])

    def test_missing_live_evidence_or_provenance_never_promotes_claim(self):
        errors = validate_record({"status": "passed", "relay_echo": True}, verify_provenance=None)
        self.assertIn("coordinator binary/raw-output provenance verifier is required", errors)
        self.assertTrue(validate_matrix([], verify_provenance=lambda _: []))

    def test_suite_validates_real_source_contract_without_opaque_callback(self):
        root = Path("/unit/artifacts")
        records, binaries, indexed = source_doubles(root)
        required = REQUIRED
        # Only semantic doubles are replaced here; the concrete binary, argv,
        # lifecycle, snapshot and matrix checks execute, never a live promotion.
        with patch("path_acceptance.validate_case", return_value=[]):
            self.assertEqual(validate_suite(records, required, root, binaries, indexed.__getitem__), [])
            mutations = (
                lambda r: r[0]["raw"]["source"]["process"]["command"].__setitem__(4, "/unit/wrong-binary"),
                lambda r: r[0]["raw"]["source"]["process"]["command"].__setitem__(3, "unowned-namespace"),
                lambda r: r[0]["raw"]["source"]["process"]["stop_budget"].__setitem__("seconds", 100),
                lambda r: r[0]["raw"]["source"]["process"]["terminal_status"].__setitem__("termination", "terminated"),
                lambda r: r[0]["raw"]["source"]["ready"].__setitem__("peer_id", "stale-embedded-peer"),
                lambda r: r[0]["attempts"].append(copy.deepcopy(r[0]["attempts"][0])),
                lambda r: r[0].__setitem__("acceptance_scenario_ids", ["coordinated_dial_port_reuse"]),
                lambda r: r[0]["attempts"][0].__setitem__("exit_code", True),
            )
            for mutate in mutations:
                value = copy.deepcopy(records)
                mutate(value)
                self.assertTrue(validate_suite(value, required, root, binaries, indexed.__getitem__))
            self.assertTrue(validate_suite(records, required, root, binaries, lambda _: {}))
            self.assertTrue(validate_suite(records, required, root, binaries, None))
            self.assertTrue(validate_suite(records, required, root, binaries,
                lambda path: (_ for _ in ()).throw(ValueError("not indexed"))))
            for value in ((DIRECTIONS, "passed", "private_network", ("quic",), RUNNER_SCENARIO_ID, (), "forge.p2p.evidence.dcutr.v1"),
                          (DIRECTIONS, "passed", "native", ("tcp",), RUNNER_SCENARIO_ID, (), "forge.p2p.evidence.dcutr.v1"),
                          (DIRECTIONS, "passed", "native", ("quic",), "quic_topology/dcutr_relay_topology", (), "forge.p2p.evidence.dcutr.v1")):
                self.assertTrue(validate_suite(records, {("relay.dcutr", "dcutr"): value}, root, binaries, indexed.__getitem__))
        self.assertTrue(validate_suite(records, required, root, binaries, indexed.__getitem__),
                        "source-only unit doubles cannot pass actual path semantics")

    def test_suite_keeps_all_cancellation_directions_and_separate_native_manifest(self):
        records, binaries, indexed = source_doubles(Path("/unit/artifacts"))
        required = REQUIRED
        self.assertTrue(validate_suite(records[:-1], required, "/unit/artifacts", binaries, indexed.__getitem__))
        value = records[:-1] + [records[0]]
        self.assertTrue(validate_suite(value, required, "/unit/artifacts", binaries, indexed.__getitem__))
        self.assertTrue(validate_suite(records, {("connections.coordinated_dial_port_reuse", "coordinated_dial_port_reuse"):
            (DIRECTIONS, "passed")}, "/unit/artifacts", binaries, indexed.__getitem__))

    def test_sources_do_not_mutate_raw_commands_or_invent_receipts(self):
        root = Path("/unit/artifacts")
        records, binaries, indexed = source_doubles(root)
        original = copy.deepcopy(records[0])
        _sources(records[0], next(case_specs()), root, binaries, indexed.__getitem__)
        self.assertEqual(records[0], original)

    def test_wave_scope_is_source_negative_only_and_does_not_bypass_indexed_outputs(self):
        root = Path("/unit/artifacts")
        records, binaries, indexed = source_doubles(root)
        scoped = next(r for r in records if r.get("terminal_scope") == RUST_SOURCE_WAVE_SCOPE)
        spec = next(s for s in case_specs() if s.identifier == scoped["scenario_id"])
        _sources(scoped, spec, root, binaries, indexed.__getitem__)
        for key in ("terminal_scope", "native_wave_failure"):
            value = copy.deepcopy(scoped)
            value.pop(key)
            with self.assertRaises(ValueError):
                _sources(value, spec, root, binaries, indexed.__getitem__)
        value = copy.deepcopy(scoped)
        value["terminal_scope"] = "native_dcutr_completion"
        with self.assertRaises(ValueError):
            _sources(value, spec, root, binaries, indexed.__getitem__)
        with self.assertRaises(ValueError):
            _sources(scoped, spec, root, binaries, lambda _: {})
        for record in records:
            if record.get("terminal_scope") == RUST_SOURCE_WAVE_SCOPE:
                continue
            other = next(s for s in case_specs() if s.identifier == record["scenario_id"])
            value = copy.deepcopy(record)
            value.update(terminal_scope=RUST_SOURCE_WAVE_SCOPE, native_wave_failure={})
            with self.assertRaises(ValueError):
                _sources(value, other, root, binaries, indexed.__getitem__)

    def test_actual_wave_schema_is_checked_after_owned_indexed_raw_hydration(self):
        # Native-schema semantic/source doubles, not a binary-provenance or
        # live-promotion test. No validator is replaced in this focused gate.
        root = Path("/unit/artifacts")
        records, binaries, indexed = source_doubles(root, actual_wave_schema=True)
        for record in records:
            if record.get("terminal_scope") != RUST_SOURCE_WAVE_SCOPE:
                continue
            spec = next(s for s in case_specs() if s.identifier == record["scenario_id"])
            hydrated = _sources(record, spec, root, binaries, indexed.__getitem__)
            self.assertEqual(validate_case(hydrated), [])
            for key, wrong in (("terminal_scope", "aggregate_native_terminal"), ("native_wave_failure", None)):
                value = copy.deepcopy(record)
                value[key] = wrong
                if key == "terminal_scope":
                    with self.assertRaises(ValueError):
                        _sources(value, spec, root, binaries, indexed.__getitem__)
                else:
                    self.assertTrue(validate_case(_sources(value, spec, root, binaries, indexed.__getitem__)))
            value = copy.deepcopy(record)
            value["native_wave_failure"]["events"][0]["remote_peer_id"] = "unindexed-owner"
            self.assertTrue(validate_case(_sources(value, spec, root, binaries, indexed.__getitem__)))
            source = next(o["log_file"] for o in record["raw"]["source"]["process"]["outputs"]
                          if o["argument"] == "--result-file")
            def changed_loader(path):
                value = copy.deepcopy(indexed[path])
                if path == source:
                    value["events"][0]["remote_peer_id"] = "changed-source-file"
                return value
            with self.assertRaises(ValueError):
                _sources(record, spec, root, binaries, changed_loader)


if __name__ == "__main__":
    unittest.main()
