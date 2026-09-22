"""Real interface transitions with captured, epoch-bound lifecycle receipts."""

from __future__ import annotations

import secrets
import json
import time

from mdns_evidence import PUBLIC_SERVICE, validate_mdns_evidence
from mdns_network import IsolatedMdnsNetwork
from mdns_phase import Phase, read_receipt
from mdns_staged_evidence import churn_sequence, lifecycle_receipt


def capture_stage(phase, role, stage, expected, samples):
    until = time.monotonic() + phase.remaining(25)
    owner = phase.owners[role]
    while time.monotonic() < until:
        if len(samples) >= 180:
            raise ValueError("snapshot count limit exceeded")
        epoch = len(samples) + 1
        path = phase.work / "snapshot.json"
        if path.exists():
            raw, previous = read_receipt(path, limit=16384)
            if not samples or previous.get("epoch") != epoch - 1 or raw != samples[-1]["raw"]:
                raise ValueError("unsolicited lifecycle receipt before request")
        requested = time.monotonic()
        control = phase.work / "control"
        temporary = phase.work / "control.tmp"
        temporary.write_text(f"{epoch} snapshot\n")
        temporary.replace(control)
        response_deadline = min(until, requested + 3)
        while time.monotonic() < response_deadline:
            if owner.process.poll() is not None:
                raise RuntimeError("Forge exited during churn")
            if path.is_file():
                raw, value = read_receipt(path, limit=16384)
                received_epoch = value.get("epoch")
                if type(received_epoch) is not int or received_epoch > epoch or received_epoch < epoch - 1:
                    raise ValueError("unsolicited/replayed lifecycle epoch")
                if received_epoch == epoch:
                    lifecycle_receipt(value, epoch=epoch, pid=owner.process.pid,
                                      peer=phase.ready[role]["local_peer_id"])
                    received = time.monotonic()
                    saved = phase.work / f"snapshot-{epoch:03d}.json"
                    saved.write_text(raw)
                    samples.append({"stage": stage, "epoch": epoch, "requested_at": requested,
                                    "received_at": received, "raw": raw, "receipt": value,
                                    "evidence_file": str(saved)})
                    break
            time.sleep(0.02)
        else:
            raise TimeoutError("lifecycle epoch response deadline")
        if value["mdns_observations"] == expected:
            return
        time.sleep(0.2)
    raise TimeoutError(f"churn did not reach {stage} observation count {expected}")


def run_case_churn(family, binaries, root, *, command_attempt):
    name = f"mdns-churn-ipv{family}-forge-dialer"
    artifact = {"scenario_id": name, "suite": "mdns", "status": "failed", "errors": [],
                "cleanup_errors": [], "acceptance_scenario_ids": [], "snapshots": [], "transitions": [],
                "proof_scope": "same_owned_process_and_identity_interface_down_up_observation_1_0_1"}
    network = IsolatedMdnsNetwork(family)
    phase = None
    payload = secrets.token_hex(32)
    try:
        network.setup()
        phase = Phase(network, root / name, name, command_attempt)
        with phase:
            phase.launch("server", "go", binaries["go"], "tcp-tls", PUBLIC_SERVICE, payload)
            phase.launch("client", "forge", binaries["forge"], "tcp-tls", PUBLIC_SERVICE, payload, staged=True)
            provisional = phase.wait_file(phase.work / "client.json", 40)
            if provisional.get("status") != "ok" or provisional.get("failure") is not None:
                raise ValueError("Forge did not complete initial echo")
            capture_stage(phase, "client", "initial", 1, artifact["snapshots"])
            for stage, up, count in (("down", False, 0), ("up", True, 1)):
                transition = {"up": up, "began_at": time.monotonic(), "checked": False}
                artifact["transitions"].append(transition)
                network.set_interface_up("client", up)
                transition.update(finished_at=time.monotonic(), checked=True)
                capture_stage(phase, "client", stage, count, artifact["snapshots"])
            churn_sequence(artifact["snapshots"], artifact["transitions"],
                           pid=phase.owners["client"].process.pid, peer=phase.ready["client"]["local_peer_id"])
            phase.stop()
    except Exception as error:
        artifact["errors"].append(f"{type(error).__name__}: {error}")
    finally:
        if phase is not None:
            artifact["phase"] = phase.record
            artifact["errors"].extend(phase.record["errors"])
            artifact["cleanup_errors"].extend(phase.record["cleanup_errors"])
            if artifact["snapshots"]:
                try:
                    raw, final_snapshot = read_receipt(phase.work / "snapshot.json", limit=16384)
                    if raw != artifact["snapshots"][-1]["raw"]:
                        raise ValueError("lifecycle receipt changed after last captured epoch")
                    path = phase.work / "snapshot-post-join.json"
                    path.write_text(raw)
                    artifact["post_join_lifecycle"] = {"raw": raw, "receipt": final_snapshot,
                                                       "evidence_file": str(path)}
                except Exception as error:
                    artifact["errors"].append(f"post-join lifecycle: {error}")
        artifact["cleanup_errors"].extend(network.close())
        artifact["network"] = network.evidence()
        if phase is not None and phase.work.is_dir():
            path = phase.work / "network.json"
            path.write_text(json.dumps(artifact["network"], indent=2) + "\n")
            artifact["network"] = {"result_file": str(path), **artifact["network"]}
    if phase is not None and set(phase.final) == {"client", "server"}:
        artifact["errors"].extend(validate_mdns_evidence(
            phase.final["client"], phase.final["server"], client_impl="forge", server_impl="go",
            transport="tcp-tls", family=family, client_ready=phase.ready["client"],
            server_ready=phase.ready["server"], network=artifact["network"], payload=payload.encode(),
            private_fingerprint=None, client_terminal_status=phase.owners["client"].terminal_status,
            server_terminal_status=phase.owners["server"].terminal_status))
    else:
        artifact["errors"].append("missing post-join churn receipts")
    if not artifact["errors"] and not artifact["cleanup_errors"]:
        artifact["status"] = "passed"
    return artifact


def run_suite(binaries, root, *, command_attempt):
    for family in (4, 6):
        yield run_case_churn(family, binaries, root, command_attempt=command_attempt)
