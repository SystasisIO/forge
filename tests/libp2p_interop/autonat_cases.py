"""Formal isolated AutoNAT cases; process and provenance owners remain shared."""

from dataclasses import dataclass
import json
from pathlib import Path
import re
import subprocess

from autonat_evidence import (
    PROTOCOLS, REVISIONS, _address, _bounded_event, _connections, _integer, _local_matches, _object,
    _result, _terminal, validate_autonat_evidence,
)
from autonat_network import IsolatedAutonatNetwork
from process_lifecycle import enter_scope, exit_scope, spawn_owned
from provenance import reject_duplicate_json_keys


READY_TIMEOUT = 20
CLIENT_TIMEOUT = 45
RESULT_LIMIT = 1024 * 1024


@dataclass(frozen=True)
class Case:
    client: str
    server: str
    version: int
    transport: str
    outcome: str = "reachable"

    @property
    def scenario(self):
        return f"autonat_v{self.version}"

    @property
    def identifier(self):
        return f"{self.scenario}-{self.transport}-{self.client}-to-{self.server}-{self.outcome}"


def case_specs() -> tuple[Case, ...]:
    cases = []
    for donor in ("go", "rust"):
        for client, server in (("forge", donor), (donor, "forge")):
            for version in (1, 2):
                cases.extend(Case(client, server, version, transport)
                             for transport in ("tcp", "tcp-tls", "quic"))
                cases.append(Case(client, server, version, "tcp-pnet"))
                cases.append(Case(client, server, version, "tcp-pnet", "policy_denied"))
    cases.append(Case("forge", "go", 1, "tcp", "unreachable"))
    return tuple(cases)


def _endpoint(value, ip, transport, peer=None, port=None):
    """Bind numeric fixture endpoints to the runner-owned namespace, not DNS."""
    if not isinstance(value, str) or len(value) > 512:
        raise ValueError("missing bounded namespace endpoint")
    match = re.fullmatch(r"/ip4/(11\.0\.0\.[123])/(tcp|udp)/([0-9]+)(/quic-v1)?(?:/p2p/([^/\s]+))?", value)
    if not match:
        raise ValueError(f"invalid namespace endpoint: {value}")
    actual_ip, kind, number, quic, suffix = match.groups()
    parsed_port = int(number)
    if (
        actual_ip != ip or str(parsed_port) != number or not 0 < parsed_port < 65536
        or kind != ("udp" if transport == "quic" else "tcp")
        or bool(quic) != (transport == "quic")
        or (suffix is not None and suffix != peer)
        or (port is not None and parsed_port != port)
    ):
        raise ValueError("endpoint disagrees with namespace IP, transport, peer or port")
    return f"/ip4/{ip}/{kind}/{number}" + ("/quic-v1" if quic else "")


def _readiness(ready, spec, server_ip):
    if not isinstance(ready, dict):
        raise ValueError("listener readiness must be an object")
    peer = ready.get("peer_id")
    if (
        ready.get("implementation") != spec.server or ready.get("role") != "listener"
        or not isinstance(peer, str) or not 0 < len(peer) <= 256 or any(c.isspace() for c in peer)
        or ready.get("protocol") != PROTOCOLS[spec.version] or ready.get("service_enabled") is not True
        or (spec.server != "rust" and ready.get("status") != "ready")
        or (spec.server == "rust" and ready.get("scenario") != spec.scenario)
        or ("version" in ready and (type(ready["version"]) is not int or ready["version"] != spec.version))
        or ("transport" in ready and ready["transport"] != spec.transport)
    ):
        raise ValueError("listener readiness identity/role/protocol mismatch")
    addresses = ready.get("listen_addrs")
    if addresses is None:
        addresses = [ready.get("listen_addr") or ready.get("addr")]
    if not isinstance(addresses, list) or len(addresses) != 1:
        raise ValueError("AutoNAT service requires exactly one ready listener")
    return peer, _endpoint(addresses[0], server_ip, spec.transport, peer)


def _snapshot(owner, flag):
    if owner is None:
        raise ValueError(f"no process owns {flag}")
    outputs = [o for o in owner.outputs if o.get("argument") == flag]
    if len(outputs) != 1 or outputs[0].get("exists") is not True or not outputs[0].get("log_file"):
        raise ValueError(f"missing captured {flag} for {owner.log_file}")
    path = Path(outputs[0]["log_file"])
    if path.stat().st_size > RESULT_LIMIT:
        raise ValueError(f"oversized captured JSON: {path}")
    value = json.loads(path.read_text(), object_pairs_hook=reject_duplicate_json_keys)
    if not isinstance(value, dict):
        raise ValueError(f"captured JSON is not an object: {path}")
    return value


def _pair_binding(spec, client, server, ready, addresses):
    peer, listener = _readiness(ready, spec, addresses["server"][0])
    if not isinstance(client, dict) or not isinstance(server, dict):
        raise ValueError("both immutable fixture results are required")
    if client.get("implementation") != spec.client or server.get("implementation") != spec.server:
        raise ValueError("fixture result implementation differs from launched binary")
    server_peer = server.get("peer_id" if spec.server == "rust" else "local_peer_id")
    if server_peer != peer:
        raise ValueError("listener result peer differs from readiness")
    client_peer = client.get("peer_id" if spec.client == "rust" else "local_peer_id")
    target_ip = addresses["client"][1 if spec.version == 2 else 0]
    target = _endpoint(client.get("requested_addr"), target_ip, spec.transport, client_peer,
                       9 if spec.outcome == "unreachable" else None)
    records = client.get("actual_connections")
    if not isinstance(records, list):
        raise ValueError("client has no control connection trace")
    controls = [c for c in records if isinstance(c, dict) and c.get("direction") == "outbound"
                and c.get("authenticated_peer") == peer
                and (spec.client == "forge" or c.get("connection_id") == client.get("control_connection_id"))]
    if not controls or any(_endpoint(c.get("remote_addr"), addresses["server"][0], spec.transport, peer)
                           != listener for c in controls):
        raise ValueError("actual control destination differs from the launched ready listener")
    received = server.get("actual_connections")
    if not isinstance(received, list):
        raise ValueError("server has no independently observed control source")
    received_controls = [c for c in received if isinstance(c, dict) and c.get("direction") == "inbound"
                         and c.get("authenticated_peer") == client_peer]
    try:
        sources = {_endpoint(c.get("remote_addr"), addresses["client"][0], spec.transport, client_peer)
                   for c in received_controls
                   if _endpoint(c.get("local_addr"), addresses["server"][0], spec.transport, peer) == listener}
    except ValueError as error:
        raise ValueError("actual control source differs from primary namespace IP") from error
    if len(sources) != 1:
        raise ValueError("actual control source lacks paired primary namespace IP evidence")
    source = next(iter(sources))
    for control in controls:
        local = control.get("local_addr")
        if spec.client == "rust" and local is None:
            # Rust does not expose its outbound socket endpoint. Its Identify
            # observation must agree with the independent service-side socket.
            local = client.get("observer_reported_control_addr")
        if not _local_matches(_address(local, spec.transport, client_peer,
                                       outbound_quic_bind=spec.client in ("forge", "go")), source):
            raise ValueError("actual control source lacks paired primary namespace IP evidence")
    return peer, target


def validate_policy_denial(spec, client, server, peer, client_terminal, server_terminal):
    """Local client policy control only, never a native NAT classification."""
    errors = []
    _terminal(client_terminal, "client", errors)
    _terminal(server_terminal, "server", errors)
    checked_server = _result(server, "listener", spec.version, "tcp-pnet", errors)
    if not isinstance(client, dict) or checked_server is None:
        return errors + ["policy control needs both results"]
    impl = spec.client
    if (
        client.get("implementation") != impl or client.get("role") != "dialer"
        or type(client.get("version")) is not int or client.get("version") != spec.version
        or client.get("scenario") != spec.scenario or client.get("protocol") != PROTOCOLS[spec.version]
        or client.get("transport") != "tcp-pnet" or client.get("internet_egress") != "deny"
        or client.get("status") != "rejected" or client.get("reached") is not False
        or client.get("response_addr") is not None
        or client.get("probe_state") not in (None, "unknown") or client.get("v1_vote") is not None
        or client.get("autonat_v2_address_state") is not None or client.get("current_v2_address_state") is not None
        # Forge exercises its public API and expects typed rejection. The donor
        # fixture policies reject before calling their native client APIs.
        or not _integer(client.get("autonat_probe_api_calls"),
                        1 if impl == "forge" else 0, 1 if impl == "forge" else 0)
    ):
        errors.append("client lacks explicit no-probe policy rejection")
    for key in ("error", "cleanup_error", "trace_error", "trace_overflow_error", "handler_join_error",
                "handler_error", "connection_gater_trace_failure"):
        if client.get(key) is not None:
            errors.append(f"policy client reports {key}")
    if client.get("cleanup_errors") not in (None, []):
        errors.append("policy client reports cleanup errors")
    if impl == "rust":
        if (client.get("policy_basis") != "fixture_policy_not_donor_feature"
                or client.get("donor_api_basis") != "fixtureegresspolicy_after_authenticated_control"
                or client.get("authenticated_peer") != peer):
            errors.append("Rust denial is not tied to authenticated Identify and fixture policy")
        required = ("trace_complete", "swarm_closed", "connection_drain", "joined_swarm_executor")
        events = client.get("donor_events")
        if (not isinstance(events, list) or len(events) > 64 or any(not _bounded_event(e) for e in events)
                or any(e.get("kind") != "v1_outbound_error" or e.get("error_kind") != "NoServer"
                       or e.get("peer") is not None for e in events)):
            errors.append("Rust denial has probe activity or malformed events")
        if not _integer(client.get("v1_request_events"), 0, 0) or not _integer(client.get("v2_result_events"), 0, 0):
            errors.append("Rust denial has actual probe results")
    else:
        required = ("hosts_closed", "handlers_joined",
                    "connection_gater_trace_complete" if impl == "forge" else "trace_complete")
        if (client.get("policy_denied") is not True or client.get("control_authenticated_peer") != peer
                or client.get("control_identify_completed") is not True):
            errors.append("denial lacks explicit policy/authenticated Identify evidence")
    if any(client.get(key) is not True for key in required):
        errors.append("policy client lacks joined complete trace")
    if impl != "forge" and client.get("donor_revision") != REVISIONS[impl]:
        errors.append("policy donor revision mismatch")
    if impl == "forge":
        before, after = _object(client.get("diagnostics_before_stop")), _object(client.get("diagnostics_after_stop"))
        state = before.get("reachability") if before else None
        resources = after.get("resources") if after else None
        terminal = after.get("reachability") if after else None
        if (not isinstance(state, dict) or state.get("internet_egress_allowed") is not False
                or state.get("client_v1_enabled") is not False or state.get("client_v2_enabled") is not False):
            errors.append("Forge denial diagnostics do not disable Internet AutoNAT clients")
        if (after is None or after.get("lifecycle_phase") != "stopped"
                or not _integer(after.get("active_sessions"), 0, 0) or not isinstance(resources, dict)
                or any(not _integer(resources.get(k), 0, 0) for k in (
                    "system_memory", "inbound_connections", "outbound_connections", "active_dials", "active_service_scopes"))
                or not isinstance(terminal, dict)
                or any(not _integer(terminal.get(k), 0, 0) for k in ("pending_probes", "active_handlers"))):
            errors.append("Forge denial lacks terminal resource cleanup")
        if client.get("v1_vote") is not None or client.get("autonat_v2_address_state") is not None:
            errors.append("policy denial must not contain a reachability vote")
    cp = client.get("peer_id" if impl == "rust" else "local_peer_id")
    cc = _connections(client, "actual_connections", "tcp-pnet", "client", errors)
    sc = _connections(server, "actual_connections", "tcp-pnet", "server", errors)
    if len(cc) != 1 or len(sc) != 1:
        errors.append("policy control must contain only its single authenticated control connection")
    elif (cc[0]["direction"] != "outbound" or cc[0]["authenticated_peer"] != peer
          or sc[0]["direction"] != "inbound" or sc[0]["authenticated_peer"] != cp
          or cc[0]["remote_addr"] != sc[0]["local_addr"]
          or (client.get("observer_reported_control_addr") if impl == "rust" else cc[0]["local_addr"])
          != sc[0]["remote_addr"]):
        errors.append("policy control connection is not independently authenticated and paired")
    if spec.server == "go" and (server.get("probe_connections") != [] or server.get("service_completed_requests") != []):
        errors.append("Go service dialed or handled a probe during client policy denial")
    if spec.server == "rust" and server.get("donor_events") != []:
        errors.append("Rust service handled a probe during client policy denial")
    return errors


def run_case(spec: Case, binaries: dict[str, Path], root: Path, *, pnet_key: Path,
             pnet_fingerprint: str, wait_json, command_attempt) -> dict:
    """One attempt, one process scope; commit only after process/network cleanup."""
    artifact = {"scenario_id": spec.identifier, "suite": "autonat", "status": "failed",
                "client_implementation": spec.client, "server_implementation": spec.server,
                "version": spec.version, "transport": spec.transport, "expected_outcome": spec.outcome,
                "proof_scope": "client_policy_control" if spec.outcome == "policy_denied" else "isolated_native_autonat_pair",
                "errors": [], "cleanup_errors": []}
    work = root / spec.identifier
    network = IsolatedAutonatNetwork(include_client_secondary_address=spec.version == 2)
    scope, token = enter_scope()
    server = client = None
    ready = None
    raw = {}
    try:
        work.mkdir(parents=True, exist_ok=False)
        network.setup()
        if spec.outcome == "unreachable":
            network.assert_closed_tcp_port("client", 9)
        def launch(role, implementation, extra, timeout):
            command = [str(binaries[implementation]), "listen" if role == "server" else "dial",
                       "--scenario", spec.scenario, "--transport", spec.transport,
                       "--bind-ip", network.addresses[role][0],
                       "--result-file", str(work / f"{role}.json"), *extra]
            if implementation == "forge":
                command += ["--store-dir", str(work / f"{role}-store")]
            if spec.transport == "tcp-pnet":
                command += ["--pnet-key-file", str(pnet_key), "--internet-egress",
                            "deny" if role == "client" and spec.outcome == "policy_denied" else "allow"]
            command = network.namespace_command(role, command)
            attempt = command_attempt(command, work / f"{role}.log", spec.identifier, 1, role, timeout)
            owned = spawn_owned(command, work / f"{role}.log",
                                work / "server.stop" if role == "server" else None, attempt)
            return owned, attempt

        server, server_attempt = launch("server", spec.server, [
            "--ready-file", str(work / "ready.json"), "--stop-file", str(work / "server.stop"),
        ], READY_TIMEOUT)
        try:
            ready = wait_json(work / "ready.json", READY_TIMEOUT)
        except TimeoutError:
            server_attempt["timeout_class"] = "readiness_timeout"
            raise
        peer, listener = _readiness(ready, spec, network.addresses["server"][0])
        server.ready = ready
        extra = ["--peer-id", peer, "--addr", listener]
        if spec.version == 2:
            extra += ["--probe-addr", f"/ip4/{network.addresses['client'][1]}/" +
                      ("udp/0/quic-v1" if spec.transport == "quic" else "tcp/0")]
        if spec.outcome == "unreachable":
            extra += ["--expect-unreachable", "true", "--probe-addr", "/ip4/11.0.0.1/tcp/9"]
        client, attempt = launch("client", spec.client, extra, CLIENT_TIMEOUT)
        try:
            attempt["exit_code"] = client.process.wait(timeout=CLIENT_TIMEOUT)
        except subprocess.TimeoutExpired:
            attempt["timeout_class"] = "fixture_timeout"
            raise
        if attempt["exit_code"] != 0:
            attempt["failure_class"] = "process_exit"
            raise RuntimeError(f"client exited with {attempt['exit_code']}")
    except Exception as error:
        artifact["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        try:
            try:
                artifact["cleanup_errors"].extend(scope.close())
            except Exception as error:
                artifact["cleanup_errors"].append(f"process scope close: {error}")
            artifact["owned_processes"] = scope.evidence()
            artifact["attempts"] = scope.attempts
            for attempt in scope.attempts:
                attempt["exit_code"] = attempt.get("terminal_status", {}).get("exit_code")
            for role, owner in (("client", client), ("server", server)):
                try:
                    raw[role] = _snapshot(owner, "--result-file")
                    artifact[role] = raw[role]
                except Exception as error:
                    artifact["errors"].append(f"{role} result: {error}")
            if server is not None:
                try:
                    captured_ready = _snapshot(server, "--ready-file")
                    if ready != captured_ready:
                        raise ValueError("readiness changed after launch")
                except Exception as error:
                    artifact["errors"].append(f"readiness snapshot: {error}")
            if spec.outcome == "unreachable" and network.evidence()["state"] == "ready":
                try:
                    network.assert_closed_tcp_port("client", 9)
                except Exception as error:
                    artifact["errors"].append(f"negative port after probe: {error}")
        finally:
            try:
                artifact["cleanup_errors"].extend(network.close())
            except Exception as error:
                artifact["cleanup_errors"].append(f"network close: {error}")
            try:
                evidence = network.evidence()
                path = work / "network.json"
                path.write_text(json.dumps(evidence, indent=2) + "\n")
                artifact["network"] = {"result_file": str(path), **evidence}
                if evidence["state"] != "closed" or evidence["cleanup_failures"] or evidence["cleanup_uncertainty"]:
                    artifact["cleanup_errors"].append("network did not close cleanly")
            except Exception as error:
                artifact["cleanup_errors"].append(f"network evidence: {error}")
            exit_scope(token)
    try:
        peer, target = _pair_binding(spec, raw.get("client"), raw.get("server"), ready, network.addresses)
        artifact["expected_service_peer"] = peer
        artifact["expected_requested_address"] = target
        artifact["requested_address_binding"] = (
            "namespace_IP_transport_policy_input_only" if spec.outcome == "policy_denied" else
            "namespace_IP_transport_explicit_closed_port" if spec.outcome == "unreachable" else
            "namespace_IP_transport_plus_actual_paired_listener"
        )
        if spec.outcome == "policy_denied":
            artifact["errors"].extend(validate_policy_denial(
                spec, raw["client"], raw["server"], peer, client.terminal_status, server.terminal_status))
        else:
            artifact["errors"].extend(validate_autonat_evidence(
                raw["client"], raw["server"], version=spec.version, transport=spec.transport,
                expected_service_peer=peer, expected_requested_address=target,
                client_terminal_status=client.terminal_status, server_terminal_status=server.terminal_status,
                expected_outcome=spec.outcome))
        if spec.transport == "tcp-pnet":
            artifact["pnet_fingerprint"] = pnet_fingerprint
    except Exception as error:
        artifact["errors"].append(f"pair validation: {error}")
    if not artifact["errors"] and not artifact["cleanup_errors"]:
        artifact["status"] = "passed"
    return artifact


def run_suite(binaries, root, *, pnet_key, pnet_fingerprint, wait_json, command_attempt, claims_for_case):
    for spec in case_specs():
        artifact = run_case(spec, binaries, root, pnet_key=pnet_key, pnet_fingerprint=pnet_fingerprint,
                            wait_json=wait_json, command_attempt=command_attempt)
        artifact["acceptance_scenario_ids"] = claims_for_case(spec)
        yield artifact
