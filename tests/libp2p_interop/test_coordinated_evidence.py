"""Synthetic validator-boundary inputs only; never live socket/wire artifacts."""

from copy import deepcopy
import hashlib
import unittest

import coordinated_evidence as evidence
from coordinated_network import CoordinatedNetwork
from test_coordinated_network import TCPCommands

TOKEN = "a" * 32


def ready(implementation, role):
    ip, port = ("11.0.0.1", 40010) if role == "source" else ("11.0.0.2", 40020)
    peer = f"unit-{role}-peer"
    address = f"/ip4/{ip}/tcp/{port}"
    return {"implementation": implementation, "status": "ready", "case_token": TOKEN, "peer_id": peer,
            "listener_address": address, "listener_port": port, "listen_addrs": [f"{address}/p2p/{peer}"],
            "preexisting_connection_ids": []}


def body():
    payload = evidence.challenge(TOKEN)
    frame = bytes([len(payload)]) + payload
    return {"framed_bytes": len(frame), "framed_sha256": hashlib.sha256(frame).hexdigest(), "frames": 1,
            "complete_frames": True, "invalid_or_over_limit": False}


def go_result(role="source"):
    local, remote = ready("go", role), ready("forge", "destination" if role == "source" else "source")
    actual = "initiator" if role == "source" else "responder"
    frame = {"framed_hex": (bytes([len(evidence.challenge(TOKEN))]) + evidence.challenge(TOKEN)).hex(),
             "raw": False, "read": body()}
    connection = {"source": "go-libp2p.network.Conn.ConnState", "connection_id": "native-1",
                  "local_peer_id": local["peer_id"], "remote_peer_id": remote["peer_id"],
                  "local_address": local["listener_address"], "remote_address": remote["listener_address"],
                  "transport": "tcp", "security": "/noise", "muxer": "/yamux/1.0.0"}
    local_tuple, remote_tuple = evidence._endpoint(local["listener_address"]), evidence._endpoint(remote["listener_address"])
    return {"schema_version": 1, "implementation": "go", "local_peer_id": local["peer_id"], "case_token": TOKEN,
            "actor_role": actual, "status": "ok", "joined": True,
            "native_dial_joined": True, "operation_admitted": True, "admission_source": "coordinated_actor.preflight",
            "connections_after_stop": 0, "receipt": {"connection": connection, "native_outgoing_winner": True,
            "physical_direction": "outbound", "direction_source": "go-libp2p.swarm.Conn.Stat",
            "roles": {"security_role": actual, "yamux_role": actual},
            "role_source": "security.SecureInbound/SecureOutbound+yamux.NewConn",
            "native_dial_sockets": [{"source": "net.Dialer.DialContext.returned-socket",
                "local": f"{local_tuple[0]}:{local_tuple[1]}", "remote": f"{remote_tuple[0]}:{remote_tuple[1]}",
                "simultaneous_connect": True, "is_client": role == "source", "reason": TOKEN}],
            "application": {"protocol": evidence.ECHO_PROTOCOL, "connection_id": "native-1", "fresh_dial": False,
                            "request": deepcopy(frame), "response": deepcopy(frame)}}}


def rust_result(role="source"):
    local, remote = ready("rust", role), ready("forge", "destination" if role == "source" else "source")
    upgrade = "outbound" if role == "source" else "inbound"
    endpoint = {"direction": "outbound", "upgrade_role": upgrade, "remote_address": remote["listener_address"]}
    stream = {"stream_trace_id": 1, "direction": upgrade, "protocol": evidence.ECHO_PROTOCOL, "io_failed": False,
              "parser_error": None, "write_close_returned": True, "drop_observed": True, "read": body(), "write": body()}
    raw_connection = {"connection_trace_id": 1, "authenticated_local_peer_id": local["peer_id"],
                      "authenticated_remote_peer_id": remote["peer_id"], "local_address": local["listener_address"],
                      "remote_address": remote["listener_address"], "direction": "outbound", "endpoint": endpoint,
                      "selected_security": "/noise", "selected_muxer": "/yamux/1.0.0", "security_complete": True,
                      "muxer_complete": True, "security_delegate_completed": True, "muxer_delegate_completed": True,
                      "overflow": False, "events": [], "streams": [stream],
                      "negotiations": [{"direction": upgrade, "io_failed": False, "parser_error": None} for _ in range(2)]}
    raw_connection["transport_output_receipts"] = [{"basis": "donor_transport_output_identity", "connection_trace_id": 1,
        "after_event_sequence": 0, "dns_wrapper_enabled": False, "authenticated_remote_peer_id": remote["peer_id"],
        "request_endpoint": deepcopy(endpoint), "resolved_endpoint": deepcopy(endpoint),
        "local_address": local["listener_address"], "remote_address": remote["listener_address"]}]
    socket = {"basis": "unique_native_transport_output_and_swarm_endpoint", "socket_source": "rust.tcp.TcpStream.local_addr_peer_addr",
              "connection_trace_id": 1, "native_connection_id": "swarm-1", "local_address": local["listener_address"],
              "remote_address": remote["listener_address"], "authenticated_local_peer_id": local["peer_id"],
              "authenticated_remote_peer_id": remote["peer_id"], "connection_direction": "outbound", "security": "/noise",
              "muxer": "/yamux/1.0.0", "security_role": "client" if role == "source" else "server",
              "security_role_basis": "completed_native_security_and_muxer_delegates", "raw_upgrade_role": upgrade}
    events = [{"kind": "native_dial_admitted", "expected_peer_id": remote["peer_id"]},
              {"kind": "native_connection_established"},
              {"kind": "application_completed", "native_transport_dials_before": 1, "native_transport_dials_after": 1}]
    for index, row in enumerate(events, 1):
        row.update(sequence=index, mono_ns=index, native_connection_id="swarm-1")
    return {"schema": "forge.p2p.evidence.coordinated.v1", "implementation": "rust", "peer_id": local["peer_id"],
            "timeout_ms": 20000, "per_call_cancel_supported": False,
            "case_token": TOKEN, "coord_role": "initiator" if role == "source" else "responder",
            "status": "ok", "joined": True, "finalized": True, "error": None, "socket": socket,
            "application": {"basis": "unique_retained_connection_and_actual_new_stream_framed_io", "protocol": evidence.ECHO_PROTOCOL,
                "native_connection_id": "swarm-1", "connection_trace_id": 1, "stream_trace_id": 1, "stream_direction": upgrade,
                "write_close_returned": True, "request": body(), "response": body()},
            "native_transport_dials": [{"address": remote["listener_address"], "requested_role": "dialer" if role == "source" else "listener", "port_use": "Reuse"}],
            "events": events, "task_join": {"fixture_owned_tasks_joined": True, "overflow": False, "errors": []},
            "raw_upgrade_observations": {"overflow": False, "fixture_owned_tasks_joined": True, "complete": True,
                "connections": [raw_connection], "swarm_events": [{"swarm_connection_id": "swarm-1",
                    "authenticated_remote_peer_id": remote["peer_id"], "endpoint": endpoint}]}}


def forge_result(role="source"):
    value = go_result(role)
    value["implementation"] = "forge"
    value["receipt"]["connection"]["source"] = "forge.node.diagnostics.authenticated-session"
    value["receipt"]["connection"]["connection_id"] = 1
    value["receipt"]["direction_source"] = "forge.node.diagnostics.session.direction"
    value["receipt"]["role_source"] = "forge.node.diagnostics.session.security_role+yamux_role"
    value["receipt"]["application"].update(connection_id=1, stream_id=1,
        native_dial_attempts_before=0, native_dial_attempts_after=0,
        dial_observation_source="forge.node.metrics.sealed-gater-rejections")
    value["resources"] = {"file_descriptors": 0}
    del value["receipt"]["native_dial_sockets"]
    del value["connections_after_stop"]
    return value


def actor_errors(value, implementation, role="source", profile="native"):
    other = "destination" if role == "source" else "source"
    return evidence._errors(lambda: evidence._actor(value, implementation, TOKEN, ready(implementation, role), ready("forge", other), role, profile))


class ActorEvidenceTests(unittest.TestCase):
    def test_forge_completed_delegate_roles_and_actual_framed_io(self):
        for role in ("source", "destination"):
            self.assertEqual(actor_errors(forge_result(role), "forge", role), [])

    def test_go_actual_context_and_both_handshake_roles(self):
        for role in ("source", "destination"):
            self.assertEqual(actor_errors(go_result(role), "go", role), [])

    def test_rust_outgoing_transport_can_have_inbound_upgrade_role(self):
        for role in ("source", "destination"):
            self.assertEqual(actor_errors(rust_result(role), "rust", role), [])

    def test_rust_native_identify_does_not_replace_unique_echo(self):
        for role in ("source", "destination"):
            value = rust_result(role)
            streams = value["raw_upgrade_observations"]["connections"][0]["streams"]
            streams.extend({"stream_trace_id": index, "protocol": protocol,
                            "io_failed": False, "parser_error": None}
                           for index, protocol in enumerate(("/ipfs/id/1.0.0", "/ipfs/id/push/1.0.0"), 2))
            self.assertEqual(actor_errors(value, "rust", role), [])
            for field, bad in (("protocol", "/unknown/1"), ("io_failed", True),
                               ("parser_error", "invalid"), ("stream_trace_id", 1)):
                changed = deepcopy(value)
                changed["raw_upgrade_observations"]["connections"][0]["streams"][1][field] = bad
                self.assertTrue(actor_errors(changed, "rust", role))
            for retained in (streams[1:], [*streams, deepcopy(streams[0])]):
                changed = deepcopy(value)
                changed["raw_upgrade_observations"]["connections"][0]["streams"] = retained
                self.assertTrue(actor_errors(changed, "rust", role))
            changed = deepcopy(value)
            duplicate = deepcopy(streams[0])
            duplicate["stream_trace_id"] = 4
            changed["raw_upgrade_observations"]["connections"][0]["streams"].append(duplicate)
            self.assertTrue(actor_errors(changed, "rust", role))

    def test_go_rejects_ephemeral_socket_or_wrong_remote(self):
        for field in ("local_address", "remote_address"):
            value = go_result()
            value["receipt"]["connection"][field] = "/ip4/11.0.0.1/tcp/45000"
            self.assertTrue(actor_errors(value, "go"))

    def test_source_accepted_socket_cannot_be_outgoing_winner(self):
        for field, bad in (("native_outgoing_winner", False), ("physical_direction", "inbound")):
            value = go_result()
            value["receipt"][field] = bad
            self.assertTrue(actor_errors(value, "go"))

    def test_requested_role_is_not_observed_security_or_yamux_role(self):
        for field in ("security_role", "yamux_role"):
            value = go_result()
            value["receipt"]["roles"][field] = "responder"
            self.assertTrue(actor_errors(value, "go"))
        value = go_result()
        value["receipt"]["role_source"] = "coordinator_requested_role"
        self.assertTrue(actor_errors(value, "go"))

    def test_go_native_dial_context_must_be_simultaneous_and_identity_bound(self):
        for field, bad in (("simultaneous_connect", False), ("is_client", False), ("reason", "b" * 32), ("source", "configured_socket")):
            value = go_result()
            value["receipt"]["native_dial_sockets"][0][field] = bad
            self.assertTrue(actor_errors(value, "go"))

    def test_probe_cannot_redial_or_replace_connection(self):
        for field, bad in (("fresh_dial", True), ("connection_id", "native-2")):
            value = go_result()
            value["receipt"]["application"][field] = bad
            self.assertTrue(actor_errors(value, "go"))

    def test_forge_no_redial_proof_uses_native_gater_not_path_selection_metrics(self):
        for key, bad in (("native_dial_attempts_before", 1), ("native_dial_attempts_after", 1),
                         ("dial_observation_source", "forge.node.metrics.path_direct_attempts")):
            value = forge_result()
            value["receipt"]["application"][key] = bad
            self.assertTrue(actor_errors(value, "forge"))

    def test_wrong_challenge_or_manufactured_frame_summary_fails(self):
        for field, bad in (("framed_bytes", 0), ("frames", 0), ("complete_frames", False), ("framed_sha256", "0" * 64)):
            value = go_result()
            value["receipt"]["application"]["request"]["read"][field] = bad
            self.assertTrue(actor_errors(value, "go"))
        value = go_result()
        value["receipt"]["application"]["request"]["framed_hex"] = b"coordinated:old-case".hex()
        self.assertTrue(actor_errors(value, "go"))

    def test_unjoined_workers_or_forced_cleanup_is_not_success(self):
        for field in ("joined", "native_dial_joined"):
            value = go_result()
            value[field] = False
            self.assertTrue(actor_errors(value, "go"))
        value = rust_result()
        value["task_join"]["fixture_owned_tasks_joined"] = False
        self.assertTrue(actor_errors(value, "rust"))

    def test_rust_requested_role_does_not_override_actual_upgrade(self):
        value = rust_result("destination")
        value["raw_upgrade_observations"]["connections"][0]["negotiations"][0]["direction"] = "outbound"
        self.assertTrue(actor_errors(value, "rust", "destination"))

    def test_rust_requires_actual_socket_and_transport_output_correlation(self):
        for mutation in ("socket", "output", "duplicate", "peer"):
            value = rust_result()
            connection = value["raw_upgrade_observations"]["connections"][0]
            if mutation == "socket":
                connection["local_address"] = "/ip4/11.0.0.1/tcp/45000"
            elif mutation == "output":
                connection["transport_output_receipts"] = []
            elif mutation == "duplicate":
                value["raw_upgrade_observations"]["connections"].append(deepcopy(connection))
            else:
                connection["authenticated_remote_peer_id"] = "wrong-peer"
            self.assertTrue(actor_errors(value, "rust"))

    def test_rust_requires_actual_stream_digests_and_no_replacement_dial(self):
        value = rust_result()
        value["raw_upgrade_observations"]["connections"][0]["streams"][0]["write"]["framed_sha256"] = "0" * 64
        self.assertTrue(actor_errors(value, "rust"))
        value = rust_result()
        value["native_transport_dials"].append(deepcopy(value["native_transport_dials"][0]))
        self.assertTrue(actor_errors(value, "rust"))

    def test_private_metadata_is_not_installed_key_proof(self):
        value = rust_result()
        value["pnet_fingerprint_basis"] = "coordinator_input_metadata"
        self.assertTrue(actor_errors(value, "rust", profile="private"))

    def test_rust_private_actor_requires_checked_installed_protector_basis(self):
        value = rust_result()
        value["pnet_fingerprint_basis"] = "installed_native_pnet_psk_operational_sha256_v1"
        self.assertEqual(actor_errors(value, "rust", profile="private"), [])
        for basis in (None, "requested_psk", "coordinator_input_metadata"):
            value["pnet_fingerprint_basis"] = basis
            self.assertTrue(actor_errors(value, "rust", profile="private"))

    def test_null_forge_actual_roles_fail_closed(self):
        value = forge_result()
        value["receipt"]["roles"] = None
        self.assertTrue(actor_errors(value, "forge"))
        value = forge_result()
        value["receipt"]["role_source"] = "requested_role"
        self.assertTrue(actor_errors(value, "forge"))

    def test_start_ack_is_not_connection_or_application_evidence(self):
        value = go_result()
        value["status"], value["receipt"] = "started", None
        self.assertTrue(evidence.started_receipt(value, "go", TOKEN))
        self.assertTrue(evidence.validate_connected(value, "go", TOKEN, ready("go", "source"), ready("forge", "destination"), "source"))

    def test_bounded_schema_and_bool_ports(self):
        value = ready("go", "source")
        value["listener_port"] = True
        self.assertTrue(evidence.validate_ready(value, "go", TOKEN, "11.0.0.1"))
        for value in (None, [], {"implementation": "go"}):
            self.assertTrue(evidence.validate_ready(value, "go", TOKEN, "11.0.0.1"))

    def test_native_listener_peer_suffix_is_bound_to_its_local_identity(self):
        value = ready("forge", "source")
        address = value["listener_address"]
        value["listener_address"] = address + "/p2p/" + value["peer_id"]
        self.assertEqual(evidence.validate_ready(value, "forge", TOKEN, "11.0.0.1"), [])
        value["listener_address"] = address + "/p2p/other-peer"
        self.assertTrue(evidence.validate_ready(value, "forge", TOKEN, "11.0.0.1"))


class BarrierEvidenceTests(unittest.TestCase):
    def artifact(self):
        commands = TCPCommands()
        network = CoordinatedNetwork(command_runner=commands, system=lambda: "Linux", ip_lookup=lambda n: n,
                                     outer_namespace_isolated=lambda: True, namespace_token="unit")
        network.setup()
        network.arm({"client": 40010, "server": 40020})
        commands.sockets[network.namespaces["client"]] = "0 1 11.0.0.1:40010 11.0.0.2:40020\n"
        commands.sockets[network.namespaces["server"]] = "0 1 11.0.0.2:40020 11.0.0.1:40010\n"
        network.release()
        network.close()
        return {"network": network.evidence(), "raw": {role: {"ready": ready("go", role)} for role in ("source", "destination")}}

    def test_raw_owned_kernel_snapshots_and_exact_rule_lifetime(self):
        self.assertEqual(evidence.validate_barrier(self.artifact()), [])

    def test_counter_or_requested_port_without_raw_ss_is_rejected(self):
        for mutation in ("raw", "port", "released", "command"):
            artifact = self.artifact()
            coordination = artifact["network"]["coordination"]
            if mutation == "raw":
                coordination["syn_sent"]["client"]["stdout"] = ""
            elif mutation == "port":
                coordination["ports"]["client"] = 45000
            elif mutation == "released":
                coordination["released"] = False
            else:
                artifact["network"]["commands"] = []
            self.assertTrue(evidence.validate_barrier(artifact))

    def test_duplicate_socket_or_unrelated_tuple_is_rejected(self):
        for mutation in ("duplicate", "ephemeral"):
            artifact = self.artifact()
            snapshot = artifact["network"]["coordination"]["syn_sent"]["client"]
            snapshot["stdout"] = snapshot["stdout"] * 2 if mutation == "duplicate" else "0 1 11.0.0.1:45000 11.0.0.2:40020\n"
            self.assertTrue(evidence.validate_barrier(artifact))

    def test_release_requires_two_fresh_command_snapshots_before_first_delete(self):
        artifact = self.artifact()
        commands = artifact["network"]["commands"]
        first_delete = next(i for i, row in enumerate(commands) if "-D" in row["command"])
        commands[first_delete - 1]["stdout"] = ""
        self.assertTrue(evidence.validate_barrier(artifact))


def bilateral_artifact(source="forge", destination="go", profile="native"):
    """Complete synthetic validation vector; it cannot establish native interop."""
    artifact = BarrierEvidenceTests().artifact()
    scenario = "coordinated_dial_port_reuse" + ("_private_pnet" if profile == "private" else "")
    identifier = f"coordinated.{profile}.{source}_to_{destination}"
    transport = "tcp" if profile == "native" else "tcp-pnet-noise"
    work = f"/unit/{identifier}"
    artifact.update(schema_version=1, suite="coordinated", case={"identifier": identifier, "source": source,
                    "destination": destination, "profile": profile, "transport": transport},
                    scenario=scenario, scenario_id=identifier, runner_scenario_id=evidence.REUSE_RUNNER_IDS[profile],
                    acceptance_scenario_ids=[scenario], case_token=TOKEN, pnet_fingerprint="b" * 64 if profile == "private" else None,
                    errors=[], cleanup_errors=[], status="passed", phases={"started": {}, "connected": {}, "exchanged": {}},
                    plans={}, controls=[], processes=[], elapsed_seconds=1)
    for index, (role, implementation) in enumerate((("source", source), ("destination", destination)), 1):
        other = "destination" if role == "source" else "source"
        listener = ready(implementation, role)
        result = {"forge": forge_result, "go": go_result, "rust": rust_result}[implementation](role)
        result["scenario"] = scenario
        result["pnet_fingerprint"] = artifact["pnet_fingerprint"]
        connected, exchanged = deepcopy(result), deepcopy(result)
        connected["status"], exchanged["status"] = "connected", "exchanged"
        connected["joined"], exchanged["joined"] = False, False
        if implementation == "rust":
            connected["application"] = None
        else:
            connected["receipt"].pop("application")
        artifact["phases"]["connected"][role] = connected
        artifact["phases"]["exchanged"][role] = exchanged
        artifact["phases"]["started"][role] = {"implementation": implementation, "case_token": TOKEN, "status": "started",
                    "operation_admitted": True, "admission_source": "coordinated_actor.preflight",
                    "events": [{"kind": "control_completed", "completion": "native_dial_admitted"}]}
        network_role = "client" if role == "source" else "server"
        namespace = next(p["namespace"] for p in artifact["network"]["participants"] if p["role"] == network_role)
        command = ["/unit/ip", "netns", "exec", namespace, f"/unit/{implementation}", "coordinated-live", "--scenario", scenario,
                   "--transport", transport, "--coord-role", "initiator" if role == "source" else "responder", "--case-token", TOKEN,
                   "--bind-ip", "11.0.0.1" if role == "source" else "11.0.0.2", "--timeout-ms", "20000"]
        for name in ("ready", "result", "stop", "control", "plan"):
            command += [f"--{name}-file", f"{work}/{role}.{name}"]
        if implementation == "forge":
            command += ["--store-dir", f"{work}/{role}.store"]
        if profile == "private":
            command += ["--pnet-key-file", "/unit/psk", "--pnet-fingerprint", "b" * 64]
        owner = {"pid": index, "command": command, "log_file": f"{work}/{role}.log", "ready": listener, "outputs": [],
                 "terminal_status": {"exit_code": 0, "termination": "graceful"},
                 "stop_budget": {"native_close_seconds": 8, "post_stop_seconds": 0, "scheduler_allowance_seconds": 2, "seconds": 10}}
        artifact["raw"][role] = {"ready": listener, "result": result, "process": owner}
        artifact["processes"].append(owner)
        target = ready(destination if role == "source" else source, other)
        artifact["plans"][role] = {"case-token": TOKEN, "peer-id": target["peer_id"], "addr": target["listen_addrs"][0]}
        artifact["controls"].append({"actor": role, "sequence": "1", "action": "start", "case-token": TOKEN})
    artifact["controls"].append({"actor": "source", "sequence": "2", "action": "probe", "case-token": TOKEN})
    return artifact


class CaseEvidenceTests(unittest.TestCase):
    def test_canonical_native_and_private_go_pair_synthetic_vectors(self):
        for source, destination in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")):
            self.assertEqual(evidence.validate_case(bilateral_artifact(source, destination)), [])
        for source, destination in (("forge", "go"), ("go", "forge")):
            self.assertEqual(evidence.validate_case(bilateral_artifact(source, destination, "private")), [])

    def test_host_stop_does_not_manufacture_application_success(self):
        artifact = bilateral_artifact()
        artifact["phases"]["exchanged"]["source"] = deepcopy(artifact["phases"]["connected"]["source"])
        self.assertTrue(evidence.validate_case(artifact))

    def test_probe_replacement_is_rejected_even_if_its_final_receipt_is_valid(self):
        artifact = bilateral_artifact()
        artifact["phases"]["connected"]["source"]["receipt"]["connection"]["connection_id"] = 2
        self.assertTrue(evidence.validate_case(artifact))

    def test_metadata_only_rust_private_case_is_not_promoted(self):
        artifact = bilateral_artifact("forge", "rust", "private")
        artifact["raw"]["destination"]["result"]["pnet_fingerprint_basis"] = "coordinator_input_metadata"
        self.assertTrue(evidence.validate_case(artifact))

    def test_fingerprint_mismatch_or_private_tls_fails(self):
        for key, bad in (("pnet_fingerprint", "c" * 64), ("security", "/tls/1.0.0")):
            artifact = bilateral_artifact(profile="private")
            if key == "security":
                artifact["raw"]["source"]["result"]["receipt"]["connection"][key] = bad
            else:
                artifact["raw"]["source"]["result"][key] = bad
            self.assertTrue(evidence.validate_case(artifact))

    def test_ordinary_context_or_wrong_namespace_cannot_be_coordinated(self):
        for index, bad in ((5, "path-live"), (3, "host")):
            artifact = bilateral_artifact()
            artifact["raw"]["source"]["process"]["command"][index] = bad
            self.assertTrue(evidence.validate_case(artifact))

    def test_forced_exit_or_boolean_exit_code_is_not_joined_cleanup(self):
        for terminal in ({"exit_code": 0, "termination": "terminated"}, {"exit_code": False, "termination": "graceful"}):
            artifact = bilateral_artifact()
            artifact["raw"]["source"]["process"]["terminal_status"] = terminal
            self.assertTrue(evidence.validate_case(artifact))



if __name__ == "__main__":
    unittest.main()
