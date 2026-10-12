"""Synthetic configured receipts over raw AutoRelay parser fixtures, never live proof."""

from copy import deepcopy
from dataclasses import asdict
import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from autorelay_acceptance import validate_record as validate_native_record
from autorelay_evidence import validate_case as validate_native_case
from plugin_relay_cases import BACKENDS, case_root, case_specs
from plugin_relay_evidence import configured_native_case, validate_case, validate_terminal_sources
from test_autorelay_acceptance import owned_case
from test_autorelay_evidence import NOW, public_peer


def recapture(record):
    for actor in [*record["native"]["raw"].values(), *record["native"]["echoes"]]:
        owner = actor["process"]
        owner["ready"] = deepcopy(actor.get("ready", {}))
        for output in owner["outputs"]:
            key = "ready" if output["argument"] == "--ready-file" else "result"
            text = json.dumps(actor[key])
            Path(output["path"]).write_text(text)
            Path(output["log_file"]).write_text(text)


def forge_lifecycle_receipts(native):
    """Synthetic native session facts; actual production captures are not forged here."""
    ready = native["raw"]["destination"]["ready"]
    result = native["raw"]["destination"]["result"]
    target = ready["peer_id"]
    local = ready["listen_addrs"][0].split("/p2p/")[0]
    result.update(local_peer_id=target, seed_connections=[], echoes=[],
                  echo_admission_closed=True, pending_echoes=0, admitted_echoes=3)
    seeds = {}
    for role, session_id, elapsed in (("relay", 2, 1000), ("replacement", 5, 12000)):
        donor = native["raw"][role]["ready"]
        relay, address = donor["peer_id"], donor["listen_addrs"][0]
        session = {"source": "official-node.diagnostics_api.session", "session_id": session_id,
                   "local_peer_id": target, "remote_peer_id": relay, "path": "direct", "direction": "outbound",
                   "authentication": "noise", "authentication_basis": "native_session_upgrade_output",
                   "muxer": "/yamux/1.0.0", "identify_state": "identified", "identified": True, "closed": False,
                   "local_address": local, "remote_address": address.split("/p2p/")[0]}
        seed = {"source": "official-node.api.connect", "dial_address": address, "expected_peer_id": relay,
                "session": session, "unix_ms": NOW + elapsed}
        result["seed_connections"].append(seed)
        seeds[relay] = seed
        for row in result["observations"]:
            for observed in row.get("sessions", []):
                if observed["peer_id"] == relay:
                    observed["connection_id"] = session_id
    for index, native_echo in enumerate(native["echoes"]):
        donor = native_echo["result"]
        donor["local_peer_id"] = public_peer(6 + index)
        peer, relay = donor["local_peer_id"], donor["relay_peer"]
        carrier = seeds[relay]["session"]
        session = {"source": "official-node.protocol-handler-and-diagnostics_api", "session_id": (3, 4, 6)[index],
                   "local_peer_id": target, "remote_peer_id": peer, "authentication": "noise",
                   "stream_authentication": "noise", "authentication_basis": "native_inner_session_upgrade_output",
                   "muxer": "/yamux/1.0.0", "identify_state": "identified", "identified": True, "closed": False,
                   "path": "relay", "direction": "inbound", "relay_peer_id": relay,
                   "circuit_address": donor["relayed_addr"].split("/p2p-circuit")[0] + "/p2p-circuit/p2p/" + peer,
                   "carrier_session_id": carrier["session_id"], "carrier": deepcopy(carrier)}
        payload = b"x" * donor["echo_bytes"]
        receipt = {"bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(), "contains_zero": False}
        result["echoes"].append({"source": "official-node.protocol-handler", "protocol": donor["protocol"],
            "session": session, "request": receipt, "response": deepcopy(receipt),
            "response_write_completed": True, "stream_closed": True, "unix_ms": donor["unix_ms"] + 1})


def synthetic_record(spec, root, binaries, index=0):
    native = owned_case(spec.native, case_root(root, spec), binaries, index)
    native["acceptance_scenario_ids"] = []
    actor = native["raw"][spec.forge_role]
    store = case_root(root, spec) / spec.native.identifier / (spec.forge_role + "-store")
    store.mkdir()
    actor["process"]["command"] += ["--store-backend", spec.backend]
    flags = {"fixture_kind": "configured-plugin", "store_backend": spec.backend, "configuration_accepted": True}
    actor["ready"].update(flags)
    client = spec.kind == "lifecycle"
    relay = {"client-enabled": client, "server-enabled": not client, "auto-discovery-enabled": client}
    if client:
        relay.update({"target-reservations": 1, "max-candidates": 4, "max-parallel-reservations": 1,
                      "refresh-margin-ms": 3000, "candidate-backoff-ms": 1000})
    else:
        relay["reservation-ttl-ms"] = 8000
    node = {"allow-insecure-test-mode": False, "peer-store": {"store": "peers"},
            "identity": {"certificate-secret": "identity/certificate", "private-key-secret": "identity/private-key"},
            "listen": ["/ip4/127.0.0.1/tcp/0"], "tcp": {"security": "noise"},
            "topology": {"mode": "static-only"}, "path": {"policy": "relay-only" if client else "direct-only"}, "relay": relay,
            "reachability": {key: False for key in ("client-v1-enabled", "client-v2-enabled",
                                                     "service-v1-enabled", "service-v2-enabled")}}
    settings = {"source": "official-shell.accepted-document", "secret_material": "redacted",
                "identity_source": "Secrets.get_bytes", "identity_key_type": "ed25519", "psk_source": "absent",
                "backend_fallback": False, "document": {"plugins": {
                    "db": {"store": {"stores": [{"name": "peers", "driver": spec.backend, "path": str(store),
                                                  "object": {"family": "objectdb", "write-policy": "single-writer"}}]}},
                    "net": {"p2p": {"node": node}}}}}
    result = actor["result"]
    result.update(flags, suite="plugin-node", final=True, accepted_configuration=settings,
                  status="ok", errors=[], lifecycle={"plugins_joined": True, "scheduler_joined": True},
                  operation_basis="official_shell_startup_authenticated_connect_only",
                  operation_source="official-shell.startup-and-node.api.connect",
                  observation_scope="after-official-startup-before-seeds-through-pre-stop; no-native-post-stop-snapshot",
                  diagnostics_after_detach={"phase": "autorelay.post-join.diagnostics-detached",
                      "error_category": "forge.plugins.net.p2p.node", "error_code": 1, "secret_material": "redacted",
                      "source": "retained-official-node.diagnostics_api.snapshot", "rejected": True,
                      "after_plugins_joined": True, "after_scheduler_joined": True, "native_snapshot_read": False},
                  host_events={"source": "official-node.events_api.host_events", "initial_consumed": True,
                      "initial_generation": 0, "active_after_join": False, "read_after_join": None,
                      "after_plugins_joined": True, "after_scheduler_joined": True})
    if client:
        rows = result["observations"]
        rows[0]["phase"] = "before_seed"
        rows[0]["autorelay"]["running"] = True
        rows[-2]["phase"], rows[-2]["stopped"] = "pre_stop", False
        rows[-2]["autorelay"].update(running=True, stopping=False)
        result["observations"] = rows[:-1]
        result["reservations_basis"] = "diagnostics.snapshot.relay_reservations"
        forge_lifecycle_receipts(native)
    else:
        result["observations"] = [
            {"phase": "before_seed", "elapsed_ms": 0, "unix_ms": NOW, "stopped": False,
             "relay_bytes": 0, "service_reservations": 0},
            {"phase": "running", "elapsed_ms": 2500, "unix_ms": NOW + 2500, "stopped": False,
             "relay_bytes": 10, "service_reservations": 1},
            {"phase": "pre_stop", "elapsed_ms": 3000, "unix_ms": NOW + 3000, "stopped": False,
             "relay_bytes": 10, "service_reservations": 1},
        ]
    record = {"suite": "plugin-relay", "scenario_id": spec.scenario_id, "spec": asdict(spec), "native": native,
              "forge_store": {"source": "python.fixture.pre-spawn-store-check", "actor": spec.forge_role,
                              "path": str(store), "existed_before_spawn": False}, "errors": [], "status": "observed"}
    recapture(record)
    return record


class PluginRelayEvidenceTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name).resolve()
        self.binaries = {impl: self.root / impl for impl in ("forge", "go", "rust")}
        for path in self.binaries.values():
            path.write_text("synthetic executable, never run\n")
        self.specs = tuple(spec for backend in BACKENDS for spec in case_specs(backend))
        self.records = [synthetic_record(spec, self.root, self.binaries, index)
                        for index, spec in enumerate(self.specs)]

    def positive(self, record, spec):
        self.assertEqual(validate_case(record, spec), [])
        self.assertEqual(validate_terminal_sources(record, self.binaries, self.root), [])

    def test_all_eight_source_bound_lanes_and_read_only_projection(self):
        for spec, record in zip(self.specs, self.records):
            with self.subTest(case=spec.scenario_id):
                original = deepcopy(record)
                self.positive(record, spec)
                self.assertEqual(record, original)
                self.positive(json.loads(json.dumps(record)), spec)

    def test_raw_default_never_accepts_configured_boundary_or_artifact_mode_claim(self):
        for spec, record in zip(self.specs, self.records):
            with self.subTest(case=spec.scenario_id):
                self.positive(record, spec)
                native = record["native"]
                self.assertTrue(validate_native_case(native))
                native["boundary"] = "configured-plugin"
                self.assertTrue(validate_native_case(native))
                native.pop("boundary")
                view = deepcopy(native)
                owner = view["raw"][spec.forge_role]["process"]
                for process in [owner, *view["processes"], *view["attempts"]]:
                    if process.get("pid") == owner["pid"] and "--store-backend" in process["command"]:
                        index = process["command"].index("--store-backend")
                        process["command"] = process["command"][:index] + process["command"][index + 2:]
                root = case_root(self.root, spec)
                load = lambda path: json.loads(Path(path).read_text())
                self.assertTrue(validate_native_record(view, spec.native, root, self.binaries, load))
                self.assertEqual(validate_native_record(view, spec.native, root, self.binaries, load,
                                                       semantic_checker=configured_native_case), [])

    def test_official_configuration_and_owners_fail_closed(self):
        for spec, baseline in zip(self.specs, self.records):
            role = spec.forge_role
            mutations = {
                "raw fixture": lambda r: r["native"]["raw"][role]["result"].update(fixture_kind="native"),
                "missing join": lambda r: r["native"]["raw"][role]["result"]["lifecycle"].update(scheduler_joined=False),
                "backend": lambda r: r["native"]["raw"][role]["ready"].update(store_backend="memory"),
                "Secrets": lambda r: r["native"]["raw"][role]["result"]["accepted_configuration"].update(identity_source="inline"),
                "promotion": lambda r: r["native"].update(acceptance_scenario_ids=["autorelay_lifecycle"]),
                "execution error": lambda r: r["native"].update(errors=["delivery failed"]),
                "cleanup error": lambda r: r["native"].update(cleanup_errors=["unjoined"]),
            }
            for name, mutation in mutations.items():
                with self.subTest(case=spec.scenario_id, mutation=name):
                    self.positive(baseline, spec)
                    changed = deepcopy(baseline)
                    mutation(changed)
                    self.assertTrue(validate_case(changed, spec))

    def test_policy_and_live_boundary_are_not_receipt_only_claims(self):
        for spec, baseline in zip(self.specs, self.records):
            self.positive(baseline, spec)
            result = lambda r: r["native"]["raw"][spec.forge_role]["result"]
            node = lambda r: result(r)["accepted_configuration"]["document"]["plugins"]["net"]["p2p"]["node"]
            mutations = {
                "insecure": lambda r: node(r).update({"allow-insecure-test-mode": True}),
                "wrong named store": lambda r: node(r)["peer-store"].update(store="missing"),
                "manual role": lambda r: node(r)["relay"].update({"server-enabled": spec.kind == "lifecycle"}),
                "cold snapshot": lambda r: result(r)["observations"][0].update(phase="before_start"),
                "fake terminal zeros": lambda r: result(r)["observations"][-1].update(phase="post_stop", stopped=True),
                "no echo": lambda r: r["native"].update(echoes=[]),
            }
            if spec.kind == "lifecycle":
                mutations.update({
                    "disabled discovery": lambda r: node(r)["relay"].update({"auto-discovery-enabled": False}),
                    "no renewal": lambda r: result(r)["observations"][2]["reservations"][0].update(expires_unix_ms=NOW + 9000),
                    "missing Push": lambda r: r["native"]["raw"]["observer"]["result"].update(events=[
                        event for event in r["native"]["raw"]["observer"]["result"]["events"] if event["kind"] != "identify_push"]),
                })
            else:
                mutations["no measured bytes"] = lambda r: [row.update(relay_bytes=0) for row in result(r)["observations"]]
            for name, mutation in mutations.items():
                with self.subTest(case=spec.scenario_id, mutation=name):
                    self.positive(baseline, spec)
                    changed = deepcopy(baseline)
                    mutation(changed)
                    self.assertTrue(validate_case(changed, spec))

    def reject_lifecycle_receipts(self, baseline, spec, mutation):
        self.positive(baseline, spec)
        changed = deepcopy(baseline)
        mutation(changed["native"]["raw"]["destination"]["result"])
        # Donor lifecycle alone must still pass: the missing proof is Forge's
        # actual admission/write/close and correlated native session receipts.
        self.assertEqual(validate_native_case(changed["native"], boundary="configured-plugin"), [])
        self.assertIn("configured Forge lifecycle receipts:", "; ".join(validate_case(changed, spec)))
        try:
            recapture(changed)
            self.assertIn("configured Forge lifecycle receipts:", "; ".join(
                validate_terminal_sources(changed, self.binaries, self.root)))
        finally:
            recapture(baseline)

    def test_both_seed_receipts_bind_actual_donor_peer_socket_and_carrier(self):
        for spec, baseline in zip(self.specs, self.records):
            if spec.kind != "lifecycle":
                continue
            replacement = baseline["native"]["raw"]["replacement"]["ready"]["peer_id"]
            seed = lambda r: r["seed_connections"][0]
            mutations = {
                "missing all seeds": lambda r: r.pop("seed_connections"),
                "missing replacement seed": lambda r: r["seed_connections"].pop(),
                "wrong connect source": lambda r: seed(r).update(source="raw-node.async_connect"),
                "wrong expected peer": lambda r: seed(r).update(expected_peer_id=replacement),
                "wrong donor address": lambda r: seed(r).update(dial_address="/ip4/127.0.0.1/tcp/1"),
                "missing native session": lambda r: seed(r).pop("session"),
                "wrong authenticated peer": lambda r: seed(r)["session"].update(remote_peer_id=replacement),
                "wrong local peer": lambda r: seed(r)["session"].update(local_peer_id=replacement),
                "wrong physical socket": lambda r: seed(r)["session"].update(remote_address="/ip4/127.0.0.1/tcp/1"),
                "unobserved carrier ID": lambda r: seed(r)["session"].update(session_id=700),
                "boolean carrier ID": lambda r: seed(r)["session"].update(session_id=True),
                "same relay/replacement carrier": lambda r: r["seed_connections"][1]["session"].update(session_id=2),
                "missing Identify": lambda r: seed(r)["session"].update(identified=False),
                "wrong security": lambda r: seed(r)["session"].update(authentication="libp2p_tls"),
                "wrong local identity": lambda r: r.update(local_peer_id=replacement),
            }
            for name, mutation in mutations.items():
                with self.subTest(case=spec.scenario_id, mutation=name):
                    self.reject_lifecycle_receipts(baseline, spec, mutation)

    def test_all_three_actual_forge_echoes_require_write_close_and_donor_carrier_binding(self):
        for spec, baseline in zip(self.specs, self.records):
            if spec.kind != "lifecycle":
                continue
            replacement = baseline["native"]["raw"]["replacement"]["ready"]["peer_id"]
            observer = baseline["native"]["raw"]["observer"]["ready"]["peer_id"]
            echo = lambda r: r["echoes"][0]
            mutations = {
                "missing all Forge echoes": lambda r: r.pop("echoes"),
                "missing replacement echo": lambda r: r["echoes"].pop(),
                "duplicate acquired echo": lambda r: r["echoes"].__setitem__(1, deepcopy(echo(r))),
                "wrong source peer": lambda r: echo(r)["session"].update(remote_peer_id=observer),
                "wrong relay peer": lambda r: echo(r)["session"].update(relay_peer_id=replacement),
                "wrong carrier ID": lambda r: echo(r)["session"].update(carrier_session_id=5),
                "boolean carrier ID": lambda r: echo(r)["session"].update(carrier_session_id=True),
                "wrong carrier peer": lambda r: echo(r)["session"]["carrier"].update(remote_peer_id=replacement),
                "wrong nested carrier ID": lambda r: echo(r)["session"]["carrier"].update(session_id=5),
                "wrong carrier socket": lambda r: echo(r)["session"]["carrier"].update(remote_address="/ip4/127.0.0.1/tcp/1"),
                "wrong incoming circuit": lambda r: echo(r)["session"].update(circuit_address="/ip4/127.0.0.1/tcp/1"),
                "inner session reused carrier": lambda r: echo(r)["session"].update(session_id=2),
                "reused inner session": lambda r: r["echoes"][1]["session"].update(session_id=3),
                "wrong authentication": lambda r: echo(r)["session"].update(stream_authentication="libp2p_tls"),
                "wrong protocol": lambda r: echo(r).update(protocol="/ipfs/ping/1.0.0"),
                "wrong handler": lambda r: echo(r).update(source="raw-node.handler"),
                "wrong byte count": lambda r: echo(r)["request"].update(bytes=11),
                "response mismatch": lambda r: echo(r)["response"].update(sha256="0" * 64),
                "unjoined write": lambda r: echo(r).update(response_write_completed=False),
                "failed close": lambda r: echo(r).update(stream_closed=False),
                "missing close": lambda r: echo(r).pop("stream_closed"),
                "admission still open": lambda r: r.update(echo_admission_closed=False),
                "pending callback": lambda r: r.update(pending_echoes=1),
                "wrong admitted count": lambda r: r.update(admitted_echoes=2),
                "receipt after pre-stop": lambda r: echo(r).update(unix_ms=r["observations"][-1]["unix_ms"] + 1),
            }
            for name, mutation in mutations.items():
                with self.subTest(case=spec.scenario_id, mutation=name):
                    self.reject_lifecycle_receipts(baseline, spec, mutation)

    def test_typed_detach_and_retained_event_subscription_are_mandatory(self):
        for spec, baseline in zip(self.specs, self.records):
            result = lambda r: r["native"]["raw"][spec.forge_role]["result"]
            mutations = {
                "no diagnostics receipt": lambda r: result(r).pop("diagnostics_after_detach"),
                "no typed rejection": lambda r: result(r)["diagnostics_after_detach"].update(rejected=False),
                "wrong typed category": lambda r: result(r)["diagnostics_after_detach"].update(error_category="forge.net.p2p"),
                "wrong typed code": lambda r: result(r)["diagnostics_after_detach"].update(error_code=3),
                "bool code": lambda r: result(r)["diagnostics_after_detach"].update(error_code=True),
                "raw escape": lambda r: result(r)["diagnostics_after_detach"].update(native_snapshot_read=True),
                "not scheduler joined": lambda r: result(r)["diagnostics_after_detach"].update(after_scheduler_joined=False),
                "no subscription": lambda r: result(r).pop("host_events"),
                "unconsumed initial": lambda r: result(r)["host_events"].update(initial_consumed=False),
                "bool generation": lambda r: result(r)["host_events"].update(initial_generation=True),
                "still active": lambda r: result(r)["host_events"].update(active_after_join=True),
                "late event": lambda r: result(r)["host_events"].update(read_after_join={"generation": 1}),
                "not plugin joined": lambda r: result(r)["host_events"].update(after_plugins_joined=False),
                "manual connect": lambda r: result(r).update(operation_source="raw-node.async_reserve_relay"),
            }
            for name, mutation in mutations.items():
                with self.subTest(case=spec.scenario_id, mutation=name):
                    self.positive(baseline, spec)
                    changed = deepcopy(baseline)
                    mutation(changed)
                    self.assertTrue(validate_case(changed, spec))
            if spec.kind == "service":
                self.assertGreater(result(baseline)["observations"][-1]["service_reservations"], 0)
            self.assertTrue(all(row["stopped"] is False for row in result(baseline)["observations"]))

    def test_terminal_sources_bind_backend_store_binary_and_every_join(self):
        for spec, baseline in zip(self.specs, self.records):
            owner = lambda r: r["native"]["raw"][spec.forge_role]["process"]
            mutations = {
                "reused store": lambda r: r["forge_store"].update(existed_before_spawn=True),
                "wrong store": lambda r: r["native"]["raw"][spec.forge_role]["result"]["accepted_configuration"]
                    ["document"]["plugins"]["db"]["store"]["stores"][0].update(path=str(self.root)),
                "wrong binary": lambda r: owner(r)["command"].__setitem__(0, str(self.binaries["go"])),
                "wrong backend argv": lambda r: owner(r)["command"].__setitem__(-1, "memory"),
                "missing owner": lambda r: r["native"]["processes"].pop(),
                "duplicate owner": lambda r: r["native"]["processes"].append(deepcopy(r["native"]["processes"][0])),
                "unjoined": lambda r: owner(r)["terminal_status"].update(termination="terminated", exit_code=-15),
                "retry": lambda r: r["native"]["attempts"].append(deepcopy(r["native"]["attempts"][0])),
                "budget": lambda r: owner(r)["stop_budget"].update(seconds=30),
                "elapsed": lambda r: r["native"].update(elapsed_seconds=121),
            }
            for name, mutation in mutations.items():
                with self.subTest(case=spec.scenario_id, mutation=name):
                    self.positive(baseline, spec)
                    changed = deepcopy(baseline)
                    mutation(changed)
                    self.assertTrue(validate_terminal_sources(changed, self.binaries, self.root))

    def test_actual_files_cannot_disagree_with_post_join_captures(self):
        for spec, baseline in zip(self.specs, self.records):
            for process in baseline["native"]["processes"]:
                for output in process["outputs"]:
                    path = Path(output["path"])
                    original = path.read_text()
                    for replacement in ('{"forged":true}', '{"same":1,"same":2}', '{"value":NaN}'):
                        with self.subTest(case=spec.scenario_id, source=output["argument"], replacement=replacement):
                            self.positive(baseline, spec)
                            try:
                                path.write_text(replacement)
                                self.assertTrue(validate_terminal_sources(baseline, self.binaries, self.root))
                            finally:
                                path.write_text(original)


if __name__ == "__main__":
    unittest.main()
