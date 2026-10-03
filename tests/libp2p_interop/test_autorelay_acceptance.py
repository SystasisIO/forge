"""Synthetic indexed parser fixtures, not live PR9 or production evidence."""

import copy
import io
import json
from pathlib import Path
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

import autorelay_acceptance as acceptance
from autorelay_cases import case_specs
from autorelay_evidence import HOP
import check_stage6_acceptance as checker
import promote_stage6_acceptance as promotion
from provenance import WorktreeIdentity
from test_autorelay_evidence import (
    NOW, RELAY, TARGET, address, actor, connection, echo, push_wire_receipt, reservation_wire_receipt, valid_case,
)


SOURCE = Path(__file__).resolve().parent


def native_reservations(implementation, elapsed, expiry, renewed=False):
    common = {"peer_id": TARGET, "protocol": HOP, "unix_ms": NOW + elapsed}
    if implementation == "go":
        return [{**connection(TARGET), **common, "kind": "reservation_response", "status": 100,
                 "capture_complete": True, "expires_unix_ms": NOW + expiry}]
    accepted = {**common, "kind": "reservation_accepted", "renewed": renewed,
                "basis": "native_relay_ReservationReqAccepted"}
    return ([{**connection(TARGET), "kind": "connection", "unix_ms": NOW + elapsed}]
            if not renewed else []) + [accepted]


def transport_payload(value, implementation, transport):
    if isinstance(value, str):
        return value.replace("/udp/", "/tcp/").replace("/quic-v1", "") if transport != "quic" else value
    if isinstance(value, list):
        return [transport_payload(item, implementation, transport) for item in value]
    if not isinstance(value, dict):
        return value
    result = {key: transport_payload(item, implementation, transport) for key, item in value.items()}
    if result.get("transport") == "quic":
        result["transport"] = transport
    if "connection_id" in result and "remote_addr" in result and transport != "quic":
        result.pop("negotiated_transport", None)
        security = "/tls/1.0.0" if transport == "tcp-tls" else "/noise"
        if implementation == "rust":
            result["upgrade_observation"] = {"overflow": False, "connections": [{
                "authenticated_remote_peer_id": result["peer_id"], "security_complete": True,
                "muxer_complete": True, "selected_security": security, "selected_muxer": "/yamux/1.0.0",
            }]}
        else:
            result.update(negotiated_transport="tcp", negotiated_security=security,
                          negotiated_muxer="/yamux/1.0.0")
    return result


def semantic_case(spec):
    record = valid_case()
    record.update(case={"source": spec.source, "relay": spec.relay, "destination": spec.destination,
                        "transport": spec.transport, "kind": spec.kind}, scenario_id=spec.identifier,
                  status="passed", errors=[], suite="autorelay", acceptance_scenario_ids=acceptance.claims_for(spec),
                  elapsed_seconds=20)
    if spec.kind == "lifecycle":
        replacement_impl = "rust" if spec.relay == "go" else "go"
        for role, impl in (("relay", spec.relay), ("replacement", replacement_impl), ("observer", spec.source)):
            for key in ("ready", "result"):
                record["raw"][role][key]["implementation"] = impl
        record["raw"]["relay"]["result"]["events"] = (
            native_reservations(spec.relay, 1000, 9000) + native_reservations(spec.relay, 6000, 14000, True))
        record["raw"]["replacement"]["result"]["events"] = native_reservations(replacement_impl, 12000, 20000)
        if spec.source == "rust":
            pushes = [e for e in record["raw"]["observer"]["result"]["events"] if e["kind"] == "identify_push"]
            for push in pushes:
                push.pop("native_update_sequence")
                push.pop("native_completed_sequence")
                push.update(basis="native_Identify_Received_with_unique_inbound_push_wire_receipt")
    else:
        ready = {"implementation": spec.destination, "status": "ready", "peer_id": TARGET,
                 "relay_peer_id": RELAY, "protocol": HOP, "relay_connection": connection(RELAY),
                 "expires_unix_ms": NOW + 9000, "reservation_accepted": True,
                 "reservation_basis": "native_relayclient.Reserve" if spec.destination == "go" else
                                      "native_relay_client_ReservationReqAccepted_and_NewListenAddr"}
        record["raw"] = {
            "relay": actor("forge", "service", RELAY, {"observations": [
                {"relay_bytes": 10, "service_reservations": 1, "stopped": False},
                {"relay_bytes": 10, "service_reservations": 0, "stopped": True}]}),
            "destination": {"ready": ready, "process": {}},
        }
        record["echoes"], record["faults"] = [echo("service_echo", RELAY, 2000)], []
    for label, data in record["raw"].items():
        data["ready"]["listen_addrs"] = [address(data["ready"]["peer_id"])]
        impl = data["ready"]["implementation"]
        record["raw"][label] = transport_payload(data, impl, spec.transport)
    for index, data in enumerate(record["echoes"]):
        data["result"]["implementation"] = spec.source
        record["echoes"][index] = transport_payload(data, spec.source, spec.transport)
    if spec.kind == "lifecycle" and spec.source == "rust":
        pushes = [e for e in record["raw"]["observer"]["result"]["events"] if e["kind"] == "identify_push"]
        for index, push in enumerate(pushes, 1):
            push["wire_receipts"] = [push_wire_receipt(push["addresses"], index)]
    if spec.kind == "service" and spec.destination == "rust":
        record["raw"]["destination"]["ready"]["native_reservation_receipt"] = reservation_wire_receipt(
            record["raw"]["relay"]["ready"]["listen_addrs"])
    return record


def owned_case(spec, root, binaries, case_index):
    record, pid = semantic_case(spec), 10000 + case_index * 10
    work = root / spec.identifier
    work.mkdir(parents=True)
    record["processes"], record["attempts"] = [], []

    def own(data, implementation, action, log, options, label, echo_actor=False):
        nonlocal pid
        pid += 1
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text("synthetic owned process output\n")
        command = [str(binaries[implementation]), action]
        for key, value in options.items():
            command.extend([key, value])
        owner = {"pid": pid, "command": command, "log_file": str(log),
                 "terminal_status": {"exit_code": 0, "termination": "graceful"},
                 "ready": data.get("ready", {}), "outputs": []}
        if implementation == "forge" and action in ("autorelay-destination", "autorelay-service") \
                and options["--transport"] in ("tcp", "tcp-tls"):
            owner["stop_budget"] = {"native_close_seconds": 5.0, "post_stop_seconds": 1.2,
                                    "scheduler_allowance_seconds": 2.0, "seconds": 8.2}
        for flag, key in (("--ready-file", "ready"), ("--result-file", "result")):
            if flag not in options:
                continue
            snapshot = str(log) + f".{flag[2:]}.json"
            Path(snapshot).write_text(json.dumps(data[key]))
            owner["outputs"].append({"argument": flag, "path": options[flag], "exists": True, "log_file": snapshot})
            if echo_actor:
                Path(options[flag]).write_text(json.dumps(data[key]))
        data["process"] = owner
        record["processes"].append(owner)
        if label != "destination" or spec.kind == "lifecycle":
            record["attempts"].append({
                "kind": "relay_dial" if echo_actor else label,
                "scenario_id": "autorelay" if echo_actor else spec.identifier, "attempt_id": 1,
                "command": command, "pid": pid, "log_file": str(log), "requested_log_file": str(log),
                "timeout_seconds": 15 if echo_actor else 60, "exit_code": 0,
                "terminal_status": owner["terminal_status"], "outputs": owner["outputs"],
            })

    for label, data in record["raw"].items():
        impl, action, log, options = acceptance._actor_launch(spec, label, work, record["raw"])
        own(data, impl, action, log, options, label)
    for data in record["echoes"]:
        base = work / data["phase"] / f"{spec.source}-relay-dial-autorelay"
        data["result_file"] = str(base) + ".json"
        options = {"--scenario": "autorelay", "--transport": spec.transport,
                   "--peer-id": TARGET, "--relay-peer-id": data["result"]["relay_peer"],
                   "--relay-addr": data["result"]["relayed_addr"].split("/p2p-circuit")[0],
                   "--result-file": data["result_file"], "--store-dir": str(base) + "-store"}
        own(data, spec.source, "dial-relay", Path(str(base) + ".log"), options, data["phase"], True)
    return record


class AutoRelayAcceptanceTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.source = self.root / "tests/libp2p_interop"
        self.source.mkdir(parents=True)
        (self.source / "runner.py").write_text("# synthetic parser fixture, never executed\n")
        self.manifest = checker.load_json(SOURCE / "p2p_donor_capabilities.json")
        self.manifest_path = self.source / "p2p_donor_capabilities.json"
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.build = self.root / "build"
        self.artifact_root = self.build / "autorelay-run"
        self.artifact_root.mkdir(parents=True)
        self.artifact_path = self.build / "autorelay-artifacts.json"
        self.donors = self.root / "donors"
        self.donors.mkdir()
        self.binaries = {impl: self.build / impl for impl in ("forge", "go", "rust")}
        for path in self.binaries.values():
            path.write_text("synthetic binary, never executed\n")
        self.identity = WorktreeIdentity("a" * 40, "b" * 64, False)
        self.records = [owned_case(spec, self.artifact_root, self.binaries, index)
                        for index, spec in enumerate(case_specs())]
        identity, now = self.identity.as_json(), time.time() - 10
        self.artifact = {
            "schema_version": 2, "started_at_unix": now, "finished_at_unix": now + 1,
            "runner_argv": [str(Path(sys.executable).resolve()), str(self.source / "runner.py"),
                            "--enabled", "1", "--forge-fixture", str(self.binaries["forge"]),
                            "--source-dir", str(self.source), "--build-dir", str(self.build),
                            "--forge-root", str(self.root), "--donors-root", str(self.donors),
                            "--acceptance-manifest", str(self.manifest_path), "--suite", "autorelay"],
            "acceptance_manifest": {"path": str(self.manifest_path), "sha256": checker.sha256_file(self.manifest_path)},
            "artifact_root": str(self.artifact_root), "artifacts": self.records, "failures": [], "evidence_index": [],
            "fixture_provenance": {
                "forge_worktree": {"start": identity, "end": identity, "changed_during_run": False},
                "fixture_build_info": {"schema_version": 2, "forge": identity,
                    "compiler": {"path": "/unit/clang", "id": "Clang", "version": "22"}, "build_profile": "self-test"},
                "tools": {"python": {"path": str(Path(sys.executable).resolve()), "version_output": "synthetic-python"}},
                "binaries": {impl: {"path": str(path), "sha256": checker.sha256_file(path)}
                             for impl, path in self.binaries.items()},
                "runner_inputs": {"suite": "autorelay", "source_dir": str(self.source), "build_dir": str(self.build),
                                  "forge_root": str(self.root), "donors_root": str(self.donors),
                                  "acceptance_manifest": str(self.manifest_path)},
            },
        }
        self.save(True)

    def save(self, refresh_index=False):
        if refresh_index:
            self.artifact["evidence_index"] = checker.build_evidence_index(self.artifact_root, self.records)
        self.artifact_path.write_text(json.dumps(self.artifact))

    def receipt(self):
        return {"schema_version": 2, "runner_argv": self.artifact["runner_argv"],
                "started_at_unix": self.artifact["started_at_unix"] - 1,
                "finished_at_unix": self.artifact["finished_at_unix"] + 1, "returncode": 0,
                "invocation_directory": str(self.build), "artifact_path": str(self.artifact_path),
                "artifact_sha256": checker.sha256_file(self.artifact_path)}

    def validate(self, receipt=None, suite="autorelay"):
        self.save()
        with patch.object(checker, "validate_git_state", return_value=(1.0, [])), \
             patch.object(checker, "worktree_identity", return_value=self.identity), \
             patch.object(checker, "validate_donor_provenance", return_value=[]), \
             patch.object(checker.subprocess, "check_output", return_value="synthetic-python\n"):
            errors, limited = checker.validate(self.root, self.manifest_path, self.artifact_path,
                                               self.identity.head, receipt, expected_suite=suite)
        self.assertFalse(limited)
        return errors

    def assert_rejected(self, text=None):
        errors = self.validate()
        self.assertTrue(errors)
        if text:
            self.assertTrue(any(text in error for error in errors), errors)

    def recapture(self, record):
        for data in list(record["raw"].values()) + record["echoes"]:
            for output in data["process"]["outputs"]:
                key = "ready" if output["argument"] == "--ready-file" else "result"
                Path(output["log_file"]).write_text(json.dumps(data[key]))
                if "result_file" in data and key == "result":
                    Path(data["result_file"]).write_text(json.dumps(data[key]))
        self.save(True)

    def test_all_twelve_raw_cases_and_bound_promotion_receipt(self):
        self.assertEqual(self.validate(), [])
        self.assertEqual(self.validate(self.receipt()), [])
        self.assertEqual(len(self.records), 12)
        self.assertEqual(sum(len(r["processes"]) for r in self.records), 60)

    def test_exact_stop_budget_accepts_all_eight_scoped_forge_owners(self):
        scoped = [owner for record in self.records for owner in record["processes"] if "stop_budget" in owner]
        self.assertEqual(len(scoped), 8)
        for owner in scoped:
            self.assertEqual(owner["stop_budget"], {"native_close_seconds": 5.0, "post_stop_seconds": 1.2,
                "scheduler_allowance_seconds": 2.0, "seconds": 8.2})
        self.assertEqual(self.validate(), [])
        self.assertEqual(self.validate(self.receipt()), [])

    def test_missing_scoped_stop_budget_rejects_every_tcp_tls_case(self):
        for index, record in enumerate(self.records):
            if record["case"]["transport"] == "quic":
                continue
            original = copy.deepcopy(record)
            label = "destination" if record["case"]["kind"] == "lifecycle" else "relay"
            with self.subTest(case=record["scenario_id"]):
                del record["raw"][label]["process"]["stop_budget"]
                self.assert_rejected("invalid exact process owner")
            self.records[index] = original

    def test_stop_budget_requires_closed_exact_finite_nonbool_numeric_fields(self):
        spec = case_specs()[4]
        original = copy.deepcopy(self.records[4])
        valid = original["raw"]["destination"]["process"]["stop_budget"]
        invalid = [None, True, [], {**valid, "extra": 0}]
        for key in valid:
            missing = dict(valid)
            del missing[key]
            invalid.append(missing)
            for value in (valid[key] + 0.01, True, False, str(valid[key]), None,
                          float("inf"), float("-inf"), float("nan"), 10 ** 400):
                invalid.append({**valid, key: value})
        for budget in invalid:
            with self.subTest(budget=budget):
                record = copy.deepcopy(original)
                record["raw"]["destination"]["process"]["stop_budget"] = budget
                with self.assertRaisesRegex(ValueError, "stop budget"):
                    acceptance.validate_record(record, spec, self.artifact_root, self.binaries,
                                               lambda path: checker.load_json(Path(path)))

    def test_stop_budget_is_forbidden_on_quic_donor_and_echo_owners(self):
        selections = [(0, "destination"), (2, "relay"), (4, "relay"), (5, "relay"),
                      (4, "observer"), (5, "observer"), (4, "replacement"), (5, "replacement"),
                      (6, "destination"), (7, "destination"), (6, None), (7, None)]
        budget = self.records[4]["raw"]["destination"]["process"]["stop_budget"]
        for index, label in selections:
            original = copy.deepcopy(self.records[index])
            with self.subTest(case=original["scenario_id"], label=label):
                actor = self.records[index]["raw"][label] if label else self.records[index]["echoes"][0]
                actor["process"]["stop_budget"] = dict(budget)
                self.assert_rejected("invalid exact process owner")
            self.records[index] = original

    def test_scoped_budget_does_not_allow_unknown_owner_fields_or_forced_cleanup(self):
        original = copy.deepcopy(self.records[4])
        for change, error in (({"other_budget": 8.2}, "invalid exact process owner"),
                              ({"terminal_status": {"exit_code": 0, "termination": "terminated"}}, "graceful joined shutdown"),
                              ({"terminal_status": {"exit_code": 0, "termination": "killed"}}, "graceful joined shutdown")):
            with self.subTest(change=change):
                self.records[4] = copy.deepcopy(original)
                self.records[4]["raw"]["destination"]["process"].update(change)
                self.assert_rejected(error)

    def test_missing_suite_and_empty_claims_use_exact_case_mapping(self):
        for record in self.records:
            record.pop("suite")
            record["acceptance_scenario_ids"] = []
        self.assertEqual(self.validate(), [])

    def test_stage6_also_requires_complete_registered_pr9_matrix(self):
        # A partial standalone manifest tests dispatch, never full promotion.
        self.artifact_root = self.build / "interop-run"
        self.artifact_root.mkdir()
        self.records[:] = [owned_case(spec, self.artifact_root, self.binaries, index)
                           for index, spec in enumerate(case_specs())]
        self.artifact_path = self.build / "interop-artifacts.json"
        self.artifact["artifact_root"] = str(self.artifact_root)
        self.artifact["runner_argv"] = self.artifact["runner_argv"][:-2]
        self.artifact["fixture_provenance"]["runner_inputs"]["suite"] = "stage6"
        registry = self.manifest["interop_acceptance_registry"]
        owners = {owner for owner, _, _, _ in acceptance.SCENARIOS.values()}
        registry["capabilities"] = {owner: entry for owner, entry in registry["capabilities"].items() if owner in owners}
        registry["evidence_contracts"] = sorted(acceptance.EVIDENCE_CONTRACTS)
        self.manifest_path.write_text(json.dumps(self.manifest))
        self.artifact["acceptance_manifest"]["sha256"] = checker.sha256_file(self.manifest_path)
        self.save(True)
        self.assertEqual(self.validate(suite="stage6"), [])
        self.records.pop()
        self.save(True)
        self.assertTrue(any("all 12 cases" in error for error in self.validate(suite="stage6")))

    def test_every_case_is_required_and_duplicates_cannot_cover_directions(self):
        complete = list(self.records)
        for index in range(12):
            with self.subTest(missing=complete[index]["scenario_id"]):
                self.records[:] = complete[:index] + complete[index + 1:]
                self.save(True)
                self.assert_rejected("all 12 cases")
        self.records[:] = complete[:-1] + [copy.deepcopy(complete[0])]
        self.save(True)
        self.assert_rejected("all 12 cases")

    def test_manifest_has_six_exact_staged_roles_not_production_support(self):
        required, errors = checker.required_scenarios(self.manifest, "autorelay")
        self.assertEqual(errors, [])
        self.assertEqual(len(required), 6)
        capabilities = {c["id"]: c for c in self.manifest["capabilities"]}
        for (owner, name), value in required.items():
            self.assertEqual(value[0], acceptance.ROLE_DIRECTIONS[acceptance.SCENARIOS[name][1]])
            self.assertEqual(capabilities[owner]["decision"], "stage_6")
        registry = self.manifest["interop_acceptance_registry"]
        registry["capabilities"]["relay.autorelay_lifecycle"]["scenarios"][0]["required_directions"].append("go_to_forge")
        self.assertTrue(checker.required_scenarios(self.manifest, "autorelay")[1])

    def test_partial_older_manifest_does_not_require_pr9(self):
        self.assertEqual(checker.required_scenarios(checker.fixture_manifest())[1], [])
        self.assertTrue(checker.required_scenarios(checker.fixture_manifest(), "autorelay")[1])

    def test_manifest_missing_registered_or_wrong_tls_contract(self):
        for change in ({"registration": "planned"}, {"runner_scenario_id": "tcp_stage6/relay_v2_service_native_tcp_tls_yamux"}):
            manifest = copy.deepcopy(self.manifest)
            manifest["interop_acceptance_registry"]["capabilities"]["relay.circuit_v2_service"]["scenarios"][-1].update(change)
            self.assertTrue(checker.required_scenarios(manifest, "autorelay")[1])

    def test_wrong_role_transport_or_extra_claim_is_rejected(self):
        record = self.records[0]
        record["acceptance_scenario_ids"] = ["relay_v2_service"]
        self.assert_rejected("wrong Forge role")
        record["acceptance_scenario_ids"] = acceptance.claims_for(case_specs()[0])
        record["case"]["transport"] = "tcp"
        self.assert_rejected("identity/status")

    def test_embedded_payload_must_equal_indexed_final_snapshot(self):
        self.records[0]["raw"]["destination"]["result"]["complete"] = False
        self.assert_rejected("embedded payload differs")

    def test_unindexed_or_stale_raw_snapshot_and_tampered_hash(self):
        snapshot = Path(self.records[0]["raw"]["relay"]["process"]["outputs"][-1]["log_file"])
        snapshot.write_text('{"status":"passed"}')
        self.assert_rejected("hash or size differs")
        self.save(True)
        self.assert_rejected("embedded payload differs")
        self.artifact["evidence_index"].pop()
        self.assert_rejected("does not exactly cover")

    def test_exact_binary_paths_commands_and_pids_cannot_be_replaced(self):
        record = self.records[0]
        owner = record["raw"]["destination"]["process"]
        command = list(owner["command"])
        owner["command"][0] = str(self.binaries["go"])
        self.assert_rejected("fixture binary/action/options")
        owner["command"][:] = command
        record["processes"][0] = copy.deepcopy(record["processes"][0])
        record["processes"][0]["pid"] += 100000
        self.assert_rejected("canonical owned process/PID")

    def test_reused_snapshots_escaped_paths_extra_owners_and_attempts(self):
        record = self.records[0]
        original = copy.deepcopy(record)
        record["raw"]["destination"]["process"]["outputs"][-1]["log_file"] = record["raw"]["relay"]["process"]["outputs"][-1]["log_file"]
        self.save(True)
        self.assert_rejected("terminal snapshot")
        self.records[0] = copy.deepcopy(original)
        self.records[0]["raw"]["destination"]["process"]["command"].extend(["--result-file", "/tmp/escaped.json"])
        self.save(True)
        self.assert_rejected("duplicate fixture flags")
        self.records[0] = copy.deepcopy(original)
        self.records[0]["processes"].append(copy.deepcopy(original["processes"][0]))
        self.save(True)
        self.assert_rejected("missing/duplicate owned")
        self.records[0] = copy.deepcopy(original)
        self.records[0]["attempts"].append(copy.deepcopy(original["attempts"][0]))
        self.save(True)
        self.assert_rejected("duplicate exact owned attempt")

    def test_silent_stdout_is_owned_but_empty_json_is_never_proof(self):
        owner = self.records[0]["raw"]["destination"]["process"]
        Path(owner["log_file"]).write_bytes(b"")
        self.save(True)
        self.assertEqual(self.validate(), [])
        Path(owner["outputs"][-1]["log_file"]).write_bytes(b"")
        self.save(True)
        self.assert_rejected("empty non-process proof")

    def test_missing_shutdown_lease_renewal_replacement_push_and_manual_acquisition(self):
        original = copy.deepcopy(self.records[0])
        mutations = {
            "peer-cache": lambda r: r["raw"]["destination"]["result"].update(reservations_basis="peer_cache"),
            "shutdown": lambda r: r["raw"]["destination"]["result"]["observations"].pop(),
            "lease": lambda r: r["raw"]["destination"]["result"]["observations"][1].update(reservations=[]),
            "renewal": lambda r: r["raw"]["relay"]["result"]["events"].pop(),
            "replacement": lambda r: r["raw"]["replacement"]["result"]["events"].pop(),
            "manual": lambda r: r["raw"]["destination"]["result"].update(operation_basis="manual_reserve"),
            "self query": lambda r: r["raw"]["observer"]["result"].update(events=[
                e for e in r["raw"]["observer"]["result"]["events"] if e["kind"] != "identify_push"]),
        }
        for label, mutate in mutations.items():
            with self.subTest(label=label):
                self.records[0] = copy.deepcopy(original)
                mutate(self.records[0])
                self.recapture(self.records[0])
                self.assert_rejected()

    def test_wrong_native_security_rejected_even_with_new_index(self):
        record = next(r for r in self.records if r["case"]["transport"] == "tcp-tls" and r["case"]["source"] == "go")
        record["echoes"][0]["result"]["relay_connection"]["negotiated_security"] = "/noise"
        self.recapture(record)
        self.assert_rejected("security/muxer")

    def test_rust_push_bytes_and_identity_rejected_even_with_new_index(self):
        record = self.records[1]
        pushes = [e for e in record["raw"]["observer"]["result"]["events"] if e["kind"] == "identify_push"]
        original = copy.deepcopy(pushes[0]["wire_receipts"])
        wire = pushes[0]["wire_receipts"][0]
        wire["framed_hex"] = "00" + wire["framed_hex"][2:]
        self.recapture(record)
        self.assert_rejected("bytes/hash mismatch")
        pushes[0]["wire_receipts"] = [push_wire_receipt(pushes[0]["addresses"], 1, key_byte=9)]
        self.recapture(record)
        self.assert_rejected("key differs from authenticated peer")
        pushes[0]["wire_receipts"] = original

    def test_signed_native_vouchers_are_bound_to_indexed_final_readiness(self):
        for record in self.records:
            if record["case"]["kind"] != "service":
                continue
            ready = record["raw"]["destination"]["ready"]
            if record["case"]["destination"] == "rust":
                ready["native_reservation_receipt"] = reservation_wire_receipt(
                    record["raw"]["relay"]["ready"]["listen_addrs"], signed=True)
            else:
                ready.update(voucher=True, voucher_validated=True, voucher_relay=RELAY, voucher_peer=TARGET,
                             voucher_expiration=(NOW + 9000) // 1000,
                             voucher_validation_basis="native_Reserve_ConsumeEnvelope_signature_then_fixture_identity_expiry_checks")
            self.recapture(record)
        self.assertEqual(self.validate(), [])
        record = self.records[2]
        record["raw"]["destination"]["ready"]["native_reservation_receipt"]["voucher_peer"] = RELAY
        self.recapture(record)
        self.assert_rejected("verification receipt")

    def test_rust_service_must_have_final_native_hop_bytes(self):
        record = self.records[2]
        del record["raw"]["destination"]["ready"]["native_reservation_receipt"]
        self.recapture(record)
        self.assert_rejected("native accepted reservation")

    def test_final_owner_bool_exit_or_timeout_cannot_pass(self):
        self.records[0]["raw"]["destination"]["process"]["terminal_status"]["exit_code"] = False
        self.assert_rejected("graceful joined shutdown")
        self.records[0]["raw"]["destination"]["process"]["terminal_status"]["exit_code"] = 0
        self.records[0]["attempts"][0]["timeout_class"] = "fixture_timeout"
        self.assert_rejected("attempt differs")

    def test_repeated_owner_views_and_output_exists_require_exact_types(self):
        record = self.records[0]
        record["processes"][0] = copy.deepcopy(record["processes"][0])
        record["processes"][0]["terminal_status"]["exit_code"] = False
        self.assert_rejected("owned processes or PIDs")
        record["processes"][0]["terminal_status"]["exit_code"] = 0
        record["raw"]["relay"]["process"]["outputs"][0]["exists"] = 1
        self.assert_rejected("terminal snapshot")

    def test_process_pid_cannot_be_reused_across_cases(self):
        record = self.records[1]
        owner = record["raw"]["relay"]["process"]
        old = owner["pid"]
        owner["pid"] = self.records[0]["processes"][0]["pid"]
        for attempt in record["attempts"]:
            if attempt["pid"] == old:
                attempt["pid"] = owner["pid"]
        self.assert_rejected("cases reuse owned process PIDs")

    def test_clean_head_binary_hash_and_execution_receipt_still_required(self):
        receipt = self.receipt()
        receipt["artifact_sha256"] = "0" * 64
        self.assertTrue(any("receipt does not bind" in error for error in self.validate(receipt)))
        self.binaries["forge"].write_text("changed binary")
        self.assert_rejected("binary path or SHA-256")
        self.artifact["fixture_provenance"]["forge_worktree"]["start"]["dirty"] = True
        self.assert_rejected("clean expected worktree")

    def test_suite_argv_root_and_unrelated_records_cannot_be_relabeled(self):
        self.artifact["runner_argv"][-1] = "stage6"
        self.assert_rejected("argv suite differs")
        self.artifact["runner_argv"][-1] = "autorelay"
        self.records.append({"suite": "mdns"})
        self.assert_rejected("unrelated records")

    def test_matrix_definition_cannot_drop_required_cases(self):
        with patch.object(acceptance, "case_specs", return_value=case_specs()[:-1]):
            self.assert_rejected("exactly 12 unique")


class AutoRelayPromotionTests(unittest.TestCase):
    def test_standalone_checker_selects_suite_but_never_claims_promotion(self):
        argv = ["check_stage6_acceptance.py", "/unit/root", "/unit/manifest", "/unit/artifact", "a" * 40,
                "--suite", "autorelay"]
        with patch.object(checker.sys, "argv", argv), \
             patch.object(checker, "validate", return_value=([], False)) as validate, \
             patch("sys.stdout", new_callable=io.StringIO) as output:
            self.assertEqual(checker.main(), 0)
        self.assertEqual(validate.call_args.kwargs["expected_suite"], "autorelay")
        self.assertTrue(output.getvalue().startswith("CONSISTENT:"))
        self.assertNotIn("PASS", output.getvalue())

    def test_promotion_runs_exact_suite_and_owns_successful_receipt(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary).resolve()
            argv = ["promote_stage6_acceptance.py", "--runner", str(root / "runner.py"),
                    "--forge-fixture", str(root / "forge"), "--source-dir", str(root / "source"),
                    "--build-dir", str(root / "build"), "--forge-root", str(root),
                    "--donors-root", str(root / "donors"), "--acceptance-manifest",
                    str(root / promotion.CANONICAL_ACCEPTANCE_MANIFEST), "--expected-head", "a" * 40,
                    "--suite", "autorelay"]
            with patch.object(promotion.sys, "argv", argv), patch.object(promotion.subprocess, "run") as run, \
                 patch.object(promotion, "validate", return_value=(["synthetic rejection"], False)) as validate:
                run.return_value.returncode = 0
                self.assertEqual(promotion.main(), 1)
            command = run.call_args.args[0]
            self.assertEqual(command[-2:], ["--suite", "autorelay"])
            self.assertEqual(validate.call_args.kwargs["expected_suite"], "autorelay")
            receipt = validate.call_args.args[4]
            self.assertEqual(receipt["runner_argv"], command)
            self.assertEqual(Path(receipt["artifact_path"]).name, "autorelay-artifacts.json")
            self.assertTrue((Path(receipt["invocation_directory"]) / "autorelay-promotion-receipt.json").is_file())


if __name__ == "__main__":
    unittest.main()
