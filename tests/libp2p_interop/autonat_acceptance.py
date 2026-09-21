"""Exact paired AutoNAT receipts; registration is not a support promotion.

Role attribution corrects the generic four-direction template: Forge client
receipts cover only client capabilities, and Forge service receipts only service
capabilities. The complete bilateral 41-case suite remains mandatory.
"""

from pathlib import Path
import re
from collections import Counter

from autonat_cases import (
    CLIENT_TIMEOUT, READY_TIMEOUT, _pair_binding, _readiness, case_specs,
    validate_policy_denial,
)
from autonat_evidence import validate_autonat_evidence
from autonat_network import CommandResult, IsolatedAutonatNetwork
from stage6_evidence_contract import evidence_contract_for


ROLE_DIRECTIONS = {
    "client": {"forge_to_go", "forge_to_rust"},
    "service": {"go_to_forge", "rust_to_forge"},
}
SCENARIOS = {
    f"autonat_v{version}_{role}{suffix}": (
        f"protocol.autonat_v{version}_{role}", role, version, transport, profile,
    )
    for version in (1, 2) for role in ROLE_DIRECTIONS
    for suffix, transport, profile in (
        ("", "quic", "quic_stage6"),
        ("_native_tcp_yamux", "tcp", "tcp_stage6"),
        ("_private_tcp_yamux_pnet", "tcp-pnet", "private_tcp_yamux_pnet"),
    )
}
EVIDENCE_CONTRACTS = {evidence_contract_for(name) for name in SCENARIOS}


def acceptance_id(spec):
    if spec.outcome != "reachable" or spec.transport == "tcp-tls":
        return None
    role = "client" if spec.client == "forge" else "service"
    return next(name for name, (_, r, v, t, _) in SCENARIOS.items()
                if (r, v, t) == (role, spec.version, spec.transport))


def _network(spec, evidence):
    """Replay captured commands through the real owner, never execute commands.

    Exact runner provenance establishes who captured the transcript; replay
    checks its content, including routes, links, closed-port checks and joins.
    """
    if not isinstance(evidence, dict):
        raise ValueError("missing network evidence")
    outer = evidence.get("outer_namespace")
    if not isinstance(outer, str) or re.fullmatch(r"autonat-o-[a-z0-9]{1,20}", outer) is None:
        raise ValueError("invalid owned namespace")
    records = evidence.get("commands")
    if (not isinstance(records, list) or not 1 <= len(records) <= 128
            or any(not isinstance(record, dict) for record in records)):
        raise ValueError("missing bounded namespace transcript")
    ip = records[0].get("command", [None])[0]
    if not isinstance(ip, str) or not Path(ip).is_absolute() or Path(ip).name != "ip":
        raise ValueError("namespace transcript has no absolute iproute launcher")
    ss = None
    for record in records:
        command = record.get("command")
        if (set(record) != {"command", "returncode", "stdout", "stderr"}
                or not isinstance(command, list) or not command
                or any(not isinstance(arg, str) or not arg for arg in command)
                or type(record["returncode"]) is not int or record["returncode"] != 0
                or any(not isinstance(record[k], str) or len(record[k]) > 65536 for k in ("stdout", "stderr"))):
            raise ValueError("failed or malformed namespace command")
        if command[-5:] == ["-H", "-lnt", "sport", "=", ":9"]:
            value = command[-6]
            if not Path(value).is_absolute() or Path(value).name != "ss" or ss not in (None, value):
                raise ValueError("invalid closed-port launcher")
            ss = value
    cursor = 0

    def replay(command):
        nonlocal cursor
        if cursor >= len(records) or records[cursor]["command"] != command:
            raise ValueError("namespace command order/ownership differs from canonical setup and close")
        record = records[cursor]
        cursor += 1
        return CommandResult(record["returncode"], record["stdout"], record["stderr"])

    network = IsolatedAutonatNetwork(
        spec.version == 2, command_runner=replay, system=lambda: "Linux",
        ip_lookup=lambda name: {"ip": ip, "ss": ss}.get(name),
        outer_namespace_isolated=lambda: True, namespace_token=outer.removeprefix("autonat-o-"),
    )
    network.setup()
    if spec.outcome == "unreachable":
        network.assert_closed_tcp_port("client", 9)
        network.assert_closed_tcp_port("client", 9)
    if network.close() or cursor != len(records) or network.evidence() != evidence:
        raise ValueError("namespace evidence differs from complete canonical lifecycle")
    return network, ip


def validate_pair(record, spec, root, binaries, load_indexed_json, fingerprint, pnet_key):
    """Validate a pair after the common checker verified the complete index."""
    errors = []
    expected = {
        "scenario_id": spec.identifier, "suite": "autonat", "status": "passed",
        "client_implementation": spec.client, "server_implementation": spec.server,
        "version": spec.version, "transport": spec.transport, "expected_outcome": spec.outcome,
        "proof_scope": "client_policy_control" if spec.outcome == "policy_denied" else "isolated_native_autonat_pair",
        "errors": [], "cleanup_errors": [],
    }
    if any(record.get(key) != value for key, value in expected.items()):
        raise ValueError("pair identity, outcome or terminal verdict mismatch")
    fields = set(expected) | {"owned_processes", "attempts", "client", "server", "network",
                              "expected_service_peer", "expected_requested_address",
                              "requested_address_binding", "acceptance_scenario_ids"}
    if spec.transport == "tcp-pnet":
        fields.add("pnet_fingerprint")
    if set(record) != fields:
        raise ValueError("pair contains missing fields or undeclared claims")
    binding = ("namespace_IP_transport_policy_input_only" if spec.outcome == "policy_denied" else
               "namespace_IP_transport_explicit_closed_port" if spec.outcome == "unreachable" else
               "namespace_IP_transport_plus_actual_paired_listener")
    if record["requested_address_binding"] != binding:
        raise ValueError("pair address attribution basis differs from its outcome")
    if type(record.get("version")) is not int:
        raise ValueError("pair version must be an integer")
    work = root / spec.identifier
    raw_network = record.get("network")
    if not isinstance(raw_network, dict) or raw_network.get("result_file") != str(work / "network.json"):
        raise ValueError("network proof is not owned by this case")
    network_payload = load_indexed_json(raw_network["result_file"])
    if network_payload != {k: v for k, v in raw_network.items() if k != "result_file"}:
        raise ValueError("network raw file differs from record")
    network, ip = _network(spec, network_payload)
    owners, attempts = record.get("owned_processes"), record.get("attempts")
    if not isinstance(owners, list) or not isinstance(attempts, list) or len(owners) != 2 or len(attempts) != 2:
        raise ValueError("pair requires exactly two owned processes and single attempts")
    payloads, terminals = {}, {}
    ready = None
    for role, impl, owner, attempt in zip(("server", "client"), (spec.server, spec.client), owners, attempts):
        if not isinstance(owner, dict) or not isinstance(attempt, dict):
            raise ValueError("invalid process owner or attempt")
        if set(owner) != {"pid", "command", "log_file", "terminal_status", "ready", "outputs"}:
            raise ValueError("process owner contains missing or unexpected failure evidence")
        command = owner.get("command")
        action = "listen" if role == "server" else "dial"
        if (not isinstance(command, list) or len(command) < 6 or len(command[6:]) % 2
                or command[:6] != [ip, "netns", "exec", network.namespaces[role], str(binaries[impl]), action]
                or any(not isinstance(arg, str) or not arg for arg in command)):
            raise ValueError("process does not execute exact fixture inside its owned namespace")
        options = dict(zip(command[6::2], command[7::2]))
        if len(options) * 2 != len(command[6:]):
            raise ValueError("duplicate fixture flags")
        required = {
            "--scenario": spec.scenario, "--transport": spec.transport,
            "--bind-ip": network.addresses[role][0], "--result-file": str(work / f"{role}.json"),
        }
        if role == "server":
            required.update({"--ready-file": str(work / "ready.json"), "--stop-file": str(work / "server.stop")})
        else:
            peer, address = _readiness(ready, spec, network.addresses["server"][0])
            required.update({"--peer-id": peer, "--addr": address})
            if spec.version == 2:
                required["--probe-addr"] = "/ip4/11.0.0.3/" + ("udp/0/quic-v1" if spec.transport == "quic" else "tcp/0")
            if spec.outcome == "unreachable":
                required.update({"--expect-unreachable": "true", "--probe-addr": "/ip4/11.0.0.1/tcp/9"})
        if impl == "forge":
            required["--store-dir"] = str(work / f"{role}-store")
        if spec.transport == "tcp-pnet":
            required.update({"--pnet-key-file": str(pnet_key), "--internet-egress":
                             "deny" if role == "client" and spec.outcome == "policy_denied" else "allow"})
            actual, failures = fingerprint(required["--pnet-key-file"], root)
            errors.extend(failures)
            if actual is None or actual != record.get("pnet_fingerprint"):
                errors.append("pair private key fingerprint mismatch")
        if options != required:
            raise ValueError("fixture flags differ from exact case contract")
        log = str(work / f"{role}.log")
        terminal = owner.get("terminal_status")
        if (terminal != {"exit_code": 0, "termination": "graceful"}
                or type(terminal["exit_code"]) is not int
                or type(owner.get("pid")) is not int or owner["pid"] <= 0):
            raise ValueError("process lacks graceful joined terminal")
        for key, value in {
            "kind": role, "scenario_id": spec.identifier, "attempt_id": 1, "command": command,
            "requested_log_file": log, "log_file": log, "pid": owner["pid"], "exit_code": 0,
            "terminal_status": terminal, "timeout_seconds": READY_TIMEOUT if role == "server" else CLIENT_TIMEOUT,
            "outputs": owner.get("outputs"),
        }.items():
            if attempt.get(key) != value:
                raise ValueError(f"attempt {key} differs from owned execution")
        if any(key in attempt or key in owner for key in ("timeout_class", "failure_class", "spawn_error")):
            raise ValueError("failed attempt cannot be acceptance evidence")
        if set(attempt) != {"kind", "scenario_id", "attempt_id", "command", "requested_log_file",
                           "log_file", "pid", "exit_code", "terminal_status", "timeout_seconds", "outputs"}:
            raise ValueError("attempt has unexpected execution or failure fields")
        if owner.get("log_file") != log:
            raise ValueError("log is not owned by case")
        outputs = owner.get("outputs")
        flags = ["--ready-file", "--result-file"] if role == "server" else ["--result-file"]
        if not isinstance(outputs, list) or len(outputs) != len(flags):
            raise ValueError("missing immutable process outputs")
        for output, flag in zip(outputs, flags):
            snapshot = f"{log}.{flag[2:]}.json"
            if output != {"argument": flag, "path": options[flag], "exists": True, "log_file": snapshot}:
                raise ValueError("immutable output does not bind its requested path")
            payload = load_indexed_json(snapshot)
            if flag == "--ready-file":
                ready = payload
                if owner.get("ready") != ready:
                    raise ValueError("ready file differs from launched service")
            else:
                payloads[role] = payload
                if record.get(role) != payload:
                    raise ValueError("raw fixture result differs from pair payload")
        terminals[role] = terminal
    peer, address = _pair_binding(spec, payloads["client"], payloads["server"], ready, network.addresses)
    if record.get("expected_service_peer") != peer or record.get("expected_requested_address") != address:
        errors.append("pair expected identity/address differs from independent readiness and listener")
    if spec.outcome == "policy_denied":
        errors.extend(validate_policy_denial(spec, payloads["client"], payloads["server"], peer,
                                             terminals["client"], terminals["server"]))
    else:
        errors.extend(validate_autonat_evidence(
            payloads["client"], payloads["server"], version=spec.version, transport=spec.transport,
            expected_service_peer=peer, expected_requested_address=address,
            client_terminal_status=terminals["client"], server_terminal_status=terminals["server"],
            expected_outcome=spec.outcome))
    return errors


def validate_suite(records, required, root, binaries, load_indexed_json, fingerprint, pnet_key):
    specs = case_specs()
    expected = {spec.identifier: spec for spec in specs}
    coverage = Counter((s.transport, s.outcome) for s in specs)
    if (len(specs) != 41 or len(expected) != 41 or coverage != {
        ("tcp", "reachable"): 8, ("tcp-tls", "reachable"): 8, ("quic", "reachable"): 8,
        ("tcp-pnet", "reachable"): 8, ("tcp-pnet", "policy_denied"): 8, ("tcp", "unreachable"): 1,
    } or any(s.version not in (1, 2) or (s.client, s.server) not in (
        ("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge"),
    ) or (s.outcome == "unreachable" and (s.client, s.server, s.version) != ("forge", "go", 1)) for s in specs)):
        return ["canonical AutoNAT matrix must contain exactly 41 unique cases"]
    if (not isinstance(records, list) or len(records) != 41
            or any(not isinstance(r, dict) or not isinstance(r.get("scenario_id"), str) for r in records)
            or {r["scenario_id"] for r in records} != set(expected)):
        return ["AutoNAT receipt must cover all 41 cases exactly once, including DENY and Go negative"]
    errors, claims = [], set()
    for record in records:
        spec = expected[record["scenario_id"]]
        try:
            errors.extend(f"{spec.identifier}: {error}" for error in validate_pair(
                record, spec, root, binaries, load_indexed_json, fingerprint, pnet_key))
        except (ValueError, TypeError, KeyError, IndexError, OSError, RuntimeError) as error:
            errors.append(f"{spec.identifier}: {error}")
        name = acceptance_id(spec)
        expected_claim = [] if name is None else [name]
        if record.get("acceptance_scenario_ids") != expected_claim:
            errors.append(f"{spec.identifier}: receipt claims a different Forge role or duplicate capability")
        if name:
            claims.add((SCENARIOS[name][0], name, f"{spec.client}_to_{spec.server}"))
    required_claims = {(capability, name, direction) for (capability, name), value in required.items()
                       for direction in value[0]}
    if claims != required_claims:
        errors.append("AutoNAT role claims differ from exact manifest requirements")
    return errors
