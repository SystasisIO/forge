"""Executable positive mDNS cases, not a capability-promotion manifest."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import secrets
import subprocess
import time

from mdns_evidence import PUBLIC_SERVICE, RESULT_LIMIT, _ready, decode_receipt, validate_mdns_evidence
from mdns_network import IsolatedMdnsNetwork
from process_lifecycle import enter_scope, exit_scope, spawn_owned


READY_TIMEOUT = 20
SETUP_TIMEOUT = 60
OPERATION_TIMEOUT = 45
SHUTDOWN_TIMEOUT = 5
PROCESS_HEADROOM = 5
PROCESS_TIMEOUT = OPERATION_TIMEOUT + SHUTDOWN_TIMEOUT + PROCESS_HEADROOM
# Setup and both readiness waits must not consume the fixture's operation and
# shutdown allowance. The outer bound still applies to the complete exchange.
CASE_TIMEOUT = SETUP_TIMEOUT + 2 * READY_TIMEOUT + PROCESS_TIMEOUT
# Independent pinned Rust golden, valid only for the repository's public test key.
# Do not implement Salsa20 again in the Python harness or use the operational hash.
GOLDEN_KEY_FILE_SHA256 = "a2602305a7773371a5232eb73d3ee4af3fba1efee04c03afa981554a6bebd176"
GOLDEN_NETWORK_FINGERPRINT = "45fc986bbc9388a11d939df26f730f0c"


@dataclass(frozen=True)
class Case:
    client: str
    server: str
    transport: str
    family: int = 4

    def __post_init__(self):
        if ({self.client, self.server} not in ({"forge", "go"}, {"forge", "rust"})
                or self.transport not in ("tcp", "tcp-tls", "quic", "tcp-pnet")
                or type(self.family) is not int or self.family not in (4, 6)
                or (self.transport == "tcp-pnet" and "rust" in (self.client, self.server))):
            raise ValueError("unsupported mDNS case")

    @property
    def identifier(self):
        return f"mdns-ipv{self.family}-{self.transport}-{self.client}-to-{self.server}"


def case_specs() -> tuple[Case, ...]:
    return tuple(
        Case(client, server, transport, family)
        for donor in ("go", "rust")
        for client, server in (("forge", donor), (donor, "forge"))
        for transport in ("tcp", "tcp-tls", "quic") + (("tcp-pnet",) if donor == "go" else ())
        for family in (4, 6)
    )


def private_network_fingerprint(key_file: Path) -> str:
    with key_file.open("rb") as handle:
        raw = handle.read(1025)
    if hashlib.sha256(raw).hexdigest() != GOLDEN_KEY_FILE_SHA256:
        raise ValueError("mDNS private cases require the pinned golden test key")
    return GOLDEN_NETWORK_FINGERPRINT


def _snapshot(owner, flag):
    if owner is None:
        raise ValueError(f"no process owns {flag}")
    outputs = [value for value in owner.outputs if value.get("argument") == flag]
    if len(outputs) != 1 or outputs[0].get("exists") is not True or not outputs[0].get("log_file"):
        raise ValueError(f"missing immutable {flag} capture")
    with Path(outputs[0]["log_file"]).open("rb") as handle:
        raw = handle.read(RESULT_LIMIT + 1)
    if len(raw) > RESULT_LIMIT:
        raise ValueError("oversized captured mDNS result")
    return decode_receipt(raw.decode("utf-8"))


def run_case(spec: Case, binaries: dict[str, Path], root: Path, *, pnet_key: Path,
             wait_json, command_attempt, supplied_network=None) -> dict:
    artifact = {
        "scenario_id": spec.identifier, "suite": "mdns", "status": "failed",
        "client_implementation": spec.client, "server_implementation": spec.server,
        "transport": spec.transport, "family": spec.family, "expected_outcome": "discovered_authenticated_echo",
        "proof_scope": "isolated_two_participants_runtime_count_plus_donor_discovery_and_authenticated_echo",
        "acceptance_scenario_ids": [], "errors": [], "cleanup_errors": [],
    }
    work = root / spec.identifier
    # Isolation controls transfer their still-owned network to this final phase.
    network = supplied_network if supplied_network is not None else IsolatedMdnsNetwork(spec.family)
    scope, token = enter_scope()
    owners, readiness, raw = {}, {}, {}
    payload = secrets.token_hex(32)
    fingerprint = None
    started = time.monotonic()
    deadline = started + CASE_TIMEOUT

    def remaining(limit=CASE_TIMEOUT):
        value = min(limit, deadline - time.monotonic())
        if value <= 0:
            raise TimeoutError("mDNS case deadline expired")
        return value

    try:
        work.mkdir(parents=True, exist_ok=False)
        if spec.transport == "tcp-pnet":
            fingerprint = private_network_fingerprint(pnet_key)
        service = PUBLIC_SERVICE if fingerprint is None else f"_p2p-{fingerprint}._udp.local"
        artifact["expected_service_name"] = service
        artifact["challenge"] = {"bytes": len(payload), "sha256": hashlib.sha256(payload.encode()).hexdigest()}
        if supplied_network is None:
            network.setup()
        if time.monotonic() - started > SETUP_TIMEOUT:
            raise TimeoutError("mDNS network setup allowance expired")
        # Neither command consumes the opposite participant's readiness or coordinates.
        for role, implementation in (("server", spec.server), ("client", spec.client)):
            ready_path = work / f"{role}.ready.json"
            stop_path = work / f"{role}.stop"
            command = [str(binaries[implementation]), "listen" if role == "server" else "dial",
                       "--scenario", "mdns", "--transport", spec.transport,
                       "--bind-ip", network.addresses[role][0], "--payload", payload,
                       "--ready-file", str(ready_path), "--result-file", str(work / f"{role}.json"),
                       "--stop-file", str(stop_path)]
            if fingerprint is not None:
                command += ["--pnet-key-file", str(pnet_key)]
            command = network.namespace_command(role, command)
            attempt = command_attempt(command, work / f"{role}.log", spec.identifier, 1, role, CASE_TIMEOUT)
            owner = spawn_owned(command, work / f"{role}.log", stop_path, attempt)
            owners[role] = owner
            try:
                ready = wait_json(ready_path, remaining(READY_TIMEOUT))
            except TimeoutError:
                attempt["timeout_class"] = "readiness_timeout"
                raise
            _ready(ready, implementation, "listener" if role == "server" else "dialer", service)
            owner.ready = ready
            readiness[role] = ready
        client = owners["client"]
        try:
            code = client.process.wait(timeout=remaining(PROCESS_TIMEOUT))
        except subprocess.TimeoutExpired:
            scope.attempts[-1]["timeout_class"] = "fixture_timeout"
            raise
        if code != 0:
            raise RuntimeError(f"mDNS dialer exited with {code}")
        # Wait for the listener's provisional success before requesting its stop.
        # Only the post-exit immutable receipt below is accepted as evidence.
        provisional = wait_json(work / "server.json", remaining())
        if not isinstance(provisional, dict) or provisional.get("status") != "ok":
            raise ValueError("mDNS listener did not finish its echo exchange")
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
            for role in ("client", "server"):
                try:
                    raw[role] = _snapshot(owners.get(role), "--result-file")
                    artifact[role] = raw[role]
                    captured = _snapshot(owners.get(role), "--ready-file")
                    if captured != readiness.get(role):
                        raise ValueError("readiness changed after launch")
                    artifact[f"{role}_ready"] = captured
                except Exception as error:
                    artifact["errors"].append(f"{role} capture: {error}")
        finally:
            try:
                artifact["cleanup_errors"].extend(network.close())
                evidence = network.evidence()
                if work.is_dir():
                    path = work / "network.json"
                    path.write_text(json.dumps(evidence, indent=2) + "\n")
                    artifact["network"] = {"result_file": str(path), **evidence}
                else:
                    artifact["network"] = evidence
                if (evidence["state"] != "closed" or evidence["cleanup_failures"]
                        or evidence["cleanup_uncertainty"]):
                    artifact["cleanup_errors"].append("mDNS network did not close cleanly")
            except Exception as error:
                artifact["cleanup_errors"].append(f"network cleanup: {error}")
            finally:
                exit_scope(token)
    artifact["errors"].extend(validate_mdns_evidence(
        raw.get("client"), raw.get("server"), client_impl=spec.client, server_impl=spec.server,
        transport=spec.transport, family=spec.family, client_ready=readiness.get("client"),
        server_ready=readiness.get("server"), network=artifact.get("network"), payload=payload.encode(),
        private_fingerprint=fingerprint,
        client_terminal_status=owners["client"].terminal_status if "client" in owners else None,
        server_terminal_status=owners["server"].terminal_status if "server" in owners else None,
    ))
    if not artifact["errors"] and not artifact["cleanup_errors"]:
        artifact["status"] = "passed"
    return artifact


def run_suite(binaries, root, *, pnet_key, wait_json, command_attempt):
    for spec in case_specs():
        yield run_case(spec, binaries, root, pnet_key=pnet_key, wait_json=wait_json, command_attempt=command_attempt)
