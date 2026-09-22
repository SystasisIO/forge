"""Exact MDNS38 replay under the common clean-head provenance/index checker.

Registration is not production support. Network replay never executes commands.
"""

from pathlib import Path
import hashlib
import math
import re

from mdns_cases import Case, case_specs, private_network_fingerprint
from mdns_evidence import PUBLIC_SERVICE, _terminal, decode_receipt, validate_mdns_evidence
from mdns_isolation_cases import case_specs as isolation_specs
from mdns_network import IsolatedMdnsNetwork
from isolated_network import CommandResult
from mdns_staged_evidence import churn_sequence, quiet_receipt
from stage6_evidence_contract import evidence_contract_for


SCENARIOS = {
    "mdns_public": ("discovery.mdns_public", "tcp_stage6/mdns_public"),
    "mdns_private_fingerprinted_go": ("discovery.mdns_private_fingerprinted",
                                      "private_tcp_yamux_pnet/mdns_private_fingerprinted_go"),
}
EVIDENCE_CONTRACTS = {evidence_contract_for(name) for name in SCENARIOS}


def expected_cases():
    result = {s.identifier: ("positive", s) for s in case_specs()}
    result.update({f"mdns-isolation-ipv{family}-{mode}-forge-{role}": ("isolation", (mode, role, family))
                   for mode, role, family in isolation_specs()})
    result.update({f"mdns-churn-ipv{family}-forge-dialer": ("churn", family) for family in (4, 6)})
    return result


def claims_for(record):
    entry = expected_cases().get(record.get("scenario_id"))
    if entry is None or entry[0] != "positive" or entry[1].family != 4:
        return []
    transport = entry[1].transport
    return (["mdns_public"] if transport == "tcp" else
            ["mdns_private_fingerprinted_go"] if transport == "tcp-pnet" else [])


def _clean(record):
    if (not isinstance(record, dict) or record.get("status") != "passed" or record.get("suite") != "mdns"
            or record.get("errors") != [] or record.get("cleanup_errors") != []):
        raise ValueError("mDNS case is not cleanly completed")


def replay_network(evidence, family, churn=False, before=None):
    if not isinstance(evidence, dict):
        raise ValueError("missing network evidence")
    payload = {k: v for k, v in evidence.items() if k != "result_file"}
    outer = payload.get("outer_namespace")
    if not isinstance(outer, str) or not re.fullmatch(r"mdns-o-[a-z0-9]{1,20}", outer):
        raise ValueError("invalid mDNS namespace owner")
    records = payload.get("commands")
    if not isinstance(records, list) or not 1 <= len(records) <= 128:
        raise ValueError("missing bounded network transcript")
    for record in records:
        if (not isinstance(record, dict) or set(record) != {"command", "returncode", "stdout", "stderr"}
                or type(record["returncode"]) is not int or record["returncode"] != 0
                or not isinstance(record["command"], list) or not record["command"]
                or any(not isinstance(v, str) or not v for v in record["command"])
                or any(not isinstance(record[k], str) or len(record[k]) > 65536 for k in ("stdout", "stderr"))):
            raise ValueError("failed or malformed network command")
    ip = records[0]["command"][0]
    if not Path(ip).is_absolute() or Path(ip).name != "ip":
        raise ValueError("invalid iproute launcher")
    sysctls = {arg for record in records for arg in record["command"] if Path(arg).name == "sysctl"}
    if len(sysctls) > 1 or any(not Path(p).is_absolute() for p in sysctls):
        raise ValueError("invalid IPv6 sysctl launcher")
    cursor = 0

    def replay(command):
        nonlocal cursor
        if cursor >= len(records) or records[cursor]["command"] != command:
            raise ValueError("network commands differ from owned canonical lifecycle")
        record = records[cursor]
        cursor += 1
        return CommandResult(record["returncode"], record["stdout"], record["stderr"])

    network = IsolatedMdnsNetwork(family, command_runner=replay, system=lambda: "Linux",
        ip_lookup=lambda name: ip if name == "ip" else next(iter(sysctls), None),
        outer_namespace_isolated=lambda: True, namespace_token=outer.removeprefix("mdns-o-"))
    network.setup()
    if before is not None and network.evidence() != before:
        raise ValueError("pre-quiet network differs from the same owned setup")
    if churn:
        network.set_interface_up("client", False)
        network.set_interface_up("client", True)
    if network.close() or cursor != len(records) or network.evidence() != payload:
        raise ValueError("network evidence differs from complete replay")
    return network, ip


def _processes(record, work, implementations, transports, keys, network, ip, binaries, load,
               *, scenario, quiet=False, staged=False, phase=False):
    if not isinstance(record, dict):
        raise ValueError("missing process phase")
    owners, attempts = record.get("owned_processes"), record.get("attempts")
    if (not isinstance(owners, list) or len(owners) != 2 or not isinstance(attempts, list)
            or len(attempts) != 2 or record.get("errors") != [] or record.get("cleanup_errors") != []):
        raise ValueError("phase requires two clean single-attempt owners")
    ready, final, terminals, payloads, pids = {}, {}, {}, {}, {}
    for role, owner, attempt in zip(("server", "client"), owners, attempts):
        impl = implementations[role]
        command = owner["command"]
        if (not isinstance(command, list) or len(command) < 6 or len(command[6:]) % 2
                or command[:6] != [ip, "netns", "exec", network.namespaces[role], str(binaries[impl]),
                                  "listen" if role == "server" else "dial"]):
            raise ValueError("fixture command is not bound to its binary/namespace/role")
        options = dict(zip(command[6::2], command[7::2]))
        payload = options.get("--payload")
        if not isinstance(payload, str) or not re.fullmatch(r"[0-9a-f]{64}", payload):
            raise ValueError("missing independently generated echo challenge")
        expected = {"--scenario": "mdns", "--transport": transports[role], "--payload": payload,
                    "--bind-ip": network.addresses[role][0], "--ready-file": str(work / f"{role}.ready.json"),
                    "--result-file": str(work / f"{role}.json"), "--stop-file": str(work / f"{role}.stop")}
        if keys[role] is not None:
            expected["--pnet-key-file"] = str(keys[role])
        if quiet:
            expected["--mdns-outcome"] = "quiet"
        if staged and impl == "forge":
            expected.update({"--lifecycle-control-file": str(work / "control"),
                             "--lifecycle-receipt-file": str(work / "snapshot.json")})
        if options != expected or len(options) * 2 != len(command[6:]):
            raise ValueError("unexpected/duplicate flags or injected remote coordinates")
        _terminal(owner["terminal_status"])
        if type(owner.get("pid")) is not int or owner["pid"] <= 0:
            raise ValueError("invalid owned process PID")
        pids[role] = owner["pid"]
        log = str(work / f"{role}.log")
        expected_attempt = dict(kind=role, scenario_id=scenario, attempt_id=1, command=command,
            requested_log_file=log, log_file=log, pid=owner["pid"], exit_code=0,
            terminal_status=owner["terminal_status"], timeout_seconds=110 if phase else 155,
            outputs=owner["outputs"])
        if (attempt != expected_attempt or owner.get("log_file") != log
                or set(owner) != {"pid", "command", "log_file", "terminal_status", "ready", "outputs"}):
            raise ValueError("process attempt has missing or unexpected failure evidence")
        outputs = owner["outputs"]
        if not isinstance(outputs, list) or len(outputs) != 2:
            raise ValueError("missing post-join immutable receipts")
        for output, flag in zip(outputs, ("--ready-file", "--result-file")):
            path = f"{log}.{flag[2:]}.json"
            if output != dict(argument=flag, path=options[flag], exists=True, log_file=path):
                raise ValueError("raw output owner/path mismatch")
            value = load(path)
            if flag == "--ready-file":
                ready[role] = value
                if owner["ready"] != value:
                    raise ValueError("readiness differs from launched process")
            else:
                final[role] = value
        if (ready[role] != (record["ready"][role] if phase else record[f"{role}_ready"])
                or final[role] != (record["final"][role] if phase else record[role])):
            raise ValueError("embedded receipt differs from indexed raw output")
        terminals[role], payloads[role] = owner["terminal_status"], payload
    if pids["client"] == pids["server"] or (not quiet and payloads["client"] != payloads["server"]):
        raise ValueError("participants or echo challenges are not independently bound")
    return ready, final, terminals, payloads["client"], pids


def positive(record, spec, root, binaries, load, key):
    _clean(record)
    work = root / spec.identifier
    evidence = record["network"]
    path = str(work / "network.json")
    if evidence.get("result_file") != path or load(path) != {k: v for k, v in evidence.items() if k != "result_file"}:
        raise ValueError("network receipt is not indexed at its case path")
    network, ip = replay_network(evidence, spec.family)
    implementations = dict(client=spec.client, server=spec.server)
    ready, final, terminals, payload, _ = _processes(record, work, implementations,
        dict.fromkeys(implementations, spec.transport), dict.fromkeys(implementations, key if spec.transport == "tcp-pnet" else None),
        network, ip, binaries, load, scenario=spec.identifier)
    fingerprint = private_network_fingerprint(key) if spec.transport == "tcp-pnet" else None
    if (record.get("client_implementation") != spec.client or record.get("server_implementation") != spec.server
            or record.get("transport") != spec.transport or record.get("family") != spec.family
            or record.get("expected_outcome") != "discovered_authenticated_echo"
            or record.get("expected_service_name") != (PUBLIC_SERVICE if fingerprint is None else f"_p2p-{fingerprint}._udp.local")
            or record.get("challenge") != dict(bytes=64, sha256=hashlib.sha256(payload.encode()).hexdigest())):
        raise ValueError("positive case attribution/challenge mismatch")
    errors = validate_mdns_evidence(final["client"], final["server"], client_impl=spec.client, server_impl=spec.server,
        transport=spec.transport, family=spec.family, client_ready=ready["client"], server_ready=ready["server"],
        network=evidence, payload=payload.encode(), private_fingerprint=fingerprint,
        client_terminal_status=terminals["client"], server_terminal_status=terminals["server"])
    if errors:
        raise ValueError("; ".join(errors))
    return network, ip


def validate_suite(records, required, root, binaries, load, key):
    expected = expected_cases()
    if (len(expected) != 38 or not isinstance(records, list) or len(records) != 38
            or any(not isinstance(r, dict) or not isinstance(r.get("scenario_id"), str) for r in records)
            or {r["scenario_id"] for r in records} != set(expected)):
        return ["mDNS requires exactly 38 unique canonical cases (28 positive, 8 isolation, 2 churn)"]
    errors, claims, used = [], set(), set()
    for record in records:
        name = record["scenario_id"]
        kind, spec = expected[name]
        current = set()

        def indexed(path):
            resolved = Path(path).resolve()
            if resolved in used:
                raise ValueError("mDNS cases reuse indexed evidence")
            current.add(resolved)
            return load(path)

        try:
            _clean(record)
            if record.get("acceptance_scenario_ids") != claims_for(record):
                raise ValueError("case claims an unrelated acceptance contract")
            if kind == "positive":
                positive(record, spec, root, binaries, indexed, key)
                for claim in claims_for(record):
                    claims.add((SCENARIOS[claim][0], claim, f"{spec.client}_to_{spec.server}"))
            elif kind == "isolation":
                _isolation(record, spec, root, binaries, indexed, key)
            else:
                _churn(record, spec, root, binaries, indexed)
        except (ValueError, TypeError, KeyError, IndexError, AttributeError, OSError, RuntimeError) as error:
            errors.append(f"{name}: {error}")
        used.update(current)
    if claims != {(capability, name, direction) for (capability, name), value in required.items() for direction in value[0]}:
        errors.append("mDNS claims differ from manifest directions")
    return errors


def _isolation(record, spec, root, binaries, load, key):
    mode, forge_role, family = spec
    work = root / record["scenario_id"]
    implementations = {role: "forge" if role == forge_role else "go" for role in ("client", "server")}
    control_spec = Case(implementations["client"], implementations["server"], "tcp-pnet", family)
    control = record["positive_control"]
    if control.get("scenario_id") != control_spec.identifier or control.get("acceptance_scenario_ids") != []:
        raise ValueError("isolation control identity mismatch")
    network, ip = positive(control, control_spec, work / "control", binaries, load, key)
    if record["network"] != {k: v for k, v in control["network"].items() if k != "result_file"}:
        raise ValueError("positive control used a different network")
    if not isinstance(record.get("network_before"), dict):
        raise ValueError("missing pre-quiet network snapshot")
    replay_network(record["network"], family, before=record["network_before"])
    mismatch = key.with_name("mismatched-swarm.key")
    keys = {role: key if impl == "forge" else mismatch if mode == "mismatched-psk" else None
            for role, impl in implementations.items()}
    transports = {role: "tcp-pnet" if value else "tcp-tls" for role, value in keys.items()}
    ready, final, terminals, _, pids = _processes(record["quiet"], work / "quiet", implementations,
        transports, keys, network, ip, binaries, load, scenario=record["scenario_id"], quiet=True, phase=True)
    if any(pids[role] == owner["pid"] for role in pids for owner in control["owned_processes"]):
        raise ValueError("positive control did not use fresh processes")
    began, stop = record["quiet_began_at"], record["quiet_stop_at"]
    if any(type(v) not in (float, int) or not math.isfinite(v) for v in (began, stop)) or not 3 <= stop - began <= 45:
        raise ValueError("invalid bounded quiet interval")
    service = f"_p2p-{private_network_fingerprint(key)}._udp.local"
    names = [ready[role]["service_name"] for role in implementations]
    if names[0] == names[1]:
        raise ValueError("quiet participants share a namespace")
    for role, impl in implementations.items():
        actual = ready[role]["service_name"]
        if (impl == "forge" and actual != service or impl == "go" and mode == "public-private" and actual != PUBLIC_SERVICE
                or mode == "mismatched-psk" and not re.fullmatch(r"_p2p-[0-9a-f]{32}\._udp\.local", actual)):
            raise ValueError("quiet namespace does not match mode")
        quiet_receipt(final[role], ready[role], impl, "listener" if role == "server" else "dialer", actual, terminals[role])
    if mode == "mismatched-psk" and record.get("mismatch_key_sha256") != hashlib.sha256(mismatch.read_bytes()).hexdigest():
        raise ValueError("mismatched key digest differs from pinned fixture")


def _churn(record, family, root, binaries, load):
    work = root / record["scenario_id"]
    evidence = record["network"]
    if evidence.get("result_file") != str(work / "network.json") or load(evidence["result_file"]) != {
            k: v for k, v in evidence.items() if k != "result_file"}:
        raise ValueError("missing indexed churn network")
    network, ip = replay_network(evidence, family, churn=True)
    ready, final, terminals, payload, pids = _processes(record["phase"], work,
        dict(client="forge", server="go"), dict(client="tcp-tls", server="tcp-tls"),
        dict(client=None, server=None), network, ip, binaries, load, scenario=record["scenario_id"], staged=True, phase=True)
    samples = record["snapshots"]
    churn_sequence(samples, record["transitions"], pid=pids["client"], peer=ready["client"]["local_peer_id"])
    for sample in samples:
        path = str(work / f"snapshot-{sample['epoch']:03d}.json")
        if (sample.get("evidence_file") != path or load(path) != sample["receipt"]
                or decode_receipt(sample["raw"]) != sample["receipt"]):
            raise ValueError("snapshot chronology differs from indexed raw receipt")
        with Path(path).open("rb") as handle:
            raw = handle.read(16385)
        if len(raw) > 16384 or raw.decode("utf-8") != sample["raw"]:
            raise ValueError("snapshot bytes differ from indexed raw capture")
    post = record["post_join_lifecycle"]
    path = str(work / "snapshot-post-join.json")
    if (post != {"raw": samples[-1]["raw"], "receipt": samples[-1]["receipt"], "evidence_file": path}
            or load(path) != post["receipt"]):
        raise ValueError("post-join lifecycle differs from last captured epoch")
    with Path(path).open("rb") as handle:
        raw = handle.read(16385)
    if len(raw) > 16384 or raw.decode("utf-8") != post["raw"]:
        raise ValueError("post-join lifecycle bytes differ from indexed receipt")
    failures = validate_mdns_evidence(final["client"], final["server"], client_impl="forge", server_impl="go",
        transport="tcp-tls", family=family, client_ready=ready["client"], server_ready=ready["server"],
        network=evidence, payload=payload.encode(), private_fingerprint=None,
        client_terminal_status=terminals["client"], server_terminal_status=terminals["server"])
    if failures:
        raise ValueError("; ".join(failures))
