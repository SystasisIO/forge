"""Synthetic provenance rejection tests, never live interoperability proof."""

from copy import deepcopy
from dataclasses import asdict
import ast
import json
from pathlib import Path
import unittest
from unittest.mock import patch

import pubsub_acceptance as acceptance
from pubsub_cases import Case, case_specs
from pubsub_quic_proof import needs_observer
from pubsub_evidence import AUTHENTICATION, PROFILES, validate_case
from test_pubsub_evidence import synthetic_case


class Index:
    def __init__(self):
        self.payloads, self.loads = {}, []
        self.binaries = {name: f"/unit/{name}" for name in ("forge", "go", "rust")}

    def record(self, spec, *, pnet_key_file=None, fingerprint=None):
        value = synthetic_case(lower_quic=spec.profile == "native_quic")
        if spec.profile == "private_tcp_yamux":
            for raw in value["raw"].values():
                for event in raw["events"]:
                    if event["kind"] == "connection":
                        event.update(authentication_basis=AUTHENTICATION[raw["implementation"]]["tcp-pnet-noise"],
                                     pnet_verified=True, pnet_fingerprint=fingerprint)
        value.update(case=asdict(spec), scenario_id=spec.identifier, acceptance_scenario_id=spec.scenario,
                     runner_scenario_id=spec.runner_id, status="observed", attempts=[])
        value["evidence"] = validate_case(value, expected_fingerprint=fingerprint if spec.profile == "private_tcp_yamux" else None)
        work = Path("/unit") / spec.identifier
        for role, raw in value["raw"].items():
            implementation = raw["implementation"]
            options = {"--version": spec.version, "--transport": PROFILES[spec.profile], "--actor": role,
                       "--case-token": value["case_token"], "--store-dir": str(work / f"{role}.store")}
            options.update({f"--{name}-file": str(work / f"{role}.{name}")
                            for name in ("ready", "result", "stop", "control")})
            if spec.profile == "private_tcp_yamux":
                options.update({"--pnet-key-file": str(pnet_key_file), "--pnet-fingerprint": fingerprint})
            owner = value["processes"][role]
            ready = {"schema_version": 1, "implementation": implementation, "actor": role,
                     "case_token": value["case_token"], "local_peer_id": raw["local_peer_id"],
                     "peer_id": raw["local_peer_id"], "listen_addrs": ["/ip4/127.0.0.1/udp/4000/quic-v1"
                         if spec.profile == "native_quic" else "/ip4/127.0.0.1/tcp/4000"],
                     "topic": "forge-pr11:" + value["case_token"], "ready": True, "subscription_created": True}
            owner.update(command=[self.binaries[implementation], "pubsub-live",
                                  *[part for pair in options.items() for part in pair]],
                         log_file=str(work / f"{role}.log"),
                         terminal_status={"exit_code": 0, "termination": "graceful"},
                         ready=ready, outputs=[],
                         stop_budget={"native_close_seconds": 8, "post_stop_seconds": 0,
                                      "scheduler_allowance_seconds": 2, "seconds": 10})
            for name in ("ready", "result"):
                snapshot = owner["log_file"] + f".{name}-file.json"
                owner["outputs"].append({"argument": f"--{name}-file", "path": options[f"--{name}-file"],
                                         "exists": True, "log_file": snapshot})
                self.payloads[snapshot] = deepcopy(owner["ready"] if name == "ready" else raw)
            value["attempts"].append({"kind": role, "scenario_id": spec.identifier, "attempt_id": 1,
                "command": deepcopy(owner["command"]), "requested_log_file": owner["log_file"],
                "timeout_seconds": 60, "exit_code": 0, "log_file": owner["log_file"], "pid": owner["pid"],
                "terminal_status": deepcopy(owner["terminal_status"]), "outputs": deepcopy(owner["outputs"])})
            row = next(row for row in value["shutdown_barrier"]["operations"][:4] if row["actor"] == role)
            row["evidence_file"] = str(work / f"{role}.prepare-result.json")
            captured = deepcopy(raw)
            completion = next(event for event in captured["events"]
                              if event.get("kind") == "command_done"
                              and event.get("command_sequence") == row["command_sequence"]
                              and event.get("command_kind") == "prepare_shutdown")
            captured["events"] = captured["events"][:completion["sequence"]]
            captured.update(finalized=False, joined=False)
            self.payloads[row["evidence_file"]] = {
                "schema_version": 1, "source": "python.fixture.native_prepare_snapshot", "actor": role,
                "case_token": value["case_token"], "pid": owner["pid"], "command_sequence": row["command_sequence"],
                "ack_event_sequence": row["ack_event_sequence"], "result": captured}
        return value

    def load(self, path):
        self.loads.append(path)
        if path not in self.payloads:
            raise OSError("synthetic unindexed snapshot")
        return deepcopy(self.payloads[path])


class PubSubAcceptanceTests(unittest.TestCase):
    spec = Case("go", "forge", "1.1", "native_quic")

    def check(self, value, index):
        return acceptance._sources(value, self.spec, Path("/unit"), index.binaries, index.load, None, None)

    def change_ready(self, value, index, role, mutation):
        owner = value["processes"][role]
        mutation(owner["ready"])
        index.payloads[owner["outputs"][0]["log_file"]] = deepcopy(owner["ready"])

    def test_actual_shared_owner_verifier_loads_eight_terminal_and_four_prepare_sources(self):
        index = Index()
        value = index.record(self.spec)
        before = deepcopy(value)
        with patch.object(acceptance, "_owned_output", wraps=acceptance._owned_output) as verifier:
            self.assertEqual(len(self.check(value, index)), 16)
            self.assertEqual(verifier.call_count, 4)
        self.assertEqual(len(index.loads), 12)
        self.assertEqual(set(index.loads), set(index.payloads))
        self.assertEqual(value, before)

    def test_public_go_quic_reload_is_not_contaminated_by_private_fingerprint(self):
        index = Index()
        value = index.record(self.spec)
        before = deepcopy(value)
        with self.assertRaisesRegex(ValueError, "foreign profile/peer/protocol"):
            validate_case(value, expected_fingerprint="b" * 64)
        self.assertEqual(len(acceptance._sources(value, self.spec, Path("/unit"), index.binaries, index.load,
                                               Path("/unit/pnet.key"), "b" * 64)), 16)
        self.assertEqual(len(index.loads), 12)
        self.assertEqual(value, before)

    def test_private_reload_requires_actual_matching_native_fingerprint(self):
        spec = Case("go", "forge", "1.1", "private_tcp_yamux")
        key, fingerprint = Path("/unit/pnet.key"), "b" * 64
        for mode in ("valid", "missing_key", "missing_expected", "wrong_expected", "missing_native", "wrong_native",
                     "unverified_native"):
            with self.subTest(mode=mode):
                index = Index()
                value = index.record(spec, pnet_key_file=key, fingerprint=fingerprint)
                if mode in {"missing_native", "wrong_native", "unverified_native"}:
                    raw = value["raw"]["offender"]
                    owner = next(event for event in raw["events"] if event["kind"] == "connection")
                    if mode == "missing_native":
                        owner.pop("pnet_fingerprint")
                    elif mode == "wrong_native":
                        owner["pnet_fingerprint"] = "c" * 64
                    else:
                        owner["pnet_verified"] = False
                    output = value["processes"]["offender"]["outputs"][1]["log_file"]
                    index.payloads[output] = deepcopy(raw)
                def check():
                    return acceptance._sources(value, spec, Path("/unit"), index.binaries, index.load,
                        None if mode == "missing_key" else key,
                        None if mode == "missing_expected" else "c" * 64 if mode == "wrong_expected" else fingerprint)
                if mode == "valid":
                    self.assertEqual(len(check()), 16)
                    self.assertEqual(len(index.loads), 12)
                else:
                    with self.assertRaises(ValueError):
                        check()

    def test_runner_scopes_private_fingerprint_in_all_actual_pubsub_dispatches(self):
        tree = ast.parse(Path(__file__).with_name("runner.py").read_text())
        branches = [node for node in ast.walk(tree) if isinstance(node, ast.If)
                    and any(isinstance(child, ast.For) and isinstance(child.iter, ast.Call)
                            and isinstance(child.iter.func, ast.Name) and child.iter.func.id == "pubsub_specs"
                            for child in node.body)]
        self.assertEqual(len(branches), 1)
        key, fingerprint, attempt = Path("/unit/pnet.key"), "b" * 64, object()
        calls = []
        def capture(spec, binaries, root, *, key, fingerprint, command_attempt):
            calls.append((spec, binaries, root, key, fingerprint, command_attempt))
            return {"status": "observed"}
        namespace = {"pubsub_specs": case_specs, "run_pubsub_case": capture, "binaries": {"go": "synthetic-go"},
                     "root": Path("/unit"), "pnet_key_file": key, "pnet_fingerprint": fingerprint,
                     "command_attempt": attempt, "artifacts": [], "failures": []}
        observer_calls = []
        def complete(original, spec, binaries, root, observer_binary, *, command_attempt):
            observer_calls.append((spec, observer_binary, command_attempt))
            return original
        namespace.update(needs_quic_observer=needs_observer, complete_pubsub_case=complete,
                         observer_binary=Path("/unit/observer-rust"))
        dispatch = ast.Module(body=branches[0].body, type_ignores=[])
        exec(compile(ast.fix_missing_locations(dispatch), "runner_pubsub_dispatch", "exec"), namespace)
        self.assertEqual(len(observer_calls), 4)
        self.assertTrue(all(needs_observer(spec) and binary == Path("/unit/observer-rust") and owner is attempt
                            for spec, binary, owner in observer_calls))
        self.assertEqual(tuple(call[0] for call in calls), case_specs())
        self.assertEqual(len(namespace["artifacts"]), 24)
        self.assertEqual(namespace["failures"], [])
        self.assertEqual(namespace["pnet_fingerprint"], fingerprint)
        for spec, binaries, root, actual_key, actual_fingerprint, actual_attempt in calls:
            with self.subTest(case=spec.identifier):
                self.assertIs(binaries, namespace["binaries"])
                self.assertEqual(root, namespace["root"])
                self.assertEqual(actual_key, key)
                self.assertEqual(actual_fingerprint, fingerprint if spec.profile == "private_tcp_yamux" else None)
                self.assertIs(actual_attempt, attempt)

    def test_prepare_sources_cannot_be_missing_foreign_reused_or_terminal(self):
        mutations = (
            lambda snapshot: snapshot.update(pid=True),
            lambda snapshot: snapshot.update(pid=999),
            lambda snapshot: snapshot.update(actor="foreign"),
            lambda snapshot: snapshot.update(case_token="b" * 32),
            lambda snapshot: snapshot.update(command_sequence=2),
            lambda snapshot: snapshot.update(ack_event_sequence=1),
            lambda snapshot: snapshot.update(source="manifest.claim"),
            lambda snapshot: snapshot.update(extra_claim=True),
            lambda snapshot: snapshot["result"].update(finalized=True),
            lambda snapshot: snapshot["result"].update(joined=True),
            lambda snapshot: snapshot["result"].update(error="real native failure"),
            lambda snapshot: snapshot["result"]["events"].clear(),
            lambda snapshot: snapshot["result"]["events"][-2].update(local_peer_id="foreign"),
            lambda snapshot: snapshot["result"]["events"][-1].update(status="error"),
            lambda snapshot: snapshot["result"]["events"][0].update(mono_ns=999),
        )
        for role in ("victim", "offender", "replacement", "sink"):
            for mutation in mutations:
                with self.subTest(role=role, mutation=mutation):
                    index = Index()
                    value = index.record(self.spec)
                    row = next(row for row in value["shutdown_barrier"]["operations"][:4] if row["actor"] == role)
                    mutation(index.payloads[row["evidence_file"]])
                    with self.assertRaises(ValueError):
                        self.check(value, index)
            index = Index()
            value = index.record(self.spec)
            row = next(row for row in value["shutdown_barrier"]["operations"][:4] if row["actor"] == role)
            del index.payloads[row["evidence_file"]]
            with self.assertRaises(OSError):
                self.check(value, index)
        index = Index()
        value = index.record(self.spec)
        rows = value["shutdown_barrier"]["operations"]
        rows[1]["evidence_file"] = rows[0]["evidence_file"]
        with self.assertRaises(ValueError):
            self.check(value, index)

    def test_final_index_and_manifest_cannot_rewrite_the_captured_prepare_ack(self):
        for role in ("victim", "offender", "replacement", "sink"):
            index = Index()
            value = index.record(self.spec)
            row = next(row for row in value["shutdown_barrier"]["operations"][:4] if row["actor"] == role)
            row["command_sequence"] = 2
            for event in value["raw"][role]["events"]:
                if event.get("kind") == "shutdown_prepared" or (
                        event.get("kind") == "command_done" and event.get("command_kind") == "prepare_shutdown"):
                    event["command_sequence"] = 2
            output = value["processes"][role]["outputs"][1]["log_file"]
            index.payloads[output] = deepcopy(value["raw"][role])
            with self.subTest(role=role), self.assertRaisesRegex(ValueError, "prepare"):
                self.check(value, index)

    def test_prepare_prefix_rejects_equal_values_with_different_json_types(self):
        for role in ("victim", "offender", "replacement", "sink"):
            for field, replacement in (("sequence", True), ("mono_ns", 1.0)):
                with self.subTest(role=role, field=field):
                    index = Index()
                    value = index.record(self.spec)
                    row = next(row for row in value["shutdown_barrier"]["operations"][:4]
                               if row["actor"] == role)
                    event = index.payloads[row["evidence_file"]]["result"]["events"][0]
                    self.assertEqual(event[field], replacement)
                    self.assertIs(type(event[field]), int)
                    event[field] = replacement
                    # Native receipt typing can reject the altered JSON value before the prefix comparison.
                    with self.assertRaisesRegex(ValueError, "event prefix|exact indexed native sequence/clock"):
                        self.check(value, index)

    def test_shutdown_barrier_is_required_without_accepting_extra_record_claims(self):
        for mutation in (lambda value: value.pop("shutdown_barrier"),
                         lambda value: value.update(shutdown_preparation={}),
                         lambda value: value["shutdown_barrier"].update(prepared=True),
                         lambda value: value["shutdown_barrier"]["operations"][0].update(acknowledged=True)):
            index = Index()
            value = index.record(self.spec)
            mutation(value)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(value, index)

    def test_indexed_canonical_readiness_rejects_missing_false_and_truthy_booleans(self):
        for role in ("victim", "offender", "replacement", "sink"):
            for field in ("ready", "subscription_created"):
                for mutation in (lambda ready, field=field: ready.pop(field),
                                 *[lambda ready, field=field, invalid=invalid: ready.update({field: invalid})
                                   for invalid in (None, False, 0, 1, "true", "ready")]):
                    with self.subTest(role=role, field=field, mutation=mutation):
                        index = Index()
                        value = index.record(self.spec)
                        self.change_ready(value, index, role, mutation)
                        with self.assertRaisesRegex(ValueError, "native canonical readiness"):
                            self.check(value, index)

    def test_indexed_canonical_readiness_binds_schema_owner_topic_and_identity(self):
        invalid_fields = (("schema_version", True), ("schema_version", 2), ("schema_version", "1"),
                          ("implementation", "foreign"), ("actor", "foreign"), ("case_token", "b" * 32),
                          ("topic", "forge-pr11:" + "b" * 32), ("local_peer_id", "foreign-peer"),
                          ("peer_id", "foreign-peer"), ("listen_addrs", []))
        for role in ("victim", "offender", "replacement", "sink"):
            for field, invalid in invalid_fields:
                with self.subTest(role=role, field=field):
                    for mutation in (lambda ready, field=field: ready.pop(field),
                                     lambda ready, field=field, invalid=invalid: ready.update({field: invalid})):
                        index = Index()
                        value = index.record(self.spec)
                        self.change_ready(value, index, role, mutation)
                        with self.assertRaisesRegex(ValueError, "native canonical readiness"):
                            self.check(value, index)
            index = Index()
            value = index.record(self.spec)
            self.change_ready(value, index, role,
                              lambda ready: ready.update(local_peer_id="foreign-peer", peer_id="foreign-peer"))
            with self.assertRaisesRegex(ValueError, "native canonical readiness"):
                self.check(value, index)

    def test_indexed_ready_status_and_subscribed_alias_cannot_replace_native_booleans(self):
        index = Index()
        value = index.record(self.spec)
        def legacy(ready):
            ready.pop("ready")
            ready.pop("subscription_created")
            ready.update(status="ready", subscribed=True)
        self.change_ready(value, index, "victim", legacy)
        with self.assertRaisesRegex(ValueError, "native canonical readiness"):
            self.check(value, index)

    def test_indexed_acceptance_rejects_active_partial_or_nonboolean_terminal_flags(self):
        for role in ("victim", "offender", "replacement", "sink"):
            for finalized, joined in ((False, False), (True, False), (False, True), (1, True), (True, 1),
                                      (None, True), (True, None)):
                with self.subTest(role=role, finalized=finalized, joined=joined):
                    index = Index()
                    value = index.record(self.spec)
                    value["raw"][role].update(finalized=finalized, joined=joined)
                    owner = value["processes"][role]
                    index.payloads[owner["outputs"][1]["log_file"]] = deepcopy(value["raw"][role])
                    with self.assertRaisesRegex(ValueError, "native canonical readiness"):
                        self.check(value, index)

    def test_status_echo_cannot_replace_missing_final_indexed_output(self):
        index = Index()
        value = index.record(self.spec)
        del index.payloads[value["processes"]["sink"]["outputs"][1]["log_file"]]
        with self.assertRaises(OSError):
            self.check(value, index)

    def test_exact_source_binary_argv_attempt_and_budget_fail_closed(self):
        mutations = (
            lambda v: v.update(status="passed"),
            lambda v: v["raw"]["victim"].update(joined=False),
            lambda v: v["processes"]["victim"]["command"].__setitem__(0, "/unit/other"),
            lambda v: v["processes"]["victim"]["command"].__setitem__(1, "pubsub-manual"),
            lambda v: v["processes"]["victim"]["stop_budget"].update(seconds=60),
            lambda v: v["attempts"][0].update(timeout_seconds=600),
            lambda v: v["attempts"][0].update(exit_code=True),
            lambda v: v["processes"]["victim"].update(forced_termination=True),
        )
        for mutation in mutations:
            index = Index()
            value = index.record(self.spec)
            mutation(value)
            with self.subTest(mutation=mutation), self.assertRaises(ValueError):
                self.check(value, index)

    def test_planned_manifest_cannot_promote(self):
        from check_stage6_acceptance import required_scenarios

        manifest = json.loads(Path(__file__).with_name("p2p_donor_capabilities.json").read_text())
        entries = manifest["interop_acceptance_registry"]["capabilities"][acceptance.OWNER_ID]["scenarios"]
        for entry in entries:
            entry["registration"] = "registered"
        required, errors = required_scenarios(manifest, "pubsub-scoring")
        self.assertEqual(errors, [])
        self.assertEqual(len(required), 6)
        entries[0]["registration"] = "planned"
        self.assertTrue(required_scenarios(manifest, "pubsub-scoring")[1])

    def test_missing_full_matrix_or_loader_is_not_evidence(self):
        self.assertTrue(acceptance.validate_suite([], {}, Path("/unit"), {}, None))

    def test_inventory_requires_the_full_native_dispatch_and_private_inputs(self):
        from check_p2p_feature_inventory import registered_pubsub_pairs
        from pubsub_cases import case_specs

        source = Path(__file__).with_name("runner.py").read_text()
        self.assertEqual(registered_pubsub_pairs(ast.parse(source)),
                         {(spec.runner_id, spec.scenario) for spec in case_specs()})
        for altered in (source.replace("key=pnet_key_file,\n                        fingerprint=pnet_fingerprint", "key=None,\n                        fingerprint=pnet_fingerprint"),
                        source.replace("for spec in pubsub_specs():", "for spec in pubsub_specs()[:1]:"),
                        source.replace("artifact = run_pubsub_case(spec, binaries, root,", "artifact = run_pubsub_case(spec, binaries, other_root,")):
            with self.subTest(altered=altered != source), self.assertRaises(ValueError):
                registered_pubsub_pairs(ast.parse(altered))

    def test_inventory_rejects_drift_in_profile_owner_and_directions(self):
        from check_p2p_feature_inventory import pubsub_registration_source_errors

        directory = Path(__file__).resolve().parent
        manifest = json.loads((directory / "p2p_donor_capabilities.json").read_text())
        capability = next(row for row in manifest["capabilities"] if row["id"] == acceptance.OWNER_ID)
        entries = manifest["interop_acceptance_registry"]["capabilities"][acceptance.OWNER_ID]["scenarios"]
        for entry in entries:
            self.assertEqual(pubsub_registration_source_errors(directory.parent.parent, acceptance.OWNER_ID, capability, entry), [])
            for field, value in (("required_directions", ["forge_to_go"] * 4), ("transport_stack", []),
                                 ("source_case_id", "generic_publish"), ("registration", "planned")):
                self.assertTrue(pubsub_registration_source_errors(directory.parent.parent, acceptance.OWNER_ID,
                                                                  capability, {**entry, field: value}))


if __name__ == "__main__":
    unittest.main()
