"""Separate PR10 cases using runner adapters and the existing process owner."""

from dataclasses import dataclass
from pathlib import Path

from process_lifecycle import current_scope, spawn_owned
from private_profile_evidence import CONTRACTS, PRIVATE_CONTRACTS, INLINE_CONTRACTS, PAYLOAD


@dataclass(frozen=True)
class Case:
    contract: str
    dialer: str
    listener: str

    @property
    def private(self):
        return self.contract in PRIVATE_CONTRACTS or self.contract.endswith("_private_pnet")

    @property
    def transport(self):
        tls = CONTRACTS[self.contract][1] == "/tls/1.0.0"
        return ("tcp-pnet-tls" if tls else "tcp-pnet-noise") if self.private else ("tcp-tls" if tls else "tcp")

    @property
    def runner_id(self):
        return ("private_tcp_yamux_pnet/" if self.private else "tcp_stage6/") + self.contract


def case_specs(suite="private-profile"):
    if suite not in ("private-profile", "inline-muxer", "stage6"):
        raise ValueError("unknown private profile suite")
    contracts = PRIVATE_CONTRACTS if suite == "private-profile" else INLINE_CONTRACTS if suite == "inline-muxer" else CONTRACTS
    return tuple(Case(name, source, target) for name in contracts for source, target in
                 (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge"))
                 if (name != "rendezvous_rust_private_tcp_yamux_pnet" or "rust" in (source, target))
                 and (not name.startswith("inline_muxer_go_") or "go" in (source, target))
                 and (not name.startswith("inline_muxer_rust_") or "rust" in (source, target)))


def run_case(spec, binaries, root, *, key, mismatch, fingerprint, start_listener, wait_json,
             command_attempt, run_control, effective_configuration):
    scope = current_scope()
    if scope is None:
        raise RuntimeError("private profile cases require runner.owned_case")
    work = root / f"{spec.contract}-{spec.dialer}-to-{spec.listener}"
    work.mkdir(parents=True, exist_ok=False)
    listener_file = work / "listener-result.json"
    server = start_listener(binaries[spec.listener], spec.listener, work, spec.contract, listener_file,
                            transport=spec.transport, pnet_key_file=key if spec.private else None,
                            pnet_fingerprint=fingerprint if spec.private else None)
    peer, address = server.ready["peer_id"], server.ready["listen_addrs"][0]
    result_file, stop_file = work / "dial-result.json", work / "dial.stop"
    argv = [str(binaries[spec.dialer]), "dial", "--scenario", spec.contract, "--peer-id", peer,
            "--addr", address, "--result-file", str(result_file), "--stop-file", str(stop_file),
            "--store-dir", str(work / "dial-store"), "--transport", spec.transport, "--payload", PAYLOAD.decode()]
    if spec.private:
        argv += ["--pnet-key-file", str(key), "--pnet-fingerprint", fingerprint]
    attempt = command_attempt(argv, work / "dial.log", spec.contract, 1, "dial", 45)
    owner = spawn_owned(argv, work / "dial.log", stop_file, attempt)
    # This is a completion barrier, not the committed result. Both endpoints
    # remain live until the independent listener captured its actual connection.
    pending = wait_json(result_file, 40)
    if pending.get("status") != "ok":
        raise RuntimeError("private application did not complete")
    wait_json(listener_file, 20)
    cleanup = owner.close() + server.close()
    if cleanup:
        raise RuntimeError("private endpoint cleanup failed: " + "; ".join(cleanup))
    result, listener = wait_json(result_file, 5), wait_json(listener_file, 5)
    terminal = owner.terminal_status or {}
    attempt["exit_code"] = terminal.get("exit_code")
    result.update(result_file=str(result_file), attempts=[attempt])
    profile = "private_network" if spec.private else "native"
    stack = ("tcp", "pnet", "yamux") if spec.private else ("tcp", "yamux")
    record = {"dialer": spec.dialer, "listener": spec.listener, "scenario": spec.contract,
              "runner_scenario_id": spec.runner_id, "acceptance_scenario_id": spec.contract,
              "profile": profile, "transport_stack": list(stack), "transport": spec.transport,
              "addr": address, "peer_id": peer, "result": result, "listener_result": listener,
              "listener_result_file": str(listener_file), "listener_process": server.evidence(),
              "selected_addresses": {"dial": address, "listen": server.ready["listen_addrs"]},
              "effective_configuration": effective_configuration(profile, stack, result, server.command, listener)}
    record["listener_process"].update(peer_id=peer, listen_addrs=server.ready["listen_addrs"])
    if spec.private:
        for control, input_key in (("missing_key", None), ("mismatched_key", mismatch)):
            record[control] = run_control(binaries[spec.dialer], spec.dialer, binaries[spec.listener], spec.listener,
                                         work, key, input_key, fingerprint, control, f"{spec.contract}-{control}",
                                         transport=spec.transport)
    return record
