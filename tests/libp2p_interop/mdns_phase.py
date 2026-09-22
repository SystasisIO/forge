"""Small owner for fixture phases on an already-owned isolated mDNS network."""

from __future__ import annotations

import time

from mdns_cases import _snapshot
from mdns_evidence import RESULT_LIMIT, _ready, decode_receipt
from process_lifecycle import enter_scope, exit_scope, spawn_owned


def read_receipt(path, limit=RESULT_LIMIT):
    with path.open("rb") as handle:
        raw = handle.read(limit + 1)
    if len(raw) > limit:
        raise ValueError("oversized staged receipt")
    return raw.decode("utf-8"), decode_receipt(raw.decode("utf-8"))


class Phase:
    def __init__(self, network, work, scenario, command_attempt, timeout=110):
        self.network, self.work, self.scenario = network, work, scenario
        self.command_attempt = command_attempt
        self.deadline = time.monotonic() + timeout
        self.owners, self.ready, self.final = {}, {}, {}
        self.record = {"errors": [], "cleanup_errors": []}

    def __enter__(self):
        self.work.mkdir(parents=True, exist_ok=False)
        self.scope, self.token = enter_scope()
        return self

    def remaining(self, maximum=20):
        result = min(maximum, self.deadline - time.monotonic())
        if result <= 0:
            raise TimeoutError("staged mDNS phase deadline")
        return result

    def launch(self, role, implementation, binary, transport, service, payload, *, key=None, quiet=False, staged=False):
        stop = self.work / f"{role}.stop"
        command = [str(binary), "listen" if role == "server" else "dial", "--scenario", "mdns",
                   "--transport", transport, "--bind-ip", self.network.addresses[role][0],
                   "--payload", payload, "--ready-file", str(self.work / f"{role}.ready.json"),
                   "--result-file", str(self.work / f"{role}.json"), "--stop-file", str(stop)]
        if key is not None:
            command += ["--pnet-key-file", str(key)]
        if quiet:
            command += ["--mdns-outcome", "quiet"]
        if staged:
            command += ["--lifecycle-control-file", str(self.work / "control"),
                        "--lifecycle-receipt-file", str(self.work / "snapshot.json")]
        command = self.network.namespace_command(role, command)
        attempt = self.command_attempt(command, self.work / f"{role}.log", self.scenario, 1, role, 110)
        owner = spawn_owned(command, self.work / f"{role}.log", stop, attempt)
        self.owners[role] = owner
        value = self.wait_file(self.work / f"{role}.ready.json", 20)
        # A mismatched private key's fingerprint comes from its actual fixture;
        # the caller separately requires distinct canonical service namespaces.
        if service is None:
            service = value.get("service_name")
        _ready(value, implementation, "listener" if role == "server" else "dialer", service)
        owner.ready = value
        self.ready[role] = value
        return value

    def wait_file(self, path, timeout):
        until = time.monotonic() + self.remaining(timeout)
        while time.monotonic() < until:
            if path.is_file():
                return read_receipt(path)[1]
            # A completed dialer is allowed after echo; missing files are not.
            if any(owner.process.poll() not in (None, 0) for owner in self.owners.values()):
                raise RuntimeError("fixture failed before receipt")
            time.sleep(0.02)
        raise TimeoutError(f"missing staged receipt: {path.name}")

    def require_alive(self):
        if any(owner.process.poll() is not None for owner in self.owners.values()):
            raise RuntimeError("quiet fixture exited before explicit stop")

    def stop(self):
        # Publish both stops before joining either participant.
        for owner in self.owners.values():
            owner.stop_file.write_text("stop\n")

    def __exit__(self, kind, error, traceback):
        try:
            if error is not None:
                self.record["errors"].append(f"{type(error).__name__}: {error}")
            self.record["cleanup_errors"].extend(self.scope.close())
            self.record["owned_processes"] = self.scope.evidence()
            self.record["attempts"] = self.scope.attempts
            for attempt in self.scope.attempts:
                attempt["exit_code"] = attempt.get("terminal_status", {}).get("exit_code")
            for role, owner in self.owners.items():
                try:
                    self.final[role] = _snapshot(owner, "--result-file")
                    if _snapshot(owner, "--ready-file") != self.ready[role]:
                        raise ValueError("readiness changed after launch")
                except Exception as capture_error:
                    self.record["errors"].append(f"{role}: {capture_error}")
            self.record["ready"], self.record["final"] = self.ready, self.final
        finally:
            exit_scope(self.token)
        return False
