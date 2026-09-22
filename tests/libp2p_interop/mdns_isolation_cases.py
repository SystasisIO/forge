"""Namespace isolation with explicit quiet completion and same-network control."""

from __future__ import annotations

import hashlib
import re
import secrets
import time

from mdns_cases import Case, private_network_fingerprint, run_case
from mdns_evidence import PUBLIC_SERVICE
from mdns_network import IsolatedMdnsNetwork
from mdns_phase import Phase
from mdns_staged_evidence import quiet_receipt


QUIET_SECONDS = 3.2


def case_specs():
    return tuple((mode, forge_role, family) for mode in ("mismatched-psk", "public-private")
                 for forge_role in ("client", "server") for family in (4, 6))


def run_case_isolation(mode, forge_role, family, binaries, root, *, pnet_key, mismatch_key,
                       wait_json, command_attempt):
    name = f"mdns-isolation-ipv{family}-{mode}-forge-{forge_role}"
    artifact = {"scenario_id": name, "suite": "mdns", "status": "failed", "errors": [],
                "cleanup_errors": [], "acceptance_scenario_ids": [],
                "proof_scope": "bounded_quiet_observed_counters_then_same_network_authenticated_control",
                "limitations": ["Go donor detached notifications are not joinable",
                                "Forge service error diagnostics are unavailable"]}
    network = IsolatedMdnsNetwork(family)
    work = root / name
    phase = None
    try:
        work.mkdir(parents=True, exist_ok=False)
        fingerprint = private_network_fingerprint(pnet_key)
        service = f"_p2p-{fingerprint}._udp.local"
        if mode not in ("mismatched-psk", "public-private") or forge_role not in ("client", "server"):
            raise ValueError("unsupported isolation case")
        if mode == "mismatched-psk":
            with mismatch_key.open("rb") as handle:
                mismatch = handle.read(1025)
            with pnet_key.open("rb") as handle:
                canonical = handle.read(1025)
            if len(mismatch) > 1024 or not mismatch or mismatch == canonical:
                raise ValueError("mismatch key must be a distinct bounded fixture")
            artifact["mismatch_key_sha256"] = hashlib.sha256(mismatch).hexdigest()
        network.setup()
        artifact["network_before"] = network.evidence()
        phase = Phase(network, work / "quiet", name, command_attempt, timeout=40)
        implementations = {role: "forge" if role == forge_role else "go" for role in ("client", "server")}
        with phase:
            for role in ("server", "client"):
                impl = implementations[role]
                private = impl == "forge" or mode == "mismatched-psk"
                key = pnet_key if impl == "forge" else mismatch_key if private else None
                expected = service if impl == "forge" else None if private else PUBLIC_SERVICE
                phase.launch(role, impl, binaries[impl], "tcp-pnet" if private else "tcp-tls",
                             expected, secrets.token_hex(32), key=key, quiet=True)
            services = [phase.ready[role]["service_name"] for role in ("client", "server")]
            if services[0] == services[1] or any(
                    value != PUBLIC_SERVICE and not re.fullmatch(r"_p2p-[0-9a-f]{32}\._udp\.local", value)
                    for value in services):
                raise ValueError("isolation requires distinct valid advertised namespaces")
            began = time.monotonic()
            artifact["quiet_began_at"] = began
            while time.monotonic() - began < QUIET_SECONDS:
                phase.remaining()
                phase.require_alive()
                time.sleep(0.02)
            phase.require_alive()
            artifact["quiet_stop_at"] = time.monotonic()
            phase.stop()
        artifact["quiet"] = phase.record
        if phase.record["errors"] or phase.record["cleanup_errors"]:
            raise ValueError("quiet phase did not finish cleanly")
        for role, impl in implementations.items():
            quiet_receipt(phase.final[role], phase.ready[role], impl,
                          "listener" if role == "server" else "dialer",
                          phase.ready[role]["service_name"], phase.owners[role].terminal_status)
        if artifact["quiet_stop_at"] - artifact["quiet_began_at"] < 3:
            raise ValueError("quiet window shorter than three seconds")
        # No setup/teardown between phases: only key/namespace configuration is
        # matched, with fresh fixture processes and ordinary authenticated echo.
        control = run_case(Case(implementations["client"], implementations["server"], "tcp-pnet", family),
                           binaries, work / "control", pnet_key=pnet_key, wait_json=wait_json,
                           command_attempt=command_attempt, supplied_network=network)
        artifact["positive_control"] = control
        if control["status"] != "passed":
            raise ValueError("same-network positive control failed")
        artifact["status"] = "passed"
    except Exception as error:
        artifact["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        if phase is not None:
            artifact["quiet"] = phase.record
        artifact["cleanup_errors"].extend(network.close())
        artifact["network"] = network.evidence()
        if (artifact["network"]["state"] != "closed" or artifact["network"]["cleanup_failures"]
                or artifact["network"]["cleanup_uncertainty"]):
            artifact["cleanup_errors"].append("isolation network cleanup incomplete")
        if artifact["cleanup_errors"]:
            artifact["status"] = "failed"
    return artifact


def run_suite(binaries, root, **dependencies):
    for mode, role, family in case_specs():
        yield run_case_isolation(mode, role, family, binaries, root, **dependencies)
