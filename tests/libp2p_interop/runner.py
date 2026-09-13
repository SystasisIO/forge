#!/usr/bin/env python3
import argparse
from functools import wraps
import hashlib
import io
import json
import os
import re
import shutil
import subprocess
import sys
import tarfile
import time
from pathlib import Path
from typing import Optional

sys.dont_write_bytecode = True

from dns_fixture import DnsaddrServer
from autonat_cases import run_suite as run_autonat_suite
from autonat_acceptance import acceptance_id as autonat_acceptance_id, SCENARIOS as AUTONAT_SCENARIOS
from process_lifecycle import Listener, current_scope, enter_scope, exit_scope, spawn_owned, tail_text
from provider_evidence import validate_hidden_find_peer_evidence, validate_provider_evidence
from provenance import (
    WorktreeIdentity,
    fixture_donor_checkout_errors,
    fixture_donor_revision_bindings,
    load_canonical_donor_revisions,
    sha256_file,
    worktree_identity,
)


LIVE_SCENARIO_PROFILES = {
    "quic_base": ("ping", "identify", "relay_reserve", "unknown_protocol"),
    "tcp_noise": ("ping", "identify", "echo", "echo_large"),
    "tcp_tls": ("ping", "identify", "echo"),
    "private_tcp_yamux_pnet": (
        "pnet", "dnsaddr_private_tcp_yamux_pnet",
        "autonat_v1_client_private_tcp_yamux_pnet", "autonat_v1_service_private_tcp_yamux_pnet",
        "autonat_v2_client_private_tcp_yamux_pnet", "autonat_v2_service_private_tcp_yamux_pnet",
    ),
    "tcp_stage6": (
        "dnsaddr", "autonat_v1_client_native_tcp_yamux", "autonat_v1_service_native_tcp_yamux",
        "autonat_v2_client_native_tcp_yamux", "autonat_v2_service_native_tcp_yamux",
    ),
    "quic_stage6": ("autonat_v1_client", "autonat_v1_service", "autonat_v2_client", "autonat_v2_service"),
    "quic_dht": (
        "dht_find_peer",
        "dht_provide_find_provider",
        "dht_pk_put_get",
        "dht_ipns_put_get",
    ),
    "quic_dht_hidden": ("dht_hidden_find_peer",),
    "quic_rendezvous": ("rendezvous_register_discover", "rendezvous_lifecycle"),
    "quic_pubsub": ("gossipsub_publish",),
    "mixed_pubsub": ("gossipsub_mixed_mesh_stress",),
    "quic_topology": ("relay_echo_topology", "dcutr_relay_topology"),
}
SCENARIOS = LIVE_SCENARIO_PROFILES["quic_base"]
DHT_SCENARIOS = LIVE_SCENARIO_PROFILES["quic_dht"]
HIDDEN_DHT_SCENARIO = LIVE_SCENARIO_PROFILES["quic_dht_hidden"][0]
RENDEZVOUS_SCENARIOS = LIVE_SCENARIO_PROFILES["quic_rendezvous"]
PUBSUB_SCENARIOS = LIVE_SCENARIO_PROFILES["quic_pubsub"]
DHT_VALUE_SCENARIOS = ("dht_pk_put_get", "dht_ipns_put_get")
PUBSUB_STRESS_SCENARIO = LIVE_SCENARIO_PROFILES["mixed_pubsub"][0]
TOPOLOGY_SCENARIOS = LIVE_SCENARIO_PROFILES["quic_topology"]
CURRENT_ACCEPTANCE_SCENARIOS = {
    "quic_base/ping": ("ping",),
    "quic_base/identify": ("quic_v1_transport", "identify"),
    "quic_base/relay_reserve": ("relay_v2_client_transport",),
    "tcp_noise/ping": ("ping_native_tcp_yamux",),
    "tcp_noise/identify": ("multistream_select", "noise_identity", "identify_native_tcp_yamux"),
    "tcp_noise/echo": ("tcp_yamux",),
    "tcp_tls/identify": ("tls_identity",),
    "private_tcp_yamux_pnet/pnet": ("pnet",),
    "tcp_stage6/dnsaddr": ("dnsaddr",),
    "private_tcp_yamux_pnet/dnsaddr_private_tcp_yamux_pnet": ("dnsaddr_private_tcp_yamux_pnet",),
    "quic_dht/dht_provide_find_provider": ("kademlia_amino",),
    "quic_rendezvous/rendezvous_register_discover": ("rendezvous_rust",),
}
# TLS Ping is transport smoke, not the separately registered QUIC Ping claim.
UNCLAIMED_SMOKE_SCENARIOS = ("tcp_tls/ping",)
# Executable registration only. Capability support remains staged until the
# clean-head promotion wrapper validates the entire bilateral 41-case suite.
AUTONAT_ACCEPTANCE_SCENARIOS = {
    "quic_stage6/autonat_v1_client": ("autonat_v1_client",),
    "quic_stage6/autonat_v1_service": ("autonat_v1_service",),
    "quic_stage6/autonat_v2_client": ("autonat_v2_client",),
    "quic_stage6/autonat_v2_service": ("autonat_v2_service",),
    "tcp_stage6/autonat_v1_client_native_tcp_yamux": ("autonat_v1_client_native_tcp_yamux",),
    "tcp_stage6/autonat_v1_service_native_tcp_yamux": ("autonat_v1_service_native_tcp_yamux",),
    "tcp_stage6/autonat_v2_client_native_tcp_yamux": ("autonat_v2_client_native_tcp_yamux",),
    "tcp_stage6/autonat_v2_service_native_tcp_yamux": ("autonat_v2_service_native_tcp_yamux",),
    "private_tcp_yamux_pnet/autonat_v1_client_private_tcp_yamux_pnet": ("autonat_v1_client_private_tcp_yamux_pnet",),
    "private_tcp_yamux_pnet/autonat_v1_service_private_tcp_yamux_pnet": ("autonat_v1_service_private_tcp_yamux_pnet",),
    "private_tcp_yamux_pnet/autonat_v2_client_private_tcp_yamux_pnet": ("autonat_v2_client_private_tcp_yamux_pnet",),
    "private_tcp_yamux_pnet/autonat_v2_service_private_tcp_yamux_pnet": ("autonat_v2_service_private_tcp_yamux_pnet",),
}
DIAL_TIMEOUT_SECONDS = 90
PNET_FINGERPRINT_DOMAIN = b"forge.net.pnet.operational-fingerprint.v1\0"
NATIVE_TOPOLOGIES = (
    ("forge", "go", "go"),
    ("go", "forge", "forge"),
    ("forge", "rust", "rust"),
    ("rust", "forge", "forge"),
)
HIDDEN_DHT_PERMUTATIONS = (
    ("forge", "go", "rust"),
    ("go", "forge", "rust"),
    ("rust", "go", "forge"),
)
SUPPORTED_FORGE_BUILD_PROFILES = ("default", "Debug", "Release", "RelWithDebInfo", "MinSizeRel")
LOCKED_FORGE_FIXTURE_COMPILER = {
    "compiler_id": "Clang",
    "compiler_version": "22.1.8",
}
def autonat_claims(spec):
    name = autonat_acceptance_id(spec)
    return [] if name is None else list(
        AUTONAT_ACCEPTANCE_SCENARIOS[f"{AUTONAT_SCENARIOS[name][4]}/{name}"])


def command_option_values(command: object) -> dict[str, str]:
    if not isinstance(command, list) or len(command) < 2:
        return {}
    return {
        command[index]: command[index + 1]
        for index in range(2, len(command), 2)
        if isinstance(command[index], str) and isinstance(command[index + 1], str)
    }


def launcher_execution_description(command: object, _result: object) -> dict[str, object]:
    """Keep only the current registered run's launcher input summary."""
    options = command_option_values(command)
    return {"transport": options.get("--transport")}


def effective_configuration(profile: str, transport_stack: tuple[str, ...], dial_result: dict,
                            listener_command: list[str], listener_result: Optional[dict]) -> dict:
    attempts = dial_result.get("attempts")
    dial_command = attempts[-1].get("command") if isinstance(attempts, list) and attempts else None
    return {
        "activation": "enabled",
        "profile": profile,
        "transport_stack": list(transport_stack),
        "dialer": launcher_execution_description(dial_command, dial_result),
        "listener": launcher_execution_description(listener_command, listener_result),
    }


def canonical_pnet_fingerprint(key_file: Path) -> str:
    """Parse canonical swarm-key input without retaining PSK bytes in artifacts."""
    raw = key_file.read_bytes()
    if raw.endswith(b"\r\n"):
        raw = raw[:-2]
    elif raw.endswith(b"\n"):
        raw = raw[:-1]
    lines = raw.split(b"\n")
    if (
        len(lines) != 3
        or lines[0] != b"/key/swarm/psk/1.0.0/"
        or lines[1] != b"/base16/"
        or len(lines[2]) != 64
        or any(byte not in b"0123456789abcdef" for byte in lines[2])
    ):
        raise RuntimeError(f"pnet fixture key is not canonical: {key_file}")
    decoded = bytes.fromhex(lines[2].decode("ascii"))
    return hashlib.sha256(PNET_FINGERPRINT_DOMAIN + decoded).hexdigest()


def pnet_fixture_paths(source_dir: Path, artifact_root: Path) -> tuple[Path, Path, str]:
    key = (source_dir / "fixtures" / "pnet" / "swarm.key").resolve()
    mismatch = (source_dir / "fixtures" / "pnet" / "mismatched-swarm.key").resolve()
    for path in (key, mismatch):
        if not path.is_file():
            raise RuntimeError(f"pnet fixture key is missing: {path}")
        try:
            path.relative_to(artifact_root.resolve())
        except ValueError:
            continue
        raise RuntimeError("pnet fixture key must remain outside the runner artifact directory")
    return key, mismatch, canonical_pnet_fingerprint(key)


def run(command: list[str], cwd: Optional[Path] = None, env: Optional[dict[str, str]] = None) -> None:
    subprocess.run(command, cwd=cwd, env=env, check=True)


def enabled_from_args(value: str) -> bool:
    env = os.environ.get("FORGE_ENABLE_LIBP2P_INTEROP")
    if env is not None:
        return env in ("1", "ON", "on", "true", "TRUE", "yes", "YES")
    return value in ("1", "ON", "on", "true", "TRUE", "yes", "YES")


def require_tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        raise RuntimeError(f"required tool is missing: {name}")
    return path


def command_output(command: list[str], cwd: Optional[Path] = None,
                   env: Optional[dict[str, str]] = None) -> str:
    return subprocess.check_output(command, cwd=cwd, env=env, text=True).strip()


def tool_identity(name: str, command: list[str], version_pattern: str, expected_version: str,
                  cwd: Optional[Path] = None, env: Optional[dict[str, str]] = None) -> dict:
    path = Path(require_tool(name)).resolve()
    output = command_output([str(path), *command], cwd=cwd, env=env)
    match = re.match(version_pattern, output)
    if not match:
        raise RuntimeError(f"could not parse {name} version output: {output!r}")
    version = match.group("version")
    if version != expected_version:
        raise RuntimeError(f"{name} version must be {expected_version}, got {version}: {output}")
    return {
        "path": str(path),
        "version": version,
        "version_output": output,
    }


def require_toolchain(fixture_lock: dict, source_dir: Path) -> tuple[dict, dict[str, str], dict[str, str]]:
    requirements = fixture_lock.get("toolchains")
    if not isinstance(requirements, dict):
        raise RuntimeError("fixture lock has no toolchain requirements")
    go_requirements = requirements.get("go")
    rust_requirements = requirements.get("rust")
    if not isinstance(go_requirements, dict) or not isinstance(rust_requirements, dict):
        raise RuntimeError("fixture lock toolchain requirements are malformed")
    go_version = go_requirements.get("version")
    rustc_version = rust_requirements.get("rustc_version")
    cargo_version = rust_requirements.get("cargo_version")
    if not all(isinstance(value, str) and value for value in (go_version, rustc_version, cargo_version)):
        raise RuntimeError("fixture lock toolchain versions are malformed")

    go_environment = os.environ.copy()
    go_environment.update({"GOTOOLCHAIN": "local", "GOPROXY": "off", "GOSUMDB": "off"})
    rust_environment = os.environ.copy()
    rust_environment.update({"CARGO_NET_OFFLINE": "true", "RUSTUP_OFFLINE": "true"})
    rust_fixture = source_dir / "rust_fixture"
    tools = {
        "go": tool_identity("go", ["version"], r"^go version go(?P<version>\d+\.\d+\.\d+)\b", go_version,
                            env=go_environment),
        "rustc": tool_identity("rustc", ["--version"], r"^rustc (?P<version>\d+\.\d+\.\d+)\b",
                                rustc_version, cwd=rust_fixture, env=rust_environment),
        "cargo": tool_identity("cargo", ["--version"], r"^cargo (?P<version>\d+\.\d+\.\d+)\b",
                               cargo_version, cwd=rust_fixture, env=rust_environment),
    }
    return tools, go_environment, rust_environment


def fixture_build_info(binary: Path) -> tuple[dict, dict]:
    if not binary.is_file():
        raise RuntimeError(f"Forge interop fixture is missing: {binary}")
    command = [str(binary), "build-info"]
    try:
        value = json.loads(command_output(command))
    except (subprocess.CalledProcessError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Forge interop fixture did not return machine-readable build-info: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError("Forge interop fixture build-info must be an object")
    return value, {"command": command}


def forge_fixture_requirements(fixture_lock: dict) -> dict:
    if fixture_lock.get("schema_version") != 2:
        raise RuntimeError("fixture lock schema_version must be 2")
    toolchains = fixture_lock.get("toolchains")
    if not isinstance(toolchains, dict):
        raise RuntimeError("fixture lock has no toolchain requirements")
    requirements = toolchains.get("forge_fixture")
    expected_fields = {"compiler_id", "compiler_version", "build_profiles"}
    if not isinstance(requirements, dict) or set(requirements) != expected_fields:
        raise RuntimeError("fixture lock Forge fixture requirements are malformed")
    if (
        requirements.get("compiler_id") != LOCKED_FORGE_FIXTURE_COMPILER["compiler_id"]
        or requirements.get("compiler_version") != LOCKED_FORGE_FIXTURE_COMPILER["compiler_version"]
        or requirements.get("build_profiles") != list(SUPPORTED_FORGE_BUILD_PROFILES)
    ):
        raise RuntimeError("fixture lock Forge fixture requirements are malformed")
    return requirements


def require_supported_forge_build_profile(build_profile: object, requirements: dict) -> None:
    if not isinstance(build_profile, str) or build_profile not in requirements["build_profiles"]:
        raise RuntimeError(
            "Forge interop fixture build profile is not supported by the locked baseline: "
            f"embedded={build_profile!r}, supported={requirements['build_profiles']!r}"
        )


def require_fixture_provenance(binary: Path, expected: WorktreeIdentity, fixture_lock: dict) -> tuple[dict, dict]:
    requirements = forge_fixture_requirements(fixture_lock)
    build_info, command = fixture_build_info(binary)
    forge = build_info.get("forge")
    compiler = build_info.get("compiler")
    build_profile = build_info.get("build_profile")
    if build_info.get("schema_version") != 2 or not isinstance(forge, dict) or not isinstance(compiler, dict):
        raise RuntimeError(f"Forge interop fixture build-info is incomplete: {build_info}")
    if forge.get("head") != expected.head or forge.get("worktree_sha256") != expected.fingerprint:
        raise RuntimeError(
            "Forge interop fixture provenance does not match the current worktree: "
            f"embedded={forge}, current={expected.as_json()}"
        )
    if forge.get("exact_identity") != expected.as_json()["exact_identity"]:
        raise RuntimeError(f"Forge interop fixture exact identity is malformed: {forge}")
    if not isinstance(forge.get("dirty"), bool):
        raise RuntimeError(f"Forge interop fixture dirty state is malformed: {forge}")
    if forge["dirty"] != expected.dirty:
        raise RuntimeError(
            "Forge interop fixture dirty state does not match the current worktree: "
            f"embedded={forge['dirty']}, current={expected.dirty}"
        )
    for field in ("path", "id", "version"):
        if not isinstance(compiler.get(field), str) or not compiler[field]:
            raise RuntimeError(f"Forge interop fixture compiler identity is incomplete: {compiler}")
    if not Path(compiler["path"]).is_absolute():
        raise RuntimeError(f"Forge interop fixture compiler path is not absolute: {compiler}")
    if compiler["id"] != requirements["compiler_id"] or compiler["version"] != requirements["compiler_version"]:
        raise RuntimeError(
            "Forge interop fixture compiler does not match the locked baseline: "
            f"embedded={compiler}, required={requirements}"
        )
    require_supported_forge_build_profile(build_profile, requirements)
    return build_info, command


def require_donor(root: Path, name: str) -> Path:
    path = root / name
    if not path.exists():
        raise RuntimeError(f"required donor repository is missing: {path}")
    return path


def load_fixture_lock(source_dir: Path) -> dict:
    lock_path = source_dir / "fixture-lock.json"
    try:
        lock = json.loads(lock_path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"invalid fixture lock {lock_path}: {error}") from error
    if not isinstance(lock, dict) or not isinstance(lock.get("donors"), list):
        raise RuntimeError(f"fixture lock {lock_path} has no donor list")
    return lock


def export_fixture_deps(build_dir: Path, donors_root: Path, fixture_lock: dict, git_tool: str,
                        command_records: list[dict]) -> Path:
    dependencies = build_dir / "fixture-deps"
    if dependencies.exists():
        shutil.rmtree(dependencies)
    dependencies.mkdir(parents=True)
    for donor in fixture_lock["donors"]:
        if not isinstance(donor, dict):
            raise RuntimeError("fixture lock donor entry must be an object")
        name = donor.get("name")
        directory = donor.get("directory")
        commit = donor.get("commit")
        if not all(isinstance(value, str) and value for value in (name, directory, commit)):
            raise RuntimeError(f"invalid fixture lock donor entry: {donor}")
        source = require_donor(donors_root, directory)
        destination = dependencies / directory
        destination.mkdir()
        command = [git_tool, "-C", str(source), "archive", "--format=tar", commit]
        command_records.append({"kind": "donor_archive", "donor": name, "command": command})
        archive = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
        ).stdout
        with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as values:
            members = values.getmembers()
            if any(Path(member.name).is_absolute() or ".." in Path(member.name).parts for member in members):
                raise RuntimeError(f"refusing unsafe donor archive entry for {name}")
            values.extractall(destination, members=members)
    return dependencies


def wait_json(path: Path, timeout: float) -> dict:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.exists():
            return json.loads(path.read_text())
        time.sleep(0.05)
    raise TimeoutError(f"timed out waiting for {path}")


class CaseFailure(RuntimeError):
    def __init__(self, primary: Optional[Exception], cleanup_errors: list[str], artifact: dict):
        self.primary = primary
        self.cleanup_errors = cleanup_errors
        self.artifact = artifact
        details = ([str(primary)] if primary is not None else []) + cleanup_errors
        super().__init__("; ".join(details))


def owned_case(function):
    """Nested scenario helpers share one owner; only the outer case commits."""
    @wraps(function)
    def invoke(*args, **kwargs):
        if current_scope() is not None:
            return function(*args, **kwargs)
        scope, token = enter_scope()
        result = None
        primary = None
        try:
            try:
                result = function(*args, **kwargs)
            except Exception as error:
                primary = error
            finally:
                cleanup_errors = scope.close()
            if primary is not None or cleanup_errors:
                artifact = {
                    "status": "failed",
                    "case_function": function.__name__,
                    "primary_error": str(primary) if primary is not None else None,
                    "cleanup_errors": cleanup_errors,
                    "processes": scope.evidence(),
                    "attempts": scope.attempts,
                }
                if result is not None:
                    artifact["uncommitted_result"] = result
                raise CaseFailure(primary, cleanup_errors, artifact) from primary
            if "result_file" in result:
                # Standalone dial helpers return a raw payload. Keep runner
                # metadata in attempts, which the evidence checker excludes.
                result["attempts"][-1]["owned_processes"] = scope.evidence()
            else:
                result["owned_processes"] = scope.evidence()
            return result
        finally:
            exit_scope(token)
    return invoke


def command_attempt(command: list[str], log_file: Path, scenario: str, attempt_id: int, kind: str,
                    timeout: float) -> dict:
    attempt = {
        "kind": kind,
        "scenario_id": scenario,
        "attempt_id": attempt_id,
        "command": command,
        "requested_log_file": str(log_file),
        "timeout_seconds": timeout,
        "exit_code": None,
    }
    scope = current_scope()
    if scope is None:
        raise RuntimeError("fixture attempt requires an owning scenario")
    scope.attempts.append(attempt)
    return attempt


def run_command_with_attempts(command: list[str], log_file: Path, scenario: str, kind: str, timeout: float,
                              reset_paths: tuple[Path, ...] = ()) -> list[dict]:
    attempts: list[dict] = []
    for attempt_id in (1, 2):
        for path in reset_paths:
            if path.is_dir():
                shutil.rmtree(path)
            elif path.exists():
                path.unlink()
        attempt_log = log_file if attempt_id == 1 else log_file.with_name(f"{log_file.stem}-attempt-{attempt_id}{log_file.suffix}")
        attempt = command_attempt(command, attempt_log, scenario, attempt_id, kind, timeout)
        attempts.append(attempt)
        owned = spawn_owned(command, attempt_log, attempt=attempt)
        process = owned.process
        try:
            attempt["pid"] = process.pid
            try:
                exit_code = process.wait(timeout=timeout)
                attempt["exit_code"] = exit_code
                attempt["log_tail"] = tail_text(attempt_log)
                if exit_code == 0:
                    return attempts
                attempt["failure_class"] = "process_exit"
                raise RuntimeError(
                    f"{kind} exited with {exit_code}; log={attempt_log}; tail={attempt['log_tail']}"
                )
            except subprocess.TimeoutExpired as error:
                attempt["timeout_class"] = "fixture_timeout"
                attempt["log_tail"] = tail_text(attempt_log)
                owned.close()
                attempt["exit_code"] = owned.terminal_status["exit_code"]
                if attempt["exit_code"] is None:
                    raise RuntimeError(
                        f"{kind} timed out after {error.timeout}s; process was not reaped; retry disabled; "
                        f"log={attempt_log}; tail={attempt['log_tail']}"
                    ) from error
                if attempt_id == 1:
                    continue
                raise RuntimeError(
                    f"{kind} timed out after {error.timeout}s; log={attempt_log}; tail={attempt['log_tail']}"
                )
        finally:
            owned.close()
            attempt["exit_code"] = owned.terminal_status["exit_code"]
            attempt["log_tail"] = tail_text(attempt_log)
    return attempts


def run_command_once(command: list[str], log_file: Path, scenario: str, kind: str, timeout: float) -> list[dict]:
    """Negative controls represent one deliberately observed connection attempt."""
    attempt = command_attempt(command, log_file, scenario, 1, kind, timeout)
    owned = spawn_owned(command, log_file, attempt=attempt)
    process = owned.process
    try:
        attempt["pid"] = process.pid
        try:
            attempt["exit_code"] = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            attempt["timeout_class"] = "fixture_timeout"
            owned.close()
            attempt["exit_code"] = owned.terminal_status["exit_code"]
            attempt["log_tail"] = tail_text(log_file)
            raise RuntimeError(f"{kind} timed out after {error.timeout}s; log={log_file}; tail={attempt['log_tail']}") from error
    finally:
        owned.close()
        attempt["exit_code"] = owned.terminal_status["exit_code"]
        attempt["log_tail"] = tail_text(log_file)
    attempt["log_tail"] = tail_text(log_file)
    if attempt["exit_code"] != 0:
        attempt["failure_class"] = "process_exit"
        raise RuntimeError(f"{kind} exited with {attempt['exit_code']}; log={log_file}; tail={attempt['log_tail']}")
    return [attempt]


def attach_attempts(result: dict, attempts: list[dict]) -> dict:
    result["attempts"] = attempts
    return result


def start_listener(binary: Path, implementation: str, work: Path, scenario: Optional[str] = None,
                   result_file: Optional[Path] = None, seed_file: Optional[Path] = None,
                   expected_messages: Optional[int] = None, transport: str = "quic",
                   seed_peer_id: Optional[str] = None, seed_addr: Optional[str] = None,
                   pnet_key_file: Optional[Path] = None, pnet_fingerprint: Optional[str] = None,
                   pnet_control: Optional[str] = None, pnet_correlation: Optional[str] = None) -> Listener:
    ready_file = work / f"{implementation}-ready.json"
    stop_file = work / f"{implementation}.stop"
    log_file = work / f"{implementation}.log"
    store_dir = work / f"{implementation}-store"
    command = [
        str(binary),
        "listen",
        "--ready-file",
        str(ready_file),
        "--stop-file",
        str(stop_file),
        "--store-dir",
        str(store_dir),
        "--features",
        "ping,identify" if transport == "tcp-pnet" else "ping,identify,relay,dcutr,dht,rendezvous,pubsub",
        "--transport",
        transport,
    ]
    if scenario is not None:
        command.extend(["--scenario", scenario])
    if result_file is not None:
        command.extend(["--result-file", str(result_file)])
    if seed_file is not None:
        command.extend(["--seed-file", str(seed_file)])
    if (seed_peer_id is None) != (seed_addr is None):
        raise ValueError("seed peer id and address must be supplied together")
    if seed_peer_id is not None and seed_addr is not None:
        command.extend(["--seed-peer-id", seed_peer_id, "--seed-addr", seed_addr])
    if expected_messages is not None:
        command.extend(["--expected-messages", str(expected_messages)])
    if pnet_key_file is not None:
        command.extend(["--pnet-key-file", str(pnet_key_file)])
    if pnet_fingerprint is not None:
        command.extend(["--pnet-fingerprint", pnet_fingerprint])
    if pnet_control is not None and pnet_correlation is not None:
        command.extend(["--pnet-control", pnet_control, "--pnet-correlation", pnet_correlation])
    owned = spawn_owned(command, log_file, stop_file)
    try:
        owned.ready = wait_json(ready_file, 20)
    except Exception as error:
        raise RuntimeError(f"{implementation} listener did not become ready: {error}; log={log_file}; tail={tail_text(log_file)}")
    return owned


def start_destination(binary: Path, implementation: str, relay_addr: str, relay_peer_id: str, work: Path) -> Listener:
    ready_file = work / f"{implementation}-destination-ready.json"
    stop_file = work / f"{implementation}-destination.stop"
    log_file = work / f"{implementation}-destination.log"
    store_dir = work / f"{implementation}-destination-store"
    command = [
        str(binary),
        "destination",
        "--ready-file",
        str(ready_file),
        "--stop-file",
        str(stop_file),
        "--relay-addr",
        relay_addr,
        "--relay-peer-id",
        relay_peer_id,
        "--store-dir",
        str(store_dir),
    ]
    owned = spawn_owned(command, log_file, stop_file)
    try:
        owned.ready = wait_json(ready_file, 30)
    except Exception as error:
        raise RuntimeError(f"{implementation} destination did not become ready: {error}; log={log_file}; tail={tail_text(log_file)}")
    return owned


@owned_case
def run_dial(binary: Path, implementation: str, scenario: str, peer_id: str, addr: str, work: Path,
             payload: Optional[str] = None, transport: str = "quic", fresh_store_each_attempt: bool = False,
             target_peer_id: Optional[str] = None, pnet_key_file: Optional[Path] = None,
             pnet_fingerprint: Optional[str] = None, dns_server: Optional[str] = None) -> dict:
    payload_suffix = "" if payload is None else f"-{payload}"
    result_file = work / f"{implementation}-dial-{scenario}{payload_suffix}.json"
    log_file = work / f"{implementation}-dial-{scenario}{payload_suffix}.log"
    store_dir = work / f"{implementation}-dial-{scenario}{payload_suffix}-store"
    command = [
        str(binary),
        "dial",
        "--scenario",
        scenario,
        "--peer-id",
        peer_id,
        "--addr",
        addr,
        "--result-file",
        str(result_file),
        "--store-dir",
        str(store_dir),
        "--transport",
        transport,
    ]
    if payload is not None:
        command.extend(["--payload", payload])
    if target_peer_id is not None:
        command.extend(["--target-peer-id", target_peer_id])
    if pnet_key_file is not None:
        command.extend(["--pnet-key-file", str(pnet_key_file)])
    if pnet_fingerprint is not None:
        command.extend(["--pnet-fingerprint", pnet_fingerprint])
    if dns_server is not None:
        command.extend(["--dns-server", dns_server])
    try:
        reset_paths = (store_dir, result_file) if fresh_store_each_attempt else ()
        attempts = run_command_with_attempts(
            command, log_file, scenario, "dial", DIAL_TIMEOUT_SECONDS, reset_paths=reset_paths
        )
    except RuntimeError as error:
        detail = str(error)
        if result_file.exists():
            detail += f"; result={result_file.read_text(errors='replace')}"
        raise RuntimeError(detail)
    result = json.loads(result_file.read_text())
    if result.get("status") != "ok":
        raise RuntimeError(f"{implementation} dial result did not report status=ok: {result}")
    result["result_file"] = str(result_file)
    return attach_attempts(result, attempts)


@owned_case
def run_rejected_pnet_dial(binary: Path, implementation: str, peer_id: str, addr: str, work: Path,
                           control: str, correlation: str, key_file: Optional[Path], fingerprint: str) -> dict:
    result_file = work / f"{implementation}-dial-pnet-{control}.json"
    log_file = work / f"{implementation}-dial-pnet-{control}.log"
    command = [
        str(binary), "dial", "--scenario", "pnet", "--peer-id", peer_id, "--addr", addr,
        "--result-file", str(result_file), "--store-dir", str(work / f"{implementation}-dial-{control}-store"),
        "--transport", "tcp-pnet", "--pnet-fingerprint", fingerprint,
        "--pnet-control", control, "--pnet-correlation", correlation,
    ]
    if key_file is not None:
        command.extend(["--pnet-key-file", str(key_file)])
    attempts = run_command_once(command, log_file, "pnet", "pnet_control", DIAL_TIMEOUT_SECONDS)
    result = json.loads(result_file.read_text())
    if result.get("status") != "rejected":
        raise RuntimeError(f"{implementation} pnet control did not reject: {result}")
    result["result_file"] = str(result_file)
    return attach_attempts(result, attempts)


def listener_evidence(listener: Listener) -> dict:
    return {
        "pid": listener.process.pid,
        "command": listener.command,
        "log_file": str(listener.log_file),
        "terminal_status": listener.terminal_status,
        "peer_id": listener.ready["peer_id"],
        "listen_addrs": listener.ready.get("listen_addrs", []),
    }


@owned_case
def run_hidden_dht_find_peer(binaries: dict[str, Path], seeker: str, routing: str, hidden: str, root: Path) -> dict:
    work = root / f"quic-{seeker}-via-{routing}-to-{hidden}-{HIDDEN_DHT_SCENARIO}"
    work.mkdir(parents=True, exist_ok=True)
    hidden_listener = start_listener(binaries[hidden], f"{hidden}-hidden", work)
    routing_listener = None
    try:
        routing_seed_result = work / f"{routing}-routing-seed.json"
        routing_listener = start_listener(
            binaries[routing],
            f"{routing}-routing",
            work,
            HIDDEN_DHT_SCENARIO,
            routing_seed_result,
            seed_peer_id=hidden_listener.ready["peer_id"],
            seed_addr=hidden_listener.ready["listen_addrs"][0],
        )
        routing_seed = wait_json(routing_seed_result, 30)
        if routing_seed.get("status") != "ok" or routing_seed.get("negotiated_protocol") != "/ipfs/kad/1.0.0":
            raise RuntimeError(f"{routing} did not establish a real Kademlia seed route: {routing_seed}")
        if routing == "forge" and routing_seed.get("seed_route_source") != "explicit_get_providers_wire_reply":
            raise RuntimeError(f"Forge routing listener did not prove its explicit seed RPC: {routing_seed}")

        seeker_result = run_dial(
            binaries[seeker],
            f"{seeker}-seeker",
            HIDDEN_DHT_SCENARIO,
            routing_listener.ready["peer_id"],
            routing_listener.ready["listen_addrs"][0],
            work,
            target_peer_id=hidden_listener.ready["peer_id"],
        )
        if seeker_result.get("preexisting_target") is not False:
            raise RuntimeError(f"{seeker} knew hidden peer before FindPeer: {seeker_result}")
        if seeker_result.get("found_peer") != hidden_listener.ready["peer_id"]:
            raise RuntimeError(f"{seeker} did not find the hidden peer: {seeker_result}")
        if seeker_result.get("negotiated_protocol") != "/ipfs/kad/1.0.0":
            raise RuntimeError(f"{seeker} did not prove the Amino Kademlia protocol: {seeker_result}")
        if seeker == "forge":
            require_hidden_dht_find_peer_evidence(
                seeker_result,
                routing_listener.ready["peer_id"],
                hidden_listener.ready["peer_id"],
            )
        elif seeker_result.get("dht_queries_delta", 0) < 1:
            raise RuntimeError(f"{seeker} FindPeer did not issue a DHT query: {seeker_result}")
        if seeker == "rust":
            for field in (
                "target_dialed",
                "target_connected",
                "routing_updated",
                "followup_exact_rpc",
                "closest_peers",
            ):
                if not seeker_result.get(field):
                    raise RuntimeError(f"Rust seeker did not prove {field}: {seeker_result}")
            if seeker_result.get("address_count", 0) <= 0:
                raise RuntimeError(f"Rust seeker did not learn a routable target address: {seeker_result}")

        return {
            "implementation": "mixed",
            "scenario": HIDDEN_DHT_SCENARIO,
            "transport": "quic",
            "seeker": seeker,
            "routing": routing,
            "hidden": hidden,
            "target_peer_id": hidden_listener.ready["peer_id"],
            "negotiated_protocol": "/ipfs/kad/1.0.0",
            "processes": {
                "hidden": listener_evidence(hidden_listener),
                "routing": listener_evidence(routing_listener),
                "seeker": {
                    "peer_id": seeker_result.get("local_peer_id"),
                    "command": seeker_result["attempts"][-1]["command"],
                    "log_file": seeker_result["attempts"][-1]["log_file"],
                },
            },
            "routing_seed": routing_seed,
            "result": seeker_result,
        }
    finally:
        current_scope().close()


@owned_case
def run_pubsub_mixed_mesh_stress(binaries: dict[str, Path], root: Path) -> dict:
    work = root / PUBSUB_STRESS_SCENARIO
    work.mkdir(parents=True, exist_ok=True)
    seed_file = work / "mesh-seeds.txt"
    expected_messages = 3
    participants = [
        ("forge", "forge0"),
        ("forge", "forge1"),
        ("forge", "forge2"),
        ("forge", "forge3"),
        ("go", "go0"),
        ("go", "go1"),
        ("go", "go2"),
        ("rust", "rust0"),
        ("rust", "rust1"),
        ("rust", "rust2"),
    ]
    listeners: list[Listener] = []
    result_files: list[tuple[str, Path]] = []
    try:
        for implementation, name in participants:
            result_file = work / f"{name}-stress-result.json"
            listeners.append(
                start_listener(
                    binaries[implementation],
                    name,
                    work,
                    PUBSUB_STRESS_SCENARIO,
                    result_file,
                    seed_file,
                    expected_messages,
                )
            )
            result_files.append((name, result_file))

        seed_file.write_text(
            "\n".join(listener.ready["listen_addrs"][0] for listener in listeners) + "\n"
        )
        time.sleep(8)

        publishers = [
            ("forge", "stress-forge", "forge0"),
            ("go", "stress-go", "go0"),
            ("rust", "stress-rust", "rust0"),
        ]
        listeners_by_name = {name: listener for (_, name), listener in zip(participants, listeners)}
        publish_results = []
        for implementation, payload, target_name in publishers:
            target = listeners_by_name[target_name]
            publish_results.append(
                run_dial(
                    binaries[implementation],
                    f"{implementation}-stress-publisher",
                    PUBSUB_STRESS_SCENARIO,
                    target.ready["peer_id"],
                    target.ready["listen_addrs"][0],
                    work,
                    payload,
                )
            )

        time.sleep(8)
    finally:
        current_scope().close()

    listener_results = []
    for name, result_file in result_files:
        result = wait_json(result_file, 10)
        result["name"] = name
        listener_results.append(result)
        if result.get("status") != "ok":
            raise RuntimeError(f"{name} stress listener reported {result}")

    return {
        "implementation": "mixed",
        "scenario": PUBSUB_STRESS_SCENARIO,
        "participants": [
            {
                "name": name,
                "implementation": implementation,
                "peer_id": listener.ready["peer_id"],
                "listen_addr": listener.ready["listen_addrs"][0],
            }
            for (implementation, name), listener in zip(participants, listeners)
        ],
        "publishers": publish_results,
        "listeners": listener_results,
    }


@owned_case
def run_relay_dial(binary: Path, implementation: str, scenario: str, target_peer_id: str, relay_peer_id: str,
                   relay_addr: str, work: Path) -> dict:
    result_file = work / f"{implementation}-relay-dial-{scenario}.json"
    log_file = work / f"{implementation}-relay-dial-{scenario}.log"
    store_dir = work / f"{implementation}-relay-dial-{scenario}-store"
    command = [
        str(binary),
        "dial-relay",
        "--scenario",
        scenario,
        "--peer-id",
        target_peer_id,
        "--relay-peer-id",
        relay_peer_id,
        "--relay-addr",
        relay_addr,
        "--result-file",
        str(result_file),
        "--store-dir",
        str(store_dir),
    ]
    try:
        attempts = run_command_with_attempts(command, log_file, scenario, "relay_dial", 60)
    except RuntimeError as error:
        detail = str(error)
        if result_file.exists():
            detail += f"; result={result_file.read_text(errors='replace')}"
        raise RuntimeError(detail)
    result = json.loads(result_file.read_text())
    result["result_file"] = str(result_file)
    return attach_attempts(result, attempts)


def prepare_go_fixture(source_dir: Path, build_dir: Path, go_tool: str,
                       environment: dict[str, str]) -> tuple[Path, list[dict]]:
    work = build_dir / "go_fixture"
    if work.exists():
        shutil.rmtree(work)
    shutil.copytree(source_dir / "go_fixture", work)
    binary = work / "go_fixture"
    policy = {"GOTOOLCHAIN": "local", "GOPROXY": "off", "GOSUMDB": "off"}
    commands = [
        {"command": [go_tool, "mod", "verify"], "cwd": str(work), "environment": policy},
        {
            "command": [go_tool, "test", "-mod=readonly", "-count=1", "-timeout=60s", "."],
            "cwd": str(work),
            "environment": policy,
        },
        {
            "command": [go_tool, "build", "-mod=readonly", "-trimpath", "-o", str(binary), "."],
            "cwd": str(work),
            "environment": policy,
        },
    ]
    for command in commands:
        run(command["command"], cwd=work, env=environment)
    return binary, commands


def remove_rust_fixture_entry(path: Path) -> None:
    """Remove one refresh entry without ever traversing a symlink."""
    if path.is_symlink() or path.is_file():
        path.unlink()
    elif path.is_dir():
        shutil.rmtree(path)
    else:
        path.unlink(missing_ok=True)


def refresh_rust_fixture_source(source: Path, work: Path) -> None:
    """Refresh copied Rust sources while retaining only a real Cargo target directory."""
    if work.is_symlink():
        raise RuntimeError(f"refusing to refresh Rust fixture through symlinked work directory: {work}")
    if work.exists() and not work.is_dir():
        remove_rust_fixture_entry(work)
    if work.exists():
        target = work / "target"
        retain_target = target.is_dir() and not target.is_symlink()
        for entry in work.iterdir():
            if retain_target and entry == target:
                continue
            remove_rust_fixture_entry(entry)
        shutil.copytree(
            source,
            work,
            dirs_exist_ok=True,
            symlinks=True,
            ignore=shutil.ignore_patterns("target"),
        )
        return
    shutil.copytree(source, work, symlinks=True, ignore=shutil.ignore_patterns("target"))


def prepare_rust_fixture(source_dir: Path, build_dir: Path, cargo_tool: str,
                         environment: dict[str, str]) -> tuple[Path, list[dict]]:
    work = build_dir / "rust_fixture"
    refresh_rust_fixture_source(source_dir / "rust_fixture", work)
    commands = [
        {
            "command": [cargo_tool, "test", "--frozen"],
            "cwd": str(work),
            "environment": {"CARGO_NET_OFFLINE": "true", "RUSTUP_OFFLINE": "true"},
        },
        {
            "command": [cargo_tool, "build", "--release", "--frozen"],
            "cwd": str(work),
            "environment": {"CARGO_NET_OFFLINE": "true", "RUSTUP_OFFLINE": "true"},
        },
    ]
    for command in commands:
        run(command["command"], cwd=work, env=environment)
    return work / "target" / "release" / "forge-libp2p-rust-fixture", commands


def require_rendezvous_register_discover_evidence(result: dict, implementation: str) -> None:
    required = (
        "negotiated_protocol",
        "wire_registration_count",
        "signed_peer_record_valid",
        "matching_peer_record",
        "record_sequence",
        "record_address_count",
        "registered_ttl_seconds",
        "discovered_ttl_seconds",
        "cookie_bytes",
    )
    missing = [field for field in required if field not in result]
    if missing:
        raise RuntimeError(f"{implementation} rendezvous evidence is missing {missing}: {result}")
    if result["negotiated_protocol"] != "/rendezvous/1.0.0":
        raise RuntimeError(f"{implementation} rendezvous protocol mismatch: {result}")
    if result["wire_registration_count"] != 1:
        raise RuntimeError(f"{implementation} rendezvous did not return exactly one wire registration: {result}")
    if result["signed_peer_record_valid"] is not True or result["matching_peer_record"] is not True:
        raise RuntimeError(f"{implementation} rendezvous signed peer record did not validate and match: {result}")
    for field in ("record_sequence", "record_address_count", "cookie_bytes"):
        if type(result[field]) is not int or result[field] <= 0:
            raise RuntimeError(f"{implementation} rendezvous {field} must be a positive integer: {result}")
    for field in ("registered_ttl_seconds", "discovered_ttl_seconds"):
        if result[field] != 7_200:
            raise RuntimeError(f"{implementation} rendezvous {field} must be donor default 7200: {result}")
    if implementation == "forge":
        if result.get("sanitized_registration_count") != 0 or result.get("loopback_filtered") is not True:
            raise RuntimeError(f"Forge rendezvous loopback filtering proof is incomplete: {result}")


def require_rendezvous_lifecycle_evidence(result: dict, dialer: str, listener: str) -> None:
    required = (
        "negotiated_protocol",
        "legacy_signed_peer_record",
        "initial_ttl_seconds",
        "updated_ttl_seconds",
        "renewed_ttl_seconds",
        "initial_record_sequence",
        "updated_record_sequence",
        "renewed_record_sequence",
        "pre_unregister_record_sequence",
        "initial_cookie_bytes",
        "delta_cookie_bytes",
        "cookie_changed",
        "initial_visible_count",
        "updated_visible_count",
        "renewed_visible_after_original_expiry",
        "renewed_visible_count",
        "expired_registration_count",
        "pre_unregister_count",
        "final_registration_count",
    )
    missing = [field for field in required if field not in result]
    if missing:
        raise RuntimeError(f"{dialer} rendezvous lifecycle evidence is missing {missing}: {result}")
    if result["negotiated_protocol"] != "/rendezvous/1.0.0":
        raise RuntimeError(f"{dialer} did not negotiate rendezvous lifecycle protocol: {result}")
    if result["legacy_signed_peer_record"] is not True:
        raise RuntimeError(f"{dialer} did not prove a signed legacy rendezvous record: {result}")

    ttl_fields = ("initial_ttl_seconds", "updated_ttl_seconds", "renewed_ttl_seconds")
    if any(type(result[field]) is not int or not 2 <= result[field] <= 3 for field in ttl_fields):
        raise RuntimeError(f"{listener} did not return bounded lifecycle TTLs with a renewal window: {result}")

    sequence_fields = (
        "initial_record_sequence",
        "updated_record_sequence",
        "renewed_record_sequence",
        "pre_unregister_record_sequence",
    )
    if any(type(result[field]) is not int or result[field] <= 0 for field in sequence_fields):
        raise RuntimeError(f"{dialer} rendezvous lifecycle has an invalid PeerRecord sequence: {result}")
    if not (
        result["initial_record_sequence"]
        < result["updated_record_sequence"]
        < result["renewed_record_sequence"]
        < result["pre_unregister_record_sequence"]
    ):
        raise RuntimeError(f"{dialer} did not advance each rendezvous lifecycle PeerRecord: {result}")

    if (
        type(result["initial_cookie_bytes"]) is not int
        or result["initial_cookie_bytes"] <= 0
        or type(result["delta_cookie_bytes"]) is not int
        or result["delta_cookie_bytes"] <= 0
        or result["cookie_changed"] is not True
    ):
        raise RuntimeError(f"{dialer} did not prove a non-empty changed rendezvous cookie: {result}")

    for field in (
        "initial_visible_count",
        "updated_visible_count",
        "renewed_visible_count",
        "pre_unregister_count",
    ):
        if result[field] != 1:
            raise RuntimeError(f"{dialer} rendezvous lifecycle did not prove one {field}: {result}")
    if result["renewed_visible_after_original_expiry"] is not True:
        raise RuntimeError(f"{dialer} did not prove renewed visibility after original expiry: {result}")
    if result["expired_registration_count"] != 0 or result["final_registration_count"] != 0:
        raise RuntimeError(f"{dialer} rendezvous lifecycle did not expire and unregister: {result}")


def require_dht_provider_evidence(result: dict, dialer: str, listener_peer: str) -> None:
    errors = validate_provider_evidence(result, listener_peer)
    if errors:
        raise RuntimeError(f"{dialer} DHT provider proof is invalid: {'; '.join(errors)}; result={result}")


def require_hidden_dht_find_peer_evidence(result: dict, seed_peer: str, target_peer: str) -> None:
    errors = validate_hidden_find_peer_evidence(result, seed_peer, target_peer)
    if errors:
        raise RuntimeError(f"Forge hidden FindPeer proof is invalid: {'; '.join(errors)}; result={result}")


@owned_case
def run_pair(dialer_binary: Path, dialer: str, listener_binary: Path, listener: str, scenario: str, root: Path,
             acceptance_scenario_id: Optional[str] = None) -> dict:
    runner_profile = "quic_base"
    if scenario in DHT_SCENARIOS:
        runner_profile = "quic_dht"
    elif scenario in RENDEZVOUS_SCENARIOS:
        runner_profile = "quic_rendezvous"
    elif scenario in PUBSUB_SCENARIOS:
        runner_profile = "quic_pubsub"
    return run_pair_with_transport(
        dialer_binary,
        dialer,
        listener_binary,
        listener,
        scenario,
        root,
        "quic",
        "native",
        ("quic",),
        f"{runner_profile}/{scenario}",
        acceptance_scenario_id or scenario,
    )


@owned_case
def run_dht_value_remote_get(binaries: dict[str, Path], writer: str, listener: str, scenario: str,
                             root: Path) -> dict:
    readers = [implementation for implementation in binaries if implementation not in (writer, listener)]
    if len(readers) != 1:
        raise RuntimeError(f"DHT value proof requires one distinct third reader, got {readers}")
    reader = readers[0]
    work = root / f"quic-{writer}-via-{listener}-to-{reader}-{scenario}"
    work.mkdir(parents=True, exist_ok=True)
    listener_result = work / f"{listener}-listen-{scenario}.json"
    server = start_listener(binaries[listener], listener, work, scenario, listener_result)
    try:
        addr = server.ready["listen_addrs"][0]
        peer_id = server.ready["peer_id"]
        publication = run_dial(binaries[writer], writer, scenario, peer_id, addr, work, payload="put_only")
        if publication.get("operation") != "put_only":
            raise RuntimeError(f"{writer} did not report an isolated DHT PUT: {publication}")
        delivered = wait_json(listener_result, 20)
        if delivered.get("status") != "ok" or delivered.get("record_persisted") is not True:
            raise RuntimeError(f"{listener} listener did not persist the DHT value: {delivered}")

        retrieval = run_dial(
            binaries[reader], reader, scenario, peer_id, addr, work, payload="get_only",
            fresh_store_each_attempt=True
        )
        if retrieval.get("operation") != "get_only" or retrieval.get("remote_get") is not True:
            raise RuntimeError(f"{reader} did not report an isolated remote DHT GET: {retrieval}")
        return {
            "writer": writer,
            "listener": listener,
            "reader": reader,
            "scenario": scenario,
            "transport": "quic",
            "addr": addr,
            "peer_id": peer_id,
            "reader_store_reset_each_attempt": True,
            "listener_process": {
                "pid": server.process.pid,
                "command": server.command,
                "log_file": str(server.log_file),
            },
            "publication": publication,
            "listener_result": delivered,
            "retrieval": retrieval,
        }
    finally:
        current_scope().close()


@owned_case
def run_pnet_control(dialer_binary: Path, dialer: str, listener_binary: Path, listener: str, root: Path,
                     key_file: Path, dial_key_file: Optional[Path], fingerprint: str, control: str,
                     correlation: str) -> dict:
    work = root / f"tcp-pnet-{dialer}-to-{listener}-pnet-{control}"
    work.mkdir(parents=True, exist_ok=True)
    listener_result_file = work / f"{listener}-listen-pnet-{control}.json"
    server = start_listener(
        listener_binary, listener, work, "pnet", listener_result_file, transport="tcp-pnet",
        pnet_key_file=key_file, pnet_fingerprint=fingerprint, pnet_control=control,
        pnet_correlation=correlation,
    )
    try:
        result = run_rejected_pnet_dial(
            dialer_binary, dialer, server.ready["peer_id"], server.ready["listen_addrs"][0], work,
            control, correlation, dial_key_file, fingerprint,
        )
    finally:
        server.close()
    listener_result = wait_json(listener_result_file, 20)
    listener_result["result_file"] = str(listener_result_file)
    expected_sources = {
        "forge": "forge.node.metrics",
        "go": "go-libp2p.connection-gater",
        "rust": "rust-libp2p.swarm-events",
    }
    for endpoint, payload in (("dialer", result), ("listener", listener_result)):
        counters = [
            payload.get(name)
            for name in ("attempted_connections", "established_connections", "identify_streams", "application_streams")
        ]
        implementation = payload.get("implementation")
        if implementation not in expected_sources or payload.get("counter_source") != expected_sources[implementation]:
            raise RuntimeError(f"{dialer}->{listener} pnet {control} {endpoint} lacks runtime counter provenance")
        if any(type(value) is not int or value < 0 for value in counters):
            raise RuntimeError(f"{dialer}->{listener} pnet {control} {endpoint} has invalid observed counters")
        rejected_before_identify = counters[1:] == [0, 0, 0]
        if payload.get("rejected_before_identify") is not rejected_before_identify or not rejected_before_identify:
            raise RuntimeError(f"{dialer}->{listener} pnet {control} {endpoint} crossed the session boundary")
    if result["attempted_connections"] < 1:
        raise RuntimeError(f"{dialer}->{listener} pnet {control} did not observe its dial attempt")
    if listener_result["attempted_connections"] < 1:
        raise RuntimeError(f"{dialer}->{listener} pnet {control} did not observe listener ingress")
    return {
        "result": result,
        "listener_process": listener_evidence(server),
        "listener_result": listener_result,
        "listener_result_file": str(listener_result_file),
    }


def require_pnet_dial_evidence(result: dict, implementation: str) -> None:
    security = result.get("negotiated_security")
    if security not in {"/noise", "/tls/1.0.0"}:
        raise RuntimeError(f"{implementation} pnet dial lacks observed Noise/TLS negotiation: {result}")
    if result.get("negotiated_muxer") != "/yamux/1.0.0":
        raise RuntimeError(f"{implementation} pnet dial lacks observed Yamux negotiation: {result}")
    if implementation == "rust":
        for field in ("autonat_v2_active", "relay_service_active", "relay_client_active", "dcutr_active"):
            if result.get(field) is not False:
                raise RuntimeError(f"Rust pnet dial did not prove {field}=false: {result}")


@owned_case
def run_pair_with_transport(dialer_binary: Path, dialer: str, listener_binary: Path, listener: str, scenario: str,
                            root: Path, transport: str, acceptance_profile: str,
                            transport_stack: tuple[str, ...], runner_scenario_id: str,
                            acceptance_scenario_id: Optional[str], pnet_key_file: Optional[Path] = None,
                            pnet_mismatch_key_file: Optional[Path] = None,
                            pnet_fingerprint: Optional[str] = None, dnsaddr: bool = False) -> dict:
    work = root / f"{transport}-{dialer}-to-{listener}-{acceptance_scenario_id or scenario}"
    work.mkdir(parents=True, exist_ok=True)
    if acceptance_profile not in {"native", "private_network"}:
        raise RuntimeError(f"unsupported acceptance profile: {acceptance_profile}")
    pnet_profile = acceptance_profile == "private_network"
    if pnet_profile and (
        scenario != "pnet" or pnet_key_file is None or pnet_mismatch_key_file is None or pnet_fingerprint is None
    ):
        raise RuntimeError("private pnet profile requires its canonical and mismatched source key fixtures")
    tcp_upgrade_listener = (
        acceptance_profile == "native" and transport in {"tcp", "tcp-tls"}
        and listener in {"go", "rust"} and scenario in {"identify", "echo"}
    )
    listener_result = (
        work / f"{listener}-listen-{scenario}.json"
        if pnet_profile or scenario in PUBSUB_SCENARIOS or scenario in DHT_VALUE_SCENARIOS
        or (scenario == "relay_reserve" and listener == "rust") or tcp_upgrade_listener
        else None
    )
    server = start_listener(
        listener_binary,
        listener,
        work,
        scenario,
        listener_result,
        transport=transport,
        pnet_key_file=pnet_key_file,
        pnet_fingerprint=pnet_fingerprint,
    )
    try:
        addr = server.ready["listen_addrs"][0]
        peer_id = server.ready["peer_id"]
        dns_evidence = None
        if dnsaddr:
            dns_path = work / "authoritative-dns.json"
            with DnsaddrServer(addr, peer_id, dns_path) as resolver:
                addr = resolver.root
                result = run_dial(
                    dialer_binary, dialer, scenario, peer_id, addr, work, transport=transport,
                    pnet_key_file=pnet_key_file, pnet_fingerprint=pnet_fingerprint,
                    dns_server=resolver.nameserver,
                )
            resolver.require_chain()
            dns_evidence = {"log_file": str(dns_path), "sha256": sha256_file(dns_path),
                            **json.loads(dns_path.read_text())}
        else:
            result = run_dial(
                dialer_binary, dialer, scenario, peer_id, addr, work, transport=transport,
                pnet_key_file=pnet_key_file, pnet_fingerprint=pnet_fingerprint,
            )
        if pnet_profile:
            require_pnet_dial_evidence(result, dialer)
        if scenario == "identify" and dialer == "go" and listener == "forge":
            if result.get("signed_peer_record") is not True:
                raise RuntimeError("Go libp2p did not receive Forge's signed Identify peer record")
        if scenario == "rendezvous_register_discover":
            require_rendezvous_register_discover_evidence(result, dialer)
        if scenario == "rendezvous_lifecycle":
            require_rendezvous_lifecycle_evidence(result, dialer, listener)
        if scenario == "dht_provide_find_provider":
            require_dht_provider_evidence(result, dialer, peer_id)
        if tcp_upgrade_listener:
            # Upgrade observations are published only after the donor host is joined.
            # Never wait for this final result while keeping the listener alive.
            cleanup_errors = server.close()
            if cleanup_errors:
                raise RuntimeError(f"{listener} TCP listener cleanup failed: " + "; ".join(cleanup_errors))
        delivered = wait_json(listener_result, 20) if listener_result is not None else None
        if tcp_upgrade_listener:
            proof = delivered.get("upgrade_observation")
            if not isinstance(proof, dict) or proof.get("overflow") is not False:
                raise RuntimeError(f"{listener} TCP listener did not finalize bounded upgrade evidence")
            if listener == "go":
                if proof.get("finalized_after_host_close") is not True or proof.get("complete") is not True:
                    raise RuntimeError("Go TCP listener did not finalize complete bounded upgrade evidence")
            else:
                # A listener has no outbound application binding. Its raw counterpart
                # trace is evidence only after the fixture-owned task group is joined.
                lifecycle = delivered.get("fixture_task_lifecycle")
                if (proof.get("source") != "rust-libp2p.public-connection-upgrades.v1"
                        or proof.get("finalized_after_swarm_drop") is not True
                        or proof.get("fixture_owned_tasks_joined") is not True
                        or not isinstance(lifecycle, dict)
                        or lifecycle.get("fixture_owned_tasks_joined") is not True
                        or lifecycle.get("overflow") is not False or lifecycle.get("errors") != []):
                    raise RuntimeError("Rust TCP listener did not finalize its joined raw upgrade evidence")
        if scenario == "relay_reserve" and listener == "rust":
            # First result proves the event was polled; only the joined final trace is evidence.
            cleanup_errors = server.close()
            if cleanup_errors:
                raise RuntimeError("Rust relay listener cleanup failed: " + "; ".join(cleanup_errors))
            delivered = wait_json(listener_result, 20)
            if delivered.get("trace_complete") is not True:
                raise RuntimeError("Rust relay listener did not finalize its acceptance trace")
        if delivered is not None and delivered.get("status") != "ok":
            raise RuntimeError(f"{listener} listener reported {delivered}")
        if pnet_profile and listener == "rust":
            for field in ("autonat_v2_active", "relay_service_active", "relay_client_active", "dcutr_active"):
                if delivered.get(field) is not False:
                    raise RuntimeError(f"Rust pnet listener did not prove {field}=false: {delivered}")
        controls = {}
        if pnet_profile:
            controls = {
                "missing_key": run_pnet_control(
                    dialer_binary, dialer, listener_binary, listener, work, pnet_key_file, None,
                    pnet_fingerprint, "missing_key", f"{dialer}-to-{listener}-missing-key",
                ),
                "mismatched_key": run_pnet_control(
                    dialer_binary, dialer, listener_binary, listener, work, pnet_key_file, pnet_mismatch_key_file,
                    pnet_fingerprint, "mismatched_key", f"{dialer}-to-{listener}-mismatched-key",
                ),
            }
        out = {
            "dialer": dialer,
            "listener": listener,
            "scenario": scenario,
            "runner_scenario_id": runner_scenario_id,
            "acceptance_scenario_id": acceptance_scenario_id,
            "profile": acceptance_profile,
            "transport_stack": list(transport_stack),
            "transport": transport,
            "addr": addr,
            "selected_addresses": {
                "dial": addr,
                "listen": server.ready.get("listen_addrs", []),
            },
            "peer_id": peer_id,
            "listener_process": {
                "pid": server.process.pid,
                "command": server.command,
                "log_file": str(server.log_file),
                "terminal_status": server.terminal_status,
                "peer_id": peer_id,
                "listen_addrs": server.ready.get("listen_addrs", []),
            },
            "result": result,
            "listener_result": delivered,
            "effective_configuration": effective_configuration(
                acceptance_profile,
                transport_stack,
                result,
                server.command,
                delivered,
            ),
        }
        out.update(controls)
        if dns_evidence is not None:
            out["dns_evidence"] = dns_evidence
        if listener_result is not None:
            out["listener_result_file"] = str(listener_result)
        return out
    finally:
        current_scope().close()


def require_local_topology_evidence(result: dict, scenario: str) -> None:
    if result.get("status") != "ok":
        raise RuntimeError(f"Forge topology fixture reported non-success: {result}")
    if scenario == "relay_echo_topology":
        relay_bytes = result.get("relay_bytes")
        if result.get("relay_echo") is not True or type(relay_bytes) is not int or relay_bytes <= 0:
            raise RuntimeError(f"Forge relay topology lacks successful relay evidence: {result}")
        return
    if scenario == "dcutr_relay_topology":
        hole_punch_status = result.get("hole_punch_status")
        hole_punch_successes = result.get("source_hole_punch_successes")
        relay_bytes = result.get("relay_bytes")
        if (
            type(hole_punch_status) is not int
            or hole_punch_status != 3
            or result.get("relay_echo") is not True
            or type(hole_punch_successes) is not int
            or hole_punch_successes <= 0
            or type(relay_bytes) is not int
            or relay_bytes <= 0
        ):
            raise RuntimeError(f"Forge DCUtR topology lacks successful hole-punch and direct-echo evidence: {result}")
        return
    raise RuntimeError(f"unknown Forge topology evidence scenario: {scenario}")


@owned_case
def run_topology(binary: Path, implementation: str, scenario: str, root: Path) -> dict:
    work = root / f"{implementation}-{scenario}"
    work.mkdir(parents=True, exist_ok=True)
    result_file = work / "result.json"
    log_file = work / "topology.log"
    command = [
        str(binary),
        "topology",
        "--scenario",
        scenario,
        "--result-file",
        str(result_file),
        "--store-dir",
        str(work / "stores"),
    ]
    try:
        attempts = run_command_with_attempts(command, log_file, scenario, "topology", 60)
    except RuntimeError as error:
        detail = str(error)
        if result_file.exists():
            detail += f"; result={result_file.read_text(errors='replace')}"
        raise RuntimeError(detail)
    result = json.loads(result_file.read_text())
    result["result_file"] = str(result_file)
    result = attach_attempts(result, attempts)
    require_local_topology_evidence(result, scenario)
    return {
        "implementation": implementation,
        "scenario": scenario,
        "result": result,
    }


@owned_case
def run_native_relay_topology(binaries: dict[str, Path], source: str, relay_impl: str, destination_impl: str,
                              scenario: str, root: Path) -> dict:
    work = root / f"{source}-source-{relay_impl}-relay-{destination_impl}-destination-{scenario}"
    work.mkdir(parents=True, exist_ok=True)
    relay = start_listener(binaries[relay_impl], f"{relay_impl}-relay", work)
    destination = None
    try:
        relay_addr = relay.ready["listen_addrs"][0]
        destination = start_destination(
            binaries[destination_impl],
            destination_impl,
            relay_addr,
            relay.ready["peer_id"],
            work,
        )
        result = run_relay_dial(
            binaries[source],
            source,
            scenario,
            destination.ready["peer_id"],
            relay.ready["peer_id"],
            relay_addr,
            work,
        )
        return {
            "implementation": "mixed",
            "scenario": scenario,
            "source": source,
            "relay_impl": relay_impl,
            "destination_impl": destination_impl,
            "relay": relay.ready,
            "destination": destination.ready,
            "result": result,
        }
    finally:
        current_scope().close()


def referenced_evidence_paths(value: object) -> set[Path]:
    paths: set[Path] = set()
    if isinstance(value, dict):
        for key, nested in value.items():
            if key in {"log_file", "result_file", "listener_result_file"} and isinstance(nested, str):
                paths.add(Path(nested))
            paths.update(referenced_evidence_paths(nested))
    elif isinstance(value, list):
        for nested in value:
            paths.update(referenced_evidence_paths(nested))
    return paths


def evidence_index(root: Path, artifacts: list[dict]) -> list[dict]:
    if not root.is_dir():
        return []
    indexed: list[dict] = []
    for evidence_path in sorted(referenced_evidence_paths(artifacts), key=lambda item: str(item)):
        resolved = evidence_path.resolve()
        try:
            relative = resolved.relative_to(root.resolve())
        except ValueError as error:
            raise RuntimeError(f"interop evidence escapes artifact root: {evidence_path}") from error
        if not resolved.is_file():
            raise RuntimeError(f"interop evidence is missing: {evidence_path}")
        indexed.append({
            "path": relative.as_posix(),
            "size": resolved.stat().st_size,
            "sha256": sha256_file(resolved),
        })
    return indexed


def acceptance_manifest_metadata(path: Optional[str]) -> Optional[dict]:
    if path is None:
        return None
    manifest = Path(path).resolve()
    if not manifest.is_file():
        raise RuntimeError(f"acceptance manifest is unavailable: {manifest}")
    return {"path": str(manifest), "sha256": sha256_file(manifest)}


def write_artifact(path: Path, root: Path, provenance: dict, artifacts: list[dict], failures: list[str],
                   runner_argv: list[str], started_at_unix: float, acceptance_manifest: Optional[dict]) -> None:
    path.write_text(
        json.dumps(
            {
                "schema_version": 2,
                "runner_argv": runner_argv,
                "started_at_unix": started_at_unix,
                "finished_at_unix": time.time(),
                "acceptance_manifest": acceptance_manifest,
                "artifact_root": str(root),
                "fixture_provenance": provenance,
                "artifacts": artifacts,
                "failures": failures,
                "evidence_index": evidence_index(root, artifacts),
            },
            indent=2,
        )
        + "\n"
    )


def record_case_failure(artifacts: list[dict], failures: list[str], label: str, error: Exception) -> None:
    failures.append(f"{label}: {error}")
    if isinstance(error, CaseFailure):
        artifacts.append({"case_label": label, **error.artifact})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--enabled", required=True)
    parser.add_argument("--provenance-only", action="store_true")
    parser.add_argument("--suite", choices=("stage6", "autonat"), default="stage6")
    parser.add_argument("--forge-fixture", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--forge-root", required=True)
    parser.add_argument("--donors-root", required=True)
    parser.add_argument("--acceptance-manifest")
    args = parser.parse_args()

    if not args.provenance_only and not enabled_from_args(args.enabled):
        print("SKIP: live libp2p interop disabled; configure FORGE_ENABLE_LIBP2P_INTEROP=ON or set FORGE_ENABLE_LIBP2P_INTEROP=1.")
        return 0

    donors_root = Path(args.donors_root).resolve()
    source_dir = Path(args.source_dir).resolve()
    build_dir = Path(args.build_dir).resolve()
    forge_root = Path(args.forge_root).resolve()
    build_dir.mkdir(parents=True, exist_ok=True)
    artifacts: list[dict] = []
    failures: list[str] = []
    root = build_dir / ("autonat-run" if args.suite == "autonat" else "interop-run")
    artifact_path = build_dir / (
        "interop-provenance-artifacts.json" if args.provenance_only else
        "autonat-artifacts.json" if args.suite == "autonat" else "interop-artifacts.json"
    )
    provenance = {
        "forge_worktree": {"start": None, "end": None, "changed_during_run": None},
        "fixture_build_info": None,
        "binaries": {},
        "tools": {},
        "commands": [],
        "runner_inputs": {
            "suite": args.suite,
            "source_dir": str(source_dir),
            "build_dir": str(build_dir),
            "forge_root": str(forge_root),
            "donors_root": str(donors_root),
            "acceptance_manifest": (
                str(Path(args.acceptance_manifest).resolve()) if args.acceptance_manifest is not None else None
            ),
        },
    }
    started_at_unix = time.time()
    runner_argv = [str(Path(sys.executable).resolve()), *sys.argv]
    start_identity = None
    acceptance_manifest = None
    try:
        acceptance_manifest = acceptance_manifest_metadata(args.acceptance_manifest)
        python_path = str(Path(sys.executable).resolve())
        git_path = str(Path(require_tool("git")).resolve())
        provenance["tools"]["python"] = {
            "path": python_path,
            "version_output": command_output([python_path, "--version"]),
        }
        provenance["tools"]["git"] = {
            "path": git_path,
            "version_output": command_output([git_path, "--version"]),
        }
        start_identity = worktree_identity(forge_root)
        provenance["forge_worktree"]["start"] = start_identity.as_json()
        fixture_lock = load_fixture_lock(source_dir)
        canonical_donor_revisions = load_canonical_donor_revisions(source_dir / "donor_cases.json")
        fixture_donor_revisions, donor_binding_errors = fixture_donor_revision_bindings(
            fixture_lock["donors"], canonical_donor_revisions
        )
        if donor_binding_errors:
            raise RuntimeError("; ".join(donor_binding_errors))
        forge_fixture = Path(args.forge_fixture).resolve()
        build_info, build_info_command = require_fixture_provenance(forge_fixture, start_identity, fixture_lock)
        provenance["fixture_build_info"] = build_info
        provenance["commands"].append({"kind": "forge_build_info", **build_info_command})
        provenance["binaries"]["forge"] = {
            "path": str(forge_fixture),
            "sha256": sha256_file(forge_fixture),
        }

        if not args.provenance_only:
            donor_checkout_errors = fixture_donor_checkout_errors(
                donors_root, fixture_lock["donors"], canonical_donor_revisions
            )
            if donor_checkout_errors:
                raise RuntimeError("; ".join(donor_checkout_errors))
            require_donor(donors_root, "go-libp2p")
            require_donor(donors_root, "go-libp2p-kad-dht")
            require_donor(donors_root, "go-libp2p-pubsub")
            require_donor(donors_root, "rust-libp2p")
            require_donor(donors_root, "libp2p-specs")

            fixture_check = source_dir / "check_fixture_lock.py"
            fixture_check_command = [
                python_path,
                str(fixture_check),
                str(source_dir),
                str(donors_root),
            ]
            provenance["commands"].append({"kind": "fixture_lock_check", "command": fixture_check_command})
            run(fixture_check_command)
            tools, go_environment, rust_environment = require_toolchain(fixture_lock, source_dir)
            provenance["tools"].update(tools)
            fixture_deps = export_fixture_deps(
                build_dir, donors_root, fixture_lock, git_path, provenance["commands"]
            )
            go_fixture, go_build_commands = prepare_go_fixture(
                source_dir, build_dir, tools["go"]["path"], go_environment
            )
            rust_fixture, rust_build_commands = prepare_rust_fixture(
                source_dir, build_dir, tools["cargo"]["path"], rust_environment
            )
            provenance.update({
                "lock_file": str(source_dir / "fixture-lock.json"),
                "fixture_deps": str(fixture_deps),
                "donors": fixture_lock["donors"],
                "donor_revisions": fixture_donor_revisions,
                "dependency_graphs": fixture_lock.get("dependency_graphs", {}),
            })
            provenance["commands"].extend(go_build_commands + rust_build_commands)
            binaries = {
                "forge": forge_fixture,
                "go": go_fixture,
                "rust": rust_fixture,
            }
            provenance["binaries"].update({
                implementation: {"path": str(binary), "sha256": sha256_file(binary)}
                for implementation, binary in binaries.items()
            })

            if root.exists():
                shutil.rmtree(root)
            root.mkdir(parents=True)
            pnet_key_file, pnet_mismatch_key_file, pnet_fingerprint = pnet_fixture_paths(source_dir, root)
            if args.suite == "stage6":
                for listener in ("go", "rust", "forge"):
                    for dialer in ("forge", "go", "rust"):
                        if listener == dialer:
                            continue
                        for scenario in SCENARIOS:
                            for acceptance_scenario_id in CURRENT_ACCEPTANCE_SCENARIOS.get(
                                f"quic_base/{scenario}", (scenario,)
                            ):
                                try:
                                    artifacts.append(
                                        run_pair(
                                            binaries[dialer],
                                            dialer,
                                            binaries[listener],
                                            listener,
                                            scenario,
                                            root,
                                            acceptance_scenario_id,
                                        )
                                    )
                                except Exception as error:
                                    record_case_failure(artifacts, failures, f"{dialer}->{listener} {acceptance_scenario_id}", error)
                        for scenario in DHT_SCENARIOS:
                            try:
                                if scenario in DHT_VALUE_SCENARIOS:
                                    artifacts.append(run_dht_value_remote_get(binaries, dialer, listener, scenario, root))
                                else:
                                    for acceptance_scenario_id in CURRENT_ACCEPTANCE_SCENARIOS.get(
                                        f"quic_dht/{scenario}", (scenario,)
                                    ):
                                        artifacts.append(
                                            run_pair(
                                                binaries[dialer],
                                                dialer,
                                                binaries[listener],
                                                listener,
                                                scenario,
                                                root,
                                                acceptance_scenario_id,
                                            )
                                        )
                            except Exception as error:
                                record_case_failure(artifacts, failures, f"{dialer}->{listener} {scenario}", error)
                        for scenario in PUBSUB_SCENARIOS:
                            if "forge" not in (dialer, listener):
                                continue
                            try:
                                artifacts.append(
                                    run_pair(
                                        binaries[dialer], dialer, binaries[listener], listener, scenario, root,
                                    )
                                )
                            except Exception as error:
                                record_case_failure(artifacts, failures, f"{dialer}->{listener} {scenario}", error)
                for dialer, listener in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")):
                    for transport, profile in (("tcp", "tcp_noise"), ("tcp-tls", "tcp_tls"), ("tcp", "tcp_stage6")):
                        for scenario in LIVE_SCENARIO_PROFILES[profile]:
                            if scenario in AUTONAT_SCENARIOS:
                                continue  # The paired suite below owns native AutoNAT once per invocation.
                            for acceptance_scenario_id in CURRENT_ACCEPTANCE_SCENARIOS.get(
                                f"{profile}/{scenario}",
                                (None,) if f"{profile}/{scenario}" in UNCLAIMED_SMOKE_SCENARIOS else (scenario,),
                            ):
                                try:
                                    artifacts.append(
                                        run_pair_with_transport(
                                            binaries[dialer],
                                            dialer,
                                            binaries[listener],
                                            listener,
                                            "echo" if scenario == "dnsaddr" else scenario,
                                            root,
                                            transport,
                                            "native",
                                            ("tcp", "yamux"),
                                            f"{profile}/{scenario}",
                                            acceptance_scenario_id,
                                            dnsaddr=scenario == "dnsaddr",
                                        )
                                    )
                                except Exception as error:
                                    record_case_failure(artifacts, failures,
                                                        f"{dialer}->{listener} {transport} {acceptance_scenario_id}", error)
                for dialer, listener in (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge")):
                    for scenario in LIVE_SCENARIO_PROFILES["private_tcp_yamux_pnet"]:
                        if scenario in AUTONAT_SCENARIOS:
                            continue
                        for acceptance_scenario_id in CURRENT_ACCEPTANCE_SCENARIOS.get(
                            f"private_tcp_yamux_pnet/{scenario}", (scenario,)
                        ):
                            try:
                                artifacts.append(
                                    run_pair_with_transport(
                                        binaries[dialer], dialer, binaries[listener], listener, "pnet", root,
                                        "tcp-pnet", "private_network", ("tcp", "pnet", "yamux"),
                                        f"private_tcp_yamux_pnet/{scenario}", acceptance_scenario_id,
                                        pnet_key_file, pnet_mismatch_key_file, pnet_fingerprint,
                                        dnsaddr=scenario == "dnsaddr_private_tcp_yamux_pnet",
                                    )
                                )
                            except Exception as error:
                                record_case_failure(artifacts, failures,
                                                    f"{dialer}->{listener} tcp-pnet {acceptance_scenario_id}", error)
                try:
                    artifacts.append(run_pubsub_mixed_mesh_stress(binaries, root))
                except Exception as error:
                    record_case_failure(artifacts, failures, PUBSUB_STRESS_SCENARIO, error)
                for listener, dialer in (("rust", "forge"), ("forge", "rust")):
                    for scenario in RENDEZVOUS_SCENARIOS:
                        for acceptance_scenario_id in CURRENT_ACCEPTANCE_SCENARIOS.get(
                            f"quic_rendezvous/{scenario}", (scenario,)
                        ):
                            try:
                                artifacts.append(
                                    run_pair(
                                        binaries[dialer],
                                        dialer,
                                        binaries[listener],
                                        listener,
                                        scenario,
                                        root,
                                        acceptance_scenario_id,
                                    )
                                )
                            except Exception as error:
                                record_case_failure(artifacts, failures, f"{dialer}->{listener} {acceptance_scenario_id}", error)
                for seeker, routing, hidden in HIDDEN_DHT_PERMUTATIONS:
                    try:
                        artifacts.append(run_hidden_dht_find_peer(binaries, seeker, routing, hidden, root))
                    except Exception as error:
                        record_case_failure(artifacts, failures, f"{seeker}->{routing}->{hidden} {HIDDEN_DHT_SCENARIO}", error)
                for scenario in TOPOLOGY_SCENARIOS:
                    try:
                        artifacts.append(run_topology(binaries["forge"], "forge", scenario, root))
                    except Exception as error:
                        record_case_failure(artifacts, failures, f"forge topology {scenario}", error)
                    for source, relay_impl, destination_impl in NATIVE_TOPOLOGIES:
                        try:
                            artifacts.append(
                                run_native_relay_topology(binaries, source, relay_impl, destination_impl, scenario, root)
                            )
                        except Exception as error:
                            record_case_failure(artifacts, failures,
                                                f"{source}->{relay_impl}->{destination_impl} native relay topology {scenario}", error)
            # Full Stage 6 includes the complete paired suite, using the same
            # binaries, artifact root, provenance and final execution receipt.
            registered = {f"{profile}/{name}"
                          for profile in ("quic_stage6", "tcp_stage6", "private_tcp_yamux_pnet")
                          for name in LIVE_SCENARIO_PROFILES[profile] if name in AUTONAT_SCENARIOS}
            if registered != set(AUTONAT_ACCEPTANCE_SCENARIOS):
                raise RuntimeError("AutoNAT executable registration differs from the full suite")
            for artifact in run_autonat_suite(
                binaries, root, pnet_key=pnet_key_file, pnet_fingerprint=pnet_fingerprint,
                wait_json=wait_json, command_attempt=command_attempt,
                claims_for_case=autonat_claims,
            ):
                artifacts.append(artifact)
                if artifact["status"] != "passed":
                    failures.append(f"{artifact['scenario_id']}: " +
                                    "; ".join(artifact["errors"] + artifact["cleanup_errors"]))
    except Exception as error:
        failures.append(f"preflight: {error}")
    finally:
        try:
            end_identity = worktree_identity(forge_root)
            provenance["forge_worktree"]["end"] = end_identity.as_json()
            if start_identity is not None:
                changed = (
                    start_identity.head != end_identity.head or start_identity.fingerprint != end_identity.fingerprint
                )
                provenance["forge_worktree"]["changed_during_run"] = changed
                if changed:
                    failures.append(
                        "Forge worktree changed during interop run: "
                        f"start={start_identity.as_json()}, end={end_identity.as_json()}"
                    )
        except Exception as error:
            failures.append(f"provenance end fingerprint: {error}")
        write_artifact(
            artifact_path,
            root,
            provenance,
            artifacts,
            failures,
            runner_argv,
            started_at_unix,
            acceptance_manifest,
        )

    if failures:
        for failure in failures:
            print(f"INTEROP FAILURE: {failure}", file=sys.stderr)
        print(f"interop artifacts: {artifact_path}", file=sys.stderr)
        return 1

    if args.provenance_only:
        print("forge interop fixture provenance ok")
        return 0
    print(f"live libp2p interop ok: {len(artifacts)} scenarios")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
