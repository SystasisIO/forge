"""Synthetic indexed-source rejection tests, never live interoperability proof."""

from copy import deepcopy
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import coordinated_acceptance as acceptance
from coordinated_cases import case_specs
from test_coordinated_evidence import bilateral_artifact


class Index:
    """Model the shared loader's index boundary with explicitly synthetic JSON."""
    def __init__(self):
        self.payloads, self.loads = {}, []
        self.binaries = {name: f"/unit/{name}" for name in ("forge", "go", "rust")}

    def record(self, spec, pid_base=0):
        record = bilateral_artifact(spec.source, spec.destination, spec.profile)
        record["attempts"] = []
        record["phase_sources"] = {phase: {} for phase in ("started", "connected", "exchanged")}
        work = Path("/unit") / spec.identifier
        for role in ("source", "destination"):
            actor = record["raw"][role]
            owner = actor["process"]
            owner["pid"] += pid_base
            options = dict(zip(owner["command"][6::2], owner["command"][7::2]))
            for key in ("ready", "result"):
                flag = f"--{key}-file"
                snapshot = owner["log_file"] + f".{flag[2:]}.json"
                owner["outputs"].append({"argument": flag, "path": options[flag], "exists": True, "log_file": snapshot})
                self.payloads[snapshot] = deepcopy(actor[key])
            for phase in ("started", "connected", "exchanged"):
                snapshot = str(work / f"{role}.{phase}.json")
                record["phase_sources"][phase][role] = snapshot
                self.payloads[snapshot] = deepcopy(record["phases"][phase][role])
            record["attempts"].append({"kind": role, "scenario_id": spec.identifier, "attempt_id": 1,
                "command": deepcopy(owner["command"]), "requested_log_file": owner["log_file"], "timeout_seconds": 55,
                "exit_code": 0, "log_file": owner["log_file"], "pid": owner["pid"],
                "terminal_status": deepcopy(owner["terminal_status"]), "outputs": deepcopy(owner["outputs"])})
        return record

    def load(self, path):
        self.loads.append(path)
        if path not in self.payloads:
            raise OSError("synthetic unindexed or missing snapshot")
        return deepcopy(self.payloads[path])


def requirements():
    return {(acceptance.OWNER_ID, scenario): (acceptance.REUSE_DIRECTIONS, "passed", profile, stack,
            acceptance.REUSE_RUNNER_IDS[case_profile], capabilities, acceptance.evidence_contract_for(scenario))
            for case_profile, (scenario, profile, stack, capabilities) in acceptance.PROFILES.items()}


class CoordinatedRegistrationTests(unittest.TestCase):
    def manifest(self):
        manifest = json.loads(Path(__file__).with_name("p2p_donor_capabilities.json").read_text())
        entries = manifest["interop_acceptance_registry"]["capabilities"][acceptance.OWNER_ID]["scenarios"]
        for entry in entries:
            entry["registration"] = "registered"
        return manifest, entries

    def test_shared_checker_registers_exact_native_and_private_contracts(self):
        from check_stage6_acceptance import required_scenarios

        manifest, _ = self.manifest()
        required, errors = required_scenarios(manifest, "coordinated")
        self.assertEqual(errors, [])
        self.assertEqual(required, requirements())

    def test_planned_or_malformed_contracts_cannot_promote(self):
        from check_stage6_acceptance import required_scenarios

        baseline, _ = self.manifest()
        for field, value in (("registration", "planned"), ("required_directions", ["forge_to_go"]),
                             ("requires_capabilities", []), ("runner_scenario_id", "tcp_stage6/echo")):
            with self.subTest(field=field):
                manifest = deepcopy(baseline)
                entries = manifest["interop_acceptance_registry"]["capabilities"][acceptance.OWNER_ID]["scenarios"]
                private = next(entry for entry in entries if entry["profile"] == "private_network")
                private[field] = value
                self.assertTrue(required_scenarios(manifest, "coordinated")[1])


class CoordinatedAcceptanceTests(unittest.TestCase):
    def check(self, record, spec, index, **kwargs):
        return acceptance.validate_record(record, spec, Path("/unit"), index.binaries, index.load,
                                          pnet_key_file="/unit/psk", pnet_fingerprint="b" * 64, **kwargs)

    def test_shared_owned_output_and_all_ten_indexed_json_sources_are_used(self):
        for spec in list(case_specs())[:6]:
            index = Index()
            record = index.record(spec)
            before = deepcopy(record)
            with patch.object(acceptance, "_owned_output", wraps=acceptance._owned_output) as verifier:
                self.assertEqual(self.check(record, spec, index), [])
                self.assertEqual(verifier.call_count, 2)
            self.assertEqual(len(index.loads), 10)
            self.assertEqual(record, before)

    def test_status_cannot_replace_final_indexed_raw_outputs(self):
        index, spec = Index(), next(case_specs())
        record = index.record(spec)
        result = record["raw"]["source"]["process"]["outputs"][1]["log_file"]
        del index.payloads[result]
        self.assertTrue(self.check(record, spec, index))

    def test_tampered_embedded_final_or_phase_payload_is_rejected(self):
        for kind in ("final", "phase"):
            index, spec = Index(), next(case_specs())
            record = index.record(spec)
            if kind == "final":
                record["raw"]["source"]["result"]["receipt"]["native_outgoing_winner"] = False
            else:
                record["phases"]["connected"]["source"]["status"] = "exchanged"
            self.assertTrue(self.check(record, spec, index))

    def test_missing_historical_phase_cannot_be_invented_from_final_result(self):
        index, spec = Index(), next(case_specs())
        record = index.record(spec)
        del index.payloads[record["phase_sources"]["connected"]["source"]]
        self.assertTrue(self.check(record, spec, index))

    def test_phase_path_escape_or_reuse_final_snapshot_fails(self):
        for path in ("/outside/connected.json", "/unit/source.log.result-file.json"):
            index, spec = Index(), next(case_specs())
            record = index.record(spec)
            record["phase_sources"]["connected"]["source"] = path
            self.assertTrue(self.check(record, spec, index))

    def test_exact_binary_namespace_and_argv_are_checked(self):
        for position, value in ((4, "/unit/other-binary"), (3, "host-network"), (5, "dial")):
            index, spec = Index(), next(case_specs())
            record = index.record(spec)
            record["raw"]["source"]["process"]["command"][position] = value
            self.assertTrue(self.check(record, spec, index))

    def test_timeout_or_requested_role_change_is_not_native_context(self):
        for flag, value in (("--timeout-ms", "30000"), ("--coord-role", "responder")):
            index, spec = Index(), next(case_specs())
            record = index.record(spec)
            argv = record["raw"]["source"]["process"]["command"]
            argv[argv.index(flag) + 1] = value
            self.assertTrue(self.check(record, spec, index))

    def test_forged_attempt_missing_owner_and_forced_stop_fail_closed(self):
        for mutation in ("attempt", "owner", "stop"):
            index, spec = Index(), next(case_specs())
            record = index.record(spec)
            if mutation == "attempt":
                record["attempts"][0]["pid"] = 999
            elif mutation == "owner":
                record["processes"] = []
            else:
                record["raw"]["source"]["process"]["terminal_status"]["termination"] = "killed"
            self.assertTrue(self.check(record, spec, index))

    def test_private_key_path_and_fingerprint_must_match_trusted_launch_inputs(self):
        spec = list(case_specs())[4]
        for flag, value in (("--pnet-key-file", "/unit/untrusted-key"), ("--pnet-fingerprint", "c" * 64)):
            index = Index()
            record = index.record(spec)
            argv = record["raw"]["source"]["process"]["command"]
            argv[argv.index(flag) + 1] = value
            self.assertTrue(self.check(record, spec, index))
        index = Index()
        record = index.record(spec)
        self.assertTrue(acceptance.validate_record(record, spec, "/unit", index.binaries, index.load))

    def test_rust_metadata_only_private_fingerprint_remains_unaccepted(self):
        spec = list(case_specs())[6]
        index = Index()
        record = index.record(spec)
        errors = self.check(record, spec, index)
        self.assertTrue(any("installed protector fingerprint" in error for error in errors))

    def test_exact_eight_case_suite_source_matrix_is_independently_checked(self):
        index = Index()
        records = [index.record(spec, number * 10) for number, spec in enumerate(case_specs())]
        # Isolate source/matrix mechanics from actor semantic tests. This stub
        # does not establish acceptance, and no vector is exported as evidence.
        with patch.object(acceptance, "validate_case", return_value=[]):
            self.assertEqual(acceptance.validate_suite(records, requirements(), "/unit", index.binaries, index.load,
                             pnet_key_file="/unit/psk", pnet_fingerprint="b" * 64), [])
        self.assertEqual(len(index.loads), 80)

    def test_missing_duplicate_or_unrequired_matrix_direction_is_rejected(self):
        index = Index()
        records = [index.record(spec, number * 10) for number, spec in enumerate(case_specs())]
        for changed in (records[:-1], records[:7] + [records[0]]):
            self.assertTrue(acceptance.validate_suite(changed, requirements(), "/unit", index.binaries, index.load))
        wrong = requirements()
        native = next(key for key in wrong if key[1] == "coordinated_dial_port_reuse")
        wrong[native] = ({"forge_to_go"}, *wrong[native][1:])
        self.assertTrue(acceptance.validate_suite(records, wrong, "/unit", index.binaries, index.load))

    def test_cross_case_pid_reuse_is_rejected_even_with_indexed_outputs(self):
        index = Index()
        records = [index.record(spec) for spec in case_specs()]
        with patch.object(acceptance, "validate_case", return_value=[]):
            errors = acceptance.validate_suite(records, requirements(), "/unit", index.binaries, index.load,
                                              pnet_key_file="/unit/psk", pnet_fingerprint="b" * 64)
        self.assertTrue(any("reuse PIDs" in error for error in errors))

    def test_unknown_claim_or_missing_index_loader_is_rejected(self):
        index, spec = Index(), next(case_specs())
        record = index.record(spec)
        record["claimed_full_interop"] = True
        self.assertTrue(self.check(record, spec, index))
        self.assertTrue(acceptance.validate_record(record, spec, "/unit", index.binaries, None))


if __name__ == "__main__":
    unittest.main()
