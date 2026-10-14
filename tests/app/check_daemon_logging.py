"""Exercise process logging ownership without relying on static-destructor drain."""

import json
import os
from pathlib import Path
import re
import selectors
import subprocess
import sys
import tempfile
import time


PREFIX = b"@@DAEMON_LOG "
CASE_TIMEOUT = 20
EARLY_FAILURES = {"invalid-cli", "factory-empty", "factory-null", "factory-throw"}
OPERATION_FAILURES = {"config-failure", "startup-failure", "typed-startup-failure", "shutdown-failure"}


def scenario(mode):
    return mode.removeprefix("timeout-").removeprefix("default-")


def default_output(mode):
    return mode.startswith(("default-", "timeout-default-"))


def safe_output(*outputs):
    for output in outputs:
        assert b"fixture-secret-" not in output, "raw argument or exception text leaked into diagnostics"


def console_records(output):
    # The default console uses the existing text formatter with key=value fields.
    return [{"logger": "forge.app.daemon", "fields": dict(re.findall(rb"\b([a-z_]+)=([^\s]*)", line))}
            for line in output.splitlines() if b" forge.app.daemon " in line]


def diagnostic(entries, mode, console=False):
    name = scenario(mode)
    if name not in EARLY_FAILURES | OPERATION_FAILURES:
        return
    fields = [entry["fields"] for entry in entries if entry.get("logger") == "forge.app.daemon"]
    if console:
        fields = [{key.decode(): value.decode() for key, value in item.items()} for item in fields]
    if name in OPERATION_FAILURES:
        matches = [item for item in fields if item.get("event") == "daemon.run.failed" and "error_code" in item]
        assert len(matches) == 1, (mode, fields)
        assert matches[0].get("operation") == "run", matches
        category, code = ("forge", "6") if name == "typed-startup-failure" else ("unknown", "-1")
        assert matches[0].get("error_category") == category and matches[0].get("error_code") == code, matches
    else:
        code, path, operation = {
            "invalid-cli": ("program_options.convert", "daemon.runtime-threads", "config"),
            "factory-throw": ("daemon.bootstrap", "daemon", "bootstrap"),
            "factory-empty": ("daemon.factory", "", "factory"),
            "factory-null": ("daemon.factory", "", "factory"),
        }[name]
        matches = [item for item in fields if item.get("event") == "daemon.config.failed"
                   and item.get("code") == code]
        assert len(matches) == 1, (mode, fields)
        assert "path" in matches[0] and matches[0]["path"] == path, matches
        assert matches[0].get("operation") == operation, matches
        if name == "invalid-cli" and not console:
            assert matches[0].get("reason") == "integer has invalid syntax", matches


def reports(output):
    result = []
    for line in output.splitlines():
        if line.startswith(PREFIX):
            payload = line[len(PREFIX):]
            assert not payload.startswith(b"ERROR"), payload.decode(errors="replace")
            result.append(json.loads(payload))
    return result


def phase(items, name):
    matches = [item for item in items if item["phase"] == name]
    assert len(matches) == 1, (name, items)
    return matches[0]


def drained(item):
    assert item["closed"] == 1 and item["queued"] == 0 and item["bytes"] == 0, item
    assert item["accepted"] > 0 and item["accepted"] == item["delivered"], item
    assert item["failed"] == 0 and item["dropped"] == 0 and item["premature"] == 0, item


def records(path):
    return [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]


def marker(entries, message):
    matches = [record for record in entries if record["message"] == message]
    assert len(matches) == 1, (message, [record["message"] for record in entries])
    if message != "seed":
        assert matches[0]["fields"]["closed"] == "false", matches[0]


def owning_result(mode, items, path):
    name = scenario(mode)
    item = phase(items, "DAEMON_RETURN")
    expected_code = {
        "nonzero": 17,
        "config-failure": 1,
        "startup-failure": 1,
        "typed-startup-failure": 1,
        "shutdown-failure": 1,
        "invalid-cli": 1,
        "factory-empty": 1,
        "factory-null": 1,
        "factory-throw": 1,
        "delayed": 1,
        "nonstd": -1,
    }.get(name, 0)
    assert item["code"] == expected_code and item["threw"] == (name == "nonstd"), item
    drained(item)
    assert item["factory"] == (name != "factory-empty") and item["waiter"] == (name != "nonzero"), item
    assert item["destroyed"] == (name not in EARLY_FAILURES), item
    if default_output(mode):
        assert name in EARLY_FAILURES and item["configured"] == 0 and item["started"] == 0, item
        assert not path.exists(), "fresh default case unexpectedly configured a file sink"
        return
    entries = records(path)
    marker(entries, "seed")
    if name != "factory-empty":
        marker(entries, "factory.destroy/main")
    if name != "nonzero":
        marker(entries, "waiter.destroy/main")
    if name not in EARLY_FAILURES:
        marker(entries, "app.destroy/main")
    diagnostic(entries, mode)
    if mode in {"normal", "nonzero", "startup-failure", "typed-startup-failure", "shutdown-failure", "nonstd", "delayed"}:
        assert item["started"] == 1 and item["cleaned"] == 1, item
        marker(entries, "cleanup.final/main")
    if mode in {"help", "check", "print", "generate", "invalid-cli"}:
        assert item["started"] == 0, item
    if mode in {"help", "print", "generate", "invalid-cli"}:
        assert item["configured"] == 0, item
    if mode == "check":
        assert item["configured"] == 1, item
    if mode == "generate":
        assert (path.parent / "config.yml").is_file()


class ControlReader:
    """Bounded OS reads, including partial lines, without blocking readline()."""

    def __init__(self, process):
        self.process = process
        self.output = bytearray()
        self.pending = bytearray()
        self.items = []

    def until(self, name):
        deadline = time.monotonic() + CASE_TIMEOUT
        with selectors.DefaultSelector() as selector:
            selector.register(self.process.stdout, selectors.EVENT_READ)
            while True:
                matches = [item for item in self.items if item["phase"] == name]
                if matches:
                    assert len(matches) == 1, (name, self.items)
                    return matches[0]
                remaining = deadline - time.monotonic()
                assert remaining > 0 and selector.select(remaining), ("control deadline", name, self.items)
                chunk = os.read(self.process.stdout.fileno(), 65536)
                assert chunk, ("control channel ended", name, self.items)
                self.output.extend(chunk)
                self.pending.extend(chunk)
                assert len(self.pending) <= 1024 * 1024, "unbounded control line"
                while b"\n" in self.pending:
                    line, _, rest = self.pending.partition(b"\n")
                    self.pending = bytearray(rest)
                    self.items.extend(reports(line))


def stop(process, command):
    if process.poll() is None:
        try:
            # Also drains stderr: a logging worker must be released before its
            # process owner can join, even when a parent-side assertion failed.
            process.communicate(command, timeout=8)
        except (subprocess.TimeoutExpired, BrokenPipeError):
            process.kill()
            process.communicate(timeout=5)


def ordinary(binary, mode, path):
    completed = subprocess.run([binary, mode, str(path)], capture_output=True, timeout=CASE_TIMEOUT)
    name = scenario(mode)
    expected_status = 17 if name == "nonzero" else 1 if name in EARLY_FAILURES | OPERATION_FAILURES else 0
    assert completed.returncode == expected_status, (mode, completed.returncode, completed.stdout, completed.stderr)
    safe_output(completed.stdout, completed.stderr)
    if default_output(mode):
        diagnostic(console_records(completed.stderr), mode, console=True)
    else:
        assert completed.stderr == b"", ("configured JSONL daemon wrote directly to stderr", mode, completed.stderr)
        safe_output(path.read_bytes())
    items = reports(completed.stdout)
    if mode in {"runner-reference", "runner-owned", "concurrent"}:
        opened = phase(items, "EMBEDDED_OPEN")
        assert opened["closed"] == 0 and opened["premature"] == 0, opened
        drained(phase(items, "FINAL"))
        entries = records(path)
        marker(entries, "app.destroy/main")
        if mode == "concurrent":
            marker(entries, "concurrent.still.running/second")
            marker(entries, "app.destroy/second")
            marker(entries, "app.destroy/direct")
        else:
            marker(entries, "embedded.after.runner/main")
    else:
        owning_result(mode, items, path)


def delayed(binary, mode, path):
    process = subprocess.Popen([binary, mode, str(path)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        reader = ControlReader(process)
        held = reader.until("CLEANUP_HELD")
        assert held["closed"] == 0 and held["cleaned"] == 0 and held["destroyed"] == 0, held
        assert process.poll() is None, "cleanup barrier did not hold the process"
        if mode == "delayed":
            assert not any(item["phase"] == "DAEMON_RETURN" for item in reader.items), reader.items
        else:
            returned = reader.until("EXTERNAL_RETURN")
            assert returned["code"] == 1 and returned["closed"] == 0, returned
            assert returned["cleaned"] == 0 and returned["destroyed"] == 0, returned
        stdout, stderr = process.communicate(b"release\n", timeout=CASE_TIMEOUT)
        assert stderr == b"", ("configured JSONL daemon wrote directly to stderr", mode, stderr)
        safe_output(stdout, path.read_bytes())
        items = reports(bytes(reader.output) + stdout)
        assert process.returncode == (1 if mode == "delayed" else 0), (process.returncode, items, stderr)
        if mode == "delayed":
            owning_result(mode, items, path)
        else:
            drained(phase(items, "FINAL"))
            entries = records(path)
            for message in (
                "factory.destroy/main", "waiter.destroy/main", "cleanup.final/main", "app.destroy/main",
                "factory.destroy/second", "waiter.destroy/second", "app.destroy/second",
                "external.after.second/second", "external.after.tail/main",
            ):
                marker(entries, message)
    finally:
        stop(process, b"release\n")


def blocked(binary, mode, path):
    process = subprocess.Popen([binary, mode, str(path)], stdin=subprocess.PIPE,
                               stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        reader = ControlReader(process)
        returned = reader.until("DAEMON_RETURN")
        name = scenario(mode)
        nonstd = name == "nonstd"
        expected_code = -1 if nonstd else 17 if name == "nonzero" else 1
        assert returned["code"] == expected_code and returned["threw"] == nonstd, returned
        assert returned["closed"] == 1 and returned["queued"] > 0 and returned["bytes"] > 0, returned
        assert returned["accepted"] > returned["delivered"] + returned["failed"], returned
        assert returned["elapsed_ms"] < 3000 and returned["premature"] == 0, returned
        assert returned["log_failures"] > 0, "drain failure was not counted"
        assert returned["factory"] == (name != "factory-empty") and returned["waiter"] == (name != "nonzero"), returned
        assert returned["pipe_bytes"] > 0 and returned["failed"] == 0 and returned["dropped"] == 0, returned
        assert returned["destroyed"] == (name not in EARLY_FAILURES), returned
        if name in EARLY_FAILURES:
            assert returned["configured"] == 0 and returned["started"] == 0 and returned["cleaned"] == 0, returned
        elif name == "config-failure":
            assert returned["configured"] == 1 and returned["started"] == 0, returned
        elif name in {"startup-failure", "typed-startup-failure", "shutdown-failure"}:
            assert returned["configured"] == 1 and returned["started"] == 1 and returned["cleaned"] == 1, returned
        if default_output(mode):
            assert returned["configured"] == 0 and returned["started"] == 0 and not path.exists(), returned
        # The child fills this OS pipe to EAGAIN before invoking the daemon.
        # Read only stdout until return, then release the worker before final join.
        stdout, stderr = process.communicate(b"drain\n", timeout=CASE_TIMEOUT)
        items = reports(bytes(reader.output) + stdout)
        recovered = phase(items, "RECOVERED")
        drained(recovered)
        assert recovered["accepted"] == returned["accepted"], "recovery admitted extra records"
        assert len(stderr) > returned["pipe_bytes"], "no diagnostics followed the actual full pipe"
        assert stderr[:returned["pipe_bytes"]] == b"x" * returned["pipe_bytes"], "pipe padding changed"
        output = stderr[returned["pipe_bytes"]:]
        safe_output(bytes(reader.output), stdout, output)
        diagnostic(console_records(output), mode, console=True)
        assert b"Forge logging failed" not in stderr, "finalizer used emergency stderr"
        expected_status = 0 if nonstd else expected_code
        assert process.returncode == expected_status, (process.returncode, items)
    finally:
        stop(process, b"drain\n")


def main():
    binary = sys.argv[1]
    cases = (
        "normal", "nonzero", "config-failure", "startup-failure", "shutdown-failure", "nonstd",
        "help", "check", "print", "generate", "invalid-cli", "runner-reference", "runner-owned", "concurrent",
        "delayed", "external-delayed", "timeout", "timeout-nonstd", "timeout-nonzero", "timeout-zero",
        "factory-empty", "factory-null",
        "timeout-config-failure", "timeout-startup-failure", "timeout-shutdown-failure",
        "timeout-invalid-cli", "timeout-factory-empty", "timeout-factory-null",
        "default-invalid-cli", "default-factory-empty", "default-factory-null", "default-factory-throw",
        "timeout-default-invalid-cli", "timeout-default-factory-empty", "timeout-default-factory-null",
        "timeout-default-factory-throw",
        "typed-startup-failure", "timeout-typed-startup-failure",
    )
    with tempfile.TemporaryDirectory(prefix="forge-daemon-logging-") as directory:
        for mode in cases:
            if os.name == "nt" and (mode in {"delayed", "external-delayed"} or mode.startswith("timeout")):
                print(f"SKIP {mode}: interactive fixture uses POSIX pipe polling", flush=True)
                continue
            path = Path(directory) / mode / "events.jsonl"
            path.parent.mkdir()
            if mode in {"delayed", "external-delayed"}:
                delayed(binary, mode, path)
            elif mode.startswith("timeout"):
                blocked(binary, mode, path)
            else:
                ordinary(binary, mode, path)
            print(f"PASS {mode}", flush=True)


if __name__ == "__main__":
    main()
