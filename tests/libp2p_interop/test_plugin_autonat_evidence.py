"""Synthetic configured receipts only; no donors, processes or network commands run."""

from copy import deepcopy
from dataclasses import asdict
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import autonat_cases as native_cases
from autonat_evidence import validate_autonat_evidence
from autonat_network import IsolatedAutonatNetwork
import plugin_autonat_cases as cases
from plugin_autonat_evidence import configured_pair, validate_case, validate_terminal_sources
import process_lifecycle
import runner
from test_autonat_cases import Harness
from test_autonat_evidence import valid_pair
from test_autonat_network import FakeCommandRunner


def configure_result(result, backend, store, version, service):
    """Adapt synthetic raw schema to the actual official C++ receipt, not live proof."""
    client = not service
    actual = result["requested_addr"] if client else "/ip4/11.0.0.2/tcp/42000"
    reserved = client and version == 2
    listen = actual if reserved else actual.rsplit("/", 1)[0] + "/0"
    policy = {f"{role}-v{v}-enabled": v == version and (service if role == "service" else client)
              for role in ("client", "service") for v in (1, 2)}
    policy["ping-enabled"] = False
    node = {"peer-store": {"store": "peers"}, "allow-insecure-test-mode": False,
        "identity": {"certificate-secret": "identity/certificate", "private-key-secret": "identity/private-key"},
        "listen": [listen], "topology": {"mode": "static-only"}, "tcp": {"security": "noise"},
        "path": {"policy": "direct-only"}, "relay": {
            "client-enabled": False, "server-enabled": False, "auto-discovery-enabled": False},
        "pubsub": {"enabled": False}, "mdns": {"enabled": False}, "peer-exchange": {"enabled": False},
        "reachability": policy}
    if reserved:
        node["advertised-endpoints"] = [actual]
    settings = {"source": "official-shell.accepted-document", "identity_source": "Secrets.get_bytes",
        "identity_key_type": "ed25519", "psk_source": "absent", "secret_material": "redacted", "backend_fallback": False,
        "document": {"plugins": {"db": {"store": {"stores": [{"name": "peers", "driver": backend,
            "path": str(store), "object": {"family": "objectdb", "write-policy": "single-writer"}}]}},
            "net": {"p2p": {"node": node}}}}}
    host = {"generation": 1, "resync_required": False, "phase": "maintenance", "effective": "unknown",
            "autonat_v1": "unknown", "autonat_v2": []}
    roles = {key.replace("-", "_"): enabled for key, enabled in policy.items() if key.endswith("enabled") and key != "ping-enabled"}
    initial = {"source": "official-node.diagnostics_api.snapshot", "lifecycle_phase": "maintenance", "active_sessions": 0,
        "resources": dict.fromkeys(("system_memory", "inbound_connections", "outbound_connections", "active_dials", "active_service_scopes"), 0),
        "reachability": {"host": deepcopy(host), "internet_egress_allowed": True, **roles,
            "pending_probes": 0, "waiting_probes": 0, "active_handlers": 0, "probe_errors": 0},
        "metrics": dict.fromkeys(("reachability_checks", "reachability_public", "reachability_private"), 0)}
    before = deepcopy(initial)
    before["active_sessions"] = 1
    before["resources"]["outbound_connections" if client else "inbound_connections"] = 1
    before["reachability"]["host"]["generation"] = 2
    if client:
        before["metrics"].update(reachability_checks=1, reachability_public=1)
    if client and version == 2:
        before["reachability"]["host"]["autonat_v2"] = [{"address": actual, "state": "public"}]
    result.pop("diagnostics_after_stop", None)
    result.update(fixture_kind="configured-plugin", suite="plugin-node", store_backend=backend,
        configuration_accepted=True, accepted_configuration=settings, final=True, complete=True, errors=[],
        lifecycle={"plugins_joined": True, "scheduler_joined": True}, autonat_probe_api_calls=0,
        v1_vote=None, probe_state="public" if client and version == 2 else "unknown",
        v2_fixture_candidate_count=1 if reserved else 0, diagnostics_before_connect=initial,
        diagnostics_before_stop=before, protocol_basis="configured_selected_protocol_not_negotiation_capture",
        operation_basis="official_shell_startup_authenticated_connect_only", operation_source="official-shell.startup-and-node.api.connect",
        observation_scope="after-official-startup-before-connect-through-pre-stop; no-native-post-stop-snapshot",
        diagnostics_after_detach={"phase": "autonat.post-join.diagnostics-detached", "error_category": "forge.plugins.net.p2p.node",
            "error_code": 1, "secret_material": "redacted", "source": "retained-official-node.diagnostics_api.snapshot",
            "rejected": True, "after_plugins_joined": True, "after_scheduler_joined": True, "native_snapshot_read": False},
        host_events={"source": "official-node.events_api.host_events", "initial_consumed": True, "initial_generation": 1,
            "initial": deepcopy(host), "before_stop": deepcopy(before["reachability"]["host"]), "active_after_join": False,
            "queued_events_drained_after_join": [], "read_after_join": None, "after_plugins_joined": True, "after_scheduler_joined": True},
        probe_port_reservation={"source": "boost.asio.tcp.acceptor.local_endpoint", "requested_address": "/ip4/11.0.0.3/tcp/0",
            "reserved_address": actual, "released_before_startup": True, "node_listener_proof": False} if reserved else None)
    if client:
        control = next(connection for connection in result["actual_connections"] if connection["direction"] == "outbound")
        result["control_session"] = {"source": "official-node.api.connect-and-node.diagnostics_api.session", "session_id": 1,
            "local_peer_id": result["local_peer_id"], "remote_peer_id": control["authenticated_peer"], "path": "direct",
            "direction": "outbound", "authentication": "noise", "authentication_basis": "native_session_upgrade_output",
            "muxer": "/yamux/1.0.0", "identify_state": "identified", "identified": True, "closed": False,
            "local_address": control["local_addr"], "remote_address": control["remote_addr"]}


def synthetic_record(spec, root, binaries, index=0, *, mutate_harness=None):
    """Use the real native owner/serializer with fake Popen and network replies."""
    native_root = cases.case_root(root, spec)
    store = native_root / spec.native.identifier / (spec.forge_role + "-store")
    harness = Harness(spec.native)
    configure_result(harness.results[spec.forge_role], spec.backend, store, spec.version, spec.forge_role == "server")
    if spec.forge_role == "server":
        harness.ready.update(fixture_kind="configured-plugin", configuration_accepted=True, store_backend=spec.backend,
                             accepted_configuration=deepcopy(harness.results["server"]["accepted_configuration"]))
    if mutate_harness:
        mutate_harness(harness)
    fake = FakeCommandRunner()
    network = IsolatedAutonatNetwork(spec.version == 2,
        command_runner=lambda command: fake(["ip" if argument == "/unit/ip" else argument for argument in command]),
        system=lambda: "Linux", ip_lookup=lambda name: "/unit/" + name,
        outer_namespace_isolated=lambda: True, namespace_token=f"configured{index}")
    fake.network = network
    fresh = []

    def arguments(role, owned_store):
        if role != spec.forge_role or owned_store != store or owned_store.exists() or fresh:
            raise ValueError("synthetic fresh-store witness violated")
        fresh.append({"source": "python.fixture.pre-spawn-store-check", "actor": role,
                      "path": str(store), "existed_before_spawn": False})
        return ("--store-backend", spec.backend)

    def popen(command, stdout, stderr):
        if "--store-backend" in command:
            Path(command[command.index("--store-dir") + 1]).mkdir()
        return harness.popen(command, stdout, stderr)

    with patch.object(native_cases, "IsolatedAutonatNetwork", return_value=network), \
            patch.object(process_lifecycle.subprocess, "Popen", side_effect=popen):
        native = native_cases.run_case(spec.native, binaries, native_root, pnet_key=None, pnet_fingerprint="",
            wait_json=harness.wait_ready, command_attempt=runner.command_attempt,
            forge_cli_arguments=arguments, semantic_checker=configured_pair)
    native["acceptance_scenario_ids"] = []
    return {"suite": "plugin-autonat", "scenario_id": spec.scenario_id, "spec": asdict(spec), "status": "observed",
        "errors": [], "preflight": {"source": "python.platform.system", "required_platform": "Linux",
            "observed_platform": "Linux", "status": "eligible", "reason": None},
        "native": native, "forge_store": fresh[0] if fresh else None}


def recapture(record):
    """Rewrite only synthetic original/captured files after adversarial mutations."""
    native = record["native"]
    for owner, role in zip(native["owned_processes"], ("server", "client")):
        for output in owner["outputs"]:
            value = owner["ready"] if output["argument"] == "--ready-file" else native[role]
            for key in ("path", "log_file"):
                Path(output[key]).write_text(json.dumps(value))


class ConfiguredEvidenceTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.binaries = {implementation: self.root / implementation for implementation in ("forge", "go", "rust")}
        for path in self.binaries.values():
            path.write_text("synthetic non-executable fixture\n")
        self.records = [(spec, synthetic_record(spec, self.root, self.binaries, index)) for index, spec in enumerate(
            spec for backend in cases.BACKENDS for spec in cases.case_specs(backend))]

    def check(self, spec, record):
        return validate_case(record, spec) + validate_terminal_sources(record, self.binaries, self.root)

    def test_all_eight_use_real_validators_and_no_native_promotion(self):
        for spec, record in self.records:
            with self.subTest(case=spec.scenario_id):
                self.assertEqual(self.check(spec, record), [])
                self.assertEqual(record["native"]["acceptance_scenario_ids"], [])
                self.assertEqual(record["native"][spec.forge_role]["autonat_probe_api_calls"], 0)

    def test_both_snapshots_require_typed_bounded_waiters_and_metrics(self):
        fields = {"reachability": ("waiting_probes",),
                  "metrics": ("reachability_checks", "reachability_public", "reachability_private")}
        for spec, baseline in self.records:
            self.assertEqual(self.check(spec, baseline), [])
            for snapshot in ("diagnostics_before_connect", "diagnostics_before_stop"):
                for value in (None, [], "metrics", 1, True):
                    with self.subTest(case=spec.scenario_id, snapshot=snapshot, metrics=value):
                        record = deepcopy(baseline)
                        record["native"][spec.forge_role][snapshot]["metrics"] = value
                        self.assertTrue(validate_case(record, spec))
                record = deepcopy(baseline)
                del record["native"][spec.forge_role][snapshot]["metrics"]
                self.assertTrue(validate_case(record, spec))
                for group, keys in fields.items():
                    for key in keys:
                        with self.subTest(case=spec.scenario_id, snapshot=snapshot, missing=key):
                            record = deepcopy(baseline)
                            del record["native"][spec.forge_role][snapshot][group][key]
                            self.assertTrue(validate_case(record, spec))
                        for value in (None, False, True, -1, 1.5, "0", [], {}, 2**64):
                            with self.subTest(case=spec.scenario_id, snapshot=snapshot, key=key, value=value):
                                record = deepcopy(baseline)
                                record["native"][spec.forge_role][snapshot][group][key] = value
                                self.assertTrue(validate_case(record, spec))

    def test_initial_snapshot_requires_idle_probes_and_zero_metrics(self):
        fields = {"reachability": ("pending_probes", "waiting_probes", "active_handlers", "probe_errors"),
                  "metrics": ("reachability_checks", "reachability_public", "reachability_private")}
        for spec, baseline in self.records:
            self.assertEqual(self.check(spec, baseline), [])
            for group, keys in fields.items():
                for key in keys:
                    with self.subTest(case=spec.scenario_id, initial_nonzero=key):
                        record = deepcopy(baseline)
                        forge = record["native"][spec.forge_role]
                        forge["diagnostics_before_connect"][group][key] = 1
                        forge["diagnostics_before_stop"][group][key] = max(
                            1, forge["diagnostics_before_stop"][group][key])
                        self.assertTrue(validate_case(record, spec))

    def test_cumulative_metrics_cannot_go_backwards_in_either_role(self):
        for spec, baseline in self.records:
            self.assertEqual(self.check(spec, baseline), [])
            for key in ("reachability_checks", "reachability_public", "reachability_private"):
                with self.subTest(case=spec.scenario_id, backwards=key):
                    record = deepcopy(baseline)
                    forge = record["native"][spec.forge_role]
                    forge["diagnostics_before_connect"]["metrics"][key] = 2
                    forge["diagnostics_before_stop"]["metrics"][key] = 1
                    errors = validate_case(record, spec)
                    self.assertTrue(errors)
                    self.assertIn("metric went backwards", "; ".join(errors))

    def test_client_requires_completed_public_response_and_no_pending_waiters(self):
        for spec, baseline in self.records:
            if spec.forge_role != "client":
                continue
            self.assertEqual(self.check(spec, baseline), [])
            for key in ("reachability_checks", "reachability_public"):
                with self.subTest(case=spec.scenario_id, no_increase=key):
                    record = deepcopy(baseline)
                    record["native"]["client"]["diagnostics_before_stop"]["metrics"][key] = 0
                    self.assertTrue(validate_case(record, spec))
            for group, key in (("reachability", "pending_probes"), ("reachability", "waiting_probes"),
                               ("reachability", "probe_errors"), ("metrics", "reachability_private")):
                with self.subTest(case=spec.scenario_id, incomplete_or_failed=key):
                    record = deepcopy(baseline)
                    record["native"]["client"]["diagnostics_before_stop"][group][key] = 1
                    self.assertTrue(validate_case(record, spec))
            record = deepcopy(baseline)
            record["native"]["client"]["diagnostics_before_stop"]["metrics"].update(
                reachability_checks=3, reachability_public=3)
            self.assertEqual(validate_case(record, spec), [])

    def test_service_receipt_does_not_claim_unconfigured_client_metrics(self):
        for spec, baseline in self.records:
            if spec.forge_role != "server":
                continue
            self.assertEqual(self.check(spec, baseline), [])
            metrics = baseline["native"]["server"]["diagnostics_before_stop"]["metrics"]
            self.assertEqual(set(metrics.values()), {0})
            for key in metrics:
                with self.subTest(case=spec.scenario_id, invented_client_metric=key):
                    record = deepcopy(baseline)
                    record["native"]["server"]["diagnostics_before_stop"]["metrics"][key] = 1
                    self.assertTrue(validate_case(record, spec))

    def test_configured_receipt_cannot_self_select_boundary_in_default_raw_checker(self):
        for spec, record in self.records:
            native = record["native"]
            arguments = dict(client=native["client"], server=native["server"], version=spec.version, transport="tcp",
                expected_service_peer=native["expected_service_peer"], expected_requested_address=native["expected_requested_address"],
                client_terminal_status={"exit_code": 0, "termination": "graceful"},
                server_terminal_status={"exit_code": 0, "termination": "graceful"})
            with self.subTest(case=spec.scenario_id):
                self.assertEqual(configured_pair(**arguments), [])
                self.assertTrue(validate_autonat_evidence(**arguments))
                arguments["client"]["forge_boundary"] = "configured-plugin"
                arguments["server"]["forge_boundary"] = "configured-plugin"
                self.assertTrue(validate_autonat_evidence(**arguments))

    def test_go_v1_raw_fallback_cannot_cover_configured_missing_inbound(self):
        pair = valid_pair("forge", "go", 1)
        configure_result(pair["client"], "mdbx", self.root / "only-synthetic-store", 1, False)
        self.assertEqual(configured_pair(**pair), [])
        self.assertTrue(pair["server"]["probe_connections"])
        pair["client"]["actual_connections"] = [connection for connection in pair["client"]["actual_connections"]
                                                if connection["direction"] != "inbound"]
        self.assertTrue(configured_pair(**pair))

    def test_malformed_raw_after_stop_is_not_absence_at_configured_boundary(self):
        for spec, baseline in self.records:
            self.assertEqual(self.check(spec, baseline), [])
            for value in ("not-json", "[]", "42", [], 42, True):
                with self.subTest(case=spec.scenario_id, value=value):
                    record = deepcopy(baseline)
                    record["native"][spec.forge_role]["diagnostics_after_stop"] = value
                    self.assertTrue(validate_case(record, spec))

    def test_manual_probe_roles_credentials_and_quorum_fail_without_weakening_donor_checks(self):
        mutations = (
            lambda r: r.update(autonat_probe_api_calls=1),
            lambda r: r.update(autonat_probe_api_calls=False),
            lambda r: r.update(configuration_accepted=False),
            lambda r: r.update(complete=False),
            lambda r: r["accepted_configuration"].update(identity_source="raw-node-key"),
            lambda r: r["accepted_configuration"].update(backend_fallback=True),
            lambda r: r["accepted_configuration"]["document"]["plugins"].update(crypto={"secrets": "secret-source"}),
            lambda r: r["accepted_configuration"]["document"]["plugins"]["net"]["p2p"]["node"].update(**{"allow-insecure-test-mode": True}),
            lambda r: r["accepted_configuration"]["document"]["plugins"]["net"]["p2p"]["node"]["reachability"].update(**{"min-reachability-observers": 1}),
            lambda r: r["accepted_configuration"]["document"]["plugins"]["db"]["store"]["stores"][0].update(driver="memory"),
            lambda r: r["lifecycle"].update(scheduler_joined=False),
            lambda r: r["diagnostics_after_detach"].update(native_snapshot_read=True),
            lambda r: r["diagnostics_after_detach"].update(error_code=True),
            lambda r: r["host_events"].update(active_after_join=True),
            lambda r: r["host_events"].update(read_after_join={"phase": "stopped"}),
        )
        for spec, baseline in self.records:
            self.assertEqual(self.check(spec, baseline), [])
            for mutate in mutations:
                with self.subTest(case=spec.scenario_id, mutation=mutate):
                    record = deepcopy(baseline)
                    mutate(record["native"][spec.forge_role])
                    self.assertTrue(validate_case(record, spec))
            record = deepcopy(baseline)
            policy = record["native"][spec.forge_role]["accepted_configuration"]["document"]["plugins"]["net"]["p2p"]["node"]["reachability"]
            policy[f"{'service' if spec.forge_role == 'client' else 'client'}-v{spec.version}-enabled"] = True
            self.assertTrue(validate_case(record, spec))

    def test_fresh_authentication_and_counter_shapes_cannot_be_replaced_by_ok_labels(self):
        for spec, baseline in self.records:
            self.assertEqual(self.check(spec, baseline), [])
            for mode in ("missing dialback", "unauthenticated", "missing secured", "wrong peer", "wrong socket", "control only"):
                record = deepcopy(baseline)
                client = record["native"]["client"]
                if mode in ("missing dialback", "control only"):
                    client["actual_connections"] = [c for c in client["actual_connections"] if c["direction"] != "inbound"]
                else:
                    for connection in client["actual_connections"]:
                        if connection["direction"] != "inbound":
                            continue
                        if mode == "unauthenticated":
                            connection["authenticated"] = False
                        elif mode == "wrong peer":
                            connection["authenticated_peer"] = "unpaired-peer"
                        elif mode == "wrong socket":
                            connection["local_addr"] = "/ip4/11.0.0.1/tcp/9"
                    if mode == "missing secured":
                        forge = record["native"][spec.forge_role]
                        forge["actual_connections"] = [c for c in forge["actual_connections"] if c["stage"] != "secured"]
                with self.subTest(case=spec.scenario_id, mode=mode):
                    self.assertTrue(validate_case(record, spec))
            for value in (False, -1, 1.5, None, 2**64):
                record = deepcopy(baseline)
                record["native"][spec.forge_role]["diagnostics_before_stop"]["reachability"]["probe_errors"] = value
                self.assertTrue(validate_case(record, spec))
            record = deepcopy(baseline)
            record["native"][spec.forge_role]["diagnostics_before_stop"]["reachability"]["successful_probes"] = 1
            self.assertTrue(validate_case(record, spec))

    def test_v1_unknown_and_v2_actual_address_state_are_not_interchangeable(self):
        for spec, baseline in self.records:
            if spec.forge_role != "client":
                continue
            self.assertEqual(self.check(spec, baseline), [])
            for field, value in (("v1_vote", "public"), ("autonat_v1_state", "public")):
                record = deepcopy(baseline)
                record["native"]["client"][field] = value
                self.assertTrue(validate_case(record, spec))
            if spec.version == 2:
                record = deepcopy(baseline)
                record["native"]["client"]["diagnostics_before_stop"]["reachability"]["host"]["autonat_v2"] = []
                self.assertTrue(validate_case(record, spec))

    def test_terminal_sources_bind_backend_store_original_capture_and_network(self):
        for spec, baseline in self.records:
            self.assertEqual(self.check(spec, baseline), [])
            for field, value in (("actor", "other"), ("existed_before_spawn", True), ("path", str(self.root))):
                record = deepcopy(baseline)
                record["forge_store"][field] = value
                self.assertTrue(validate_terminal_sources(record, self.binaries, self.root))
            record = deepcopy(baseline)
            for process in (*record["native"]["owned_processes"], *record["native"]["attempts"]):
                if "--store-backend" in process["command"]:
                    process["command"][process["command"].index("--store-backend") + 1] = "memory"
            self.assertTrue(validate_terminal_sources(record, self.binaries, self.root))
            output = baseline["native"]["owned_processes"][0]["outputs"][0]
            path = Path(output["path"])
            original = path.read_text()
            try:
                path.write_text('{"status":"ok"}')
                self.assertTrue(validate_terminal_sources(baseline, self.binaries, self.root))
            finally:
                path.write_text(original)
            record = deepcopy(baseline)
            record["native"]["network"]["commands"][0]["returncode"] = 1
            self.assertTrue(validate_terminal_sources(record, self.binaries, self.root))


if __name__ == "__main__":
    unittest.main()
