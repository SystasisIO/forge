"""Configured official-plugin cases; donor libraries and wire protocols stay unchanged."""

from dataclasses import asdict, dataclass
import hashlib
import json
from pathlib import Path
import re
import time

from dns_fixture import DnsaddrServer
from process_lifecycle import StopBudget, current_scope, spawn_owned
from provenance import reject_duplicate_json_keys

PUBLIC_TRANSPORTS = ("tcp", "tcp-tls", "quic")
PRIVATE_TRANSPORTS = ("tcp-pnet-noise", "tcp-pnet-tls")
DIRECTIONS = (("forge", "go"), ("go", "forge"), ("forge", "rust"), ("rust", "forge"))
PRIVATE_PAYLOAD = "\x01\x02\x7f\u00ff\u20acforge-plugin-pnet"
BINARY_PAYLOAD = bytes(index % 251 for index in range(192 * 1024))


@dataclass(frozen=True)
class Case:
    backend: str
    transport: str
    dialer: str
    listener: str
    dns: bool = False
    control: str = ""

    @property
    def private(self):
        return self.transport in PRIVATE_TRANSPORTS

    @property
    def go_completion_barrier(self):
        return self.transport in ("quic", "tcp-tls") and self.dialer == "go" and self.listener == "forge"

    @property
    def go_listener_completion(self):
        return self.transport == "quic" and self.dialer == "forge" and self.listener == "go"

    @property
    def rust_completion_barrier(self):
        return self.transport == "quic" and self.dialer == "rust" and self.listener == "forge"

    @property
    def completion_barrier(self):
        return self.go_completion_barrier or self.rust_completion_barrier

    @property
    def donor_scenario(self):
        if self.control:
            return "pnet"
        if self.private:
            return "tls_identity_private_pnet" if self.transport.endswith("tls") else "noise_identity_private_pnet"
        return "echo_large"

    def endpoint_scenario(self, implementation, role):
        # The unchanged Go TCP listener finalizes upgrade observations for echo,
        # not echo_large. Its handler still reads the actual 192 KiB challenge.
        if implementation == "go" and role == "listener" and not self.private and self.transport != "quic":
            return "echo"
        return self.donor_scenario

    @property
    def scenario_id(self):
        group = (f"psk-{self.control}" if self.control else "psk") if self.private else "dns" if self.dns else "public"
        return f"plugin-node/{self.backend}/{group}/{self.transport}/{self.dialer}-to-{self.listener}"

    @property
    def payload(self):
        # Unchanged private donor CLI accepts UTF-8 argv, not arbitrary NUL bytes.
        # This challenge still includes control and non-ASCII bytes; public
        # echo_large separately proves the full 0..250 binary pattern.
        return PRIVATE_PAYLOAD.encode() if self.private else BINARY_PAYLOAD


def case_specs(backend):
    if backend not in ("mdbx", "rocksdb"):
        raise ValueError("plugin-node requires an actual configured ObjectDB backend")
    public = [Case(backend, transport, dialer, listener) for transport in PUBLIC_TRANSPORTS
              for dialer, listener in DIRECTIONS]
    dns = [Case(backend, transport, "forge", donor, dns=True) for transport in PUBLIC_TRANSPORTS
           for donor in ("go", "rust")]
    private = [Case(backend, transport, dialer, listener, control=control)
               for transport in PRIVATE_TRANSPORTS for dialer, listener in DIRECTIONS
               for control in ("", "missing_key", "mismatched_key")]
    return tuple(public + dns + private)


def command(spec, implementation, role, binary, work, *, key=None, fingerprint="", peer="", address="", dns_server=""):
    prefix = work / role
    argv = [str(binary), "listen" if role == "listener" else "dial",
            "--scenario", spec.endpoint_scenario(implementation, role), "--transport", spec.transport,
            "--store-dir", str(prefix.with_suffix(".store")), "--result-file", str(prefix.with_suffix(".json")),
            "--stop-file", str(prefix.with_suffix(".stop"))]
    if implementation == "forge":
        argv += ["--store-backend", spec.backend]
    if role == "listener":
        argv += ["--ready-file", str(prefix.with_suffix(".ready.json")), "--features", "ping,identify"]
        if implementation == "go" and spec.go_listener_completion:
            argv += ["--echo-listener-completion", "orderly-fin", "--echo-witness-file",
                     str(prefix.with_suffix(".echo.json"))]
    else:
        argv += ["--peer-id", peer, "--addr", address, "--payload", PRIVATE_PAYLOAD]
        if spec.completion_barrier:
            argv += ["--echo-completion-barrier", "stop-file", "--ready-file", str(prefix.with_suffix(".ready.json"))]
    if key is not None:
        argv += ["--pnet-key-file", str(key), "--pnet-fingerprint", fingerprint]
    if spec.control:
        argv += ["--pnet-control", spec.control, "--pnet-correlation", spec.scenario_id]
    if dns_server:
        if implementation != "forge" or role != "dialer" or not spec.dns:
            raise ValueError("only the configured Forge outgoing DNS lane owns this nameserver")
        argv += ["--dns-server", dns_server]
    return argv


def _load(path):
    return json.loads(path.read_text(), object_pairs_hook=reject_duplicate_json_keys)


def _wait_result(path, owner, statuses, timeout=35):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            result = _load(path)
            if result.get("status") in statuses:
                return result
            if result.get("status") in ("error", "failed"):
                raise RuntimeError(f"endpoint failed; redacted receipt at {path}")
        if owner.process.poll() is not None:
            raise RuntimeError(f"endpoint exited before its completion witness; log={owner.log_file}")
        time.sleep(0.005)
    raise TimeoutError(f"endpoint completion witness expired: {path}")


def socket(value, quic=False):
    pattern = r"/ip4/127\.0\.0\.1/udp/([1-9][0-9]{0,4})/quic-v1" if quic else r"/ip4/127\.0\.0\.1/tcp/([1-9][0-9]{0,4})"
    match = re.fullmatch(pattern, value) if isinstance(value, str) else None
    if match is None or int(match[1]) > 65535:
        raise ValueError("missing actual numeric loopback socket")
    return value


def listener_socket(value, peer, quic=False):
    if (not isinstance(value, str) or value.count("/p2p/") != 1
            or not isinstance(peer, str) or not peer or "/" in peer):
        raise ValueError("listener address needs exactly one terminal owned peer identity")
    numeric, actual_peer = value.split("/p2p/")
    if actual_peer != peer:
        raise ValueError("listener address peer differs from its owned readiness identity")
    return socket(numeric, quic)


def _address(ready, transport):
    token = "/udp/" if transport == "quic" else "/tcp/"
    addresses = ready.get("listen_addrs", [])
    matching = [address for address in addresses if isinstance(address, str)
                and address.startswith("/ip4/127.0.0.1/") and token in address]
    if not matching or not isinstance(ready.get("peer_id"), str):
        raise RuntimeError("listener has no actual loopback address for the selected transport")
    for address in matching:
        listener_socket(address, ready["peer_id"], transport == "quic")
    return matching[0]


def decoded_test_key_hash(path):
    if path is None:
        return ""
    lines = path.read_bytes().splitlines()
    if len(lines) != 3 or lines[:2] != [b"/key/swarm/psk/1.0.0/", b"/base16/"]:
        raise ValueError("noncanonical public test-key fixture")
    decoded = bytes.fromhex(lines[2].decode("ascii"))
    if len(decoded) != 32:
        raise ValueError("test-key fixture is not 32 decoded bytes")
    return hashlib.sha256(decoded).hexdigest()


def run_case(spec, binaries, root, *, key, mismatch, fingerprint, wait_json, command_attempt):
    if current_scope() is None:
        raise RuntimeError("plugin-node case requires the existing runner process owner")
    work = root / spec.scenario_id.replace("/", "-")
    work.mkdir(parents=True, exist_ok=False)
    listener_key = key if spec.private else None
    dial_key = (None if spec.control == "missing_key" else mismatch if spec.control == "mismatched_key" else key) \
        if spec.private else None

    def spawn(implementation, role, **kwargs):
        argv = command(spec, implementation, role, binaries[implementation], work, **kwargs)
        attempt = command_attempt(argv, work / f"{role}.log", spec.scenario_id, 1, role, 45)
        budget = StopBudget(10, 0, 10) if implementation == "forge" else None
        return spawn_owned(argv, work / f"{role}.log", work / f"{role}.stop", attempt, stop_budget=budget)

    server = spawn(spec.listener, "listener", key=listener_key, fingerprint=fingerprint)
    server.ready = wait_json(work / "listener.ready.json", 20)
    address = _address(server.ready, spec.transport)
    peer = server.ready["peer_id"]
    dns_record = None

    def complete(dial_address, nameserver=""):
        client = spawn(spec.dialer, "dialer", key=dial_key, fingerprint=fingerprint,
                       peer=peer, address=dial_address, dns_server=nameserver)
        expected = {"rejected", "rejection_candidate"} if spec.control else {"ok"}
        exchange_witness = None
        if spec.completion_barrier:
            # Test CLI synchronization only: an actual exchange witness is not
            # a terminal donor receipt. No successful Forge close is inferred.
            from plugin_node_evidence import (
                go_completion_pair, go_completion_witness, plugin_exchange_witness,
                rust_completion_pair, rust_completion_witness,
            )
            check_witness = go_completion_witness if spec.go_completion_barrier else rust_completion_witness
            check_pair = go_completion_pair if spec.go_completion_barrier else rust_completion_pair
            exchange_witness = _wait_result(work / "dialer.ready.json", client, {"exchange_observed"})
            plugin = _wait_result(work / "listener.json", server, {"ok"})
            exchange = plugin_exchange_witness(plugin, spec, peer)
            check_witness(exchange_witness, spec, exchange["session"])
        else:
            _wait_result(work / "dialer.json", client, expected)
        if spec.listener == "forge" and not spec.completion_barrier:
            _wait_result(work / "listener.json", server, {"rejected"} if spec.control else {"ok"})
        listener_witness = None
        if spec.go_listener_completion:
            from plugin_node_evidence import go_listener_completion, plugin_exchange_witness
            plugin = _load(work / "dialer.json")
            exchange = plugin_exchange_witness(plugin, spec, peer, role="dialer")
            listener_witness = _wait_result(work / "listener.echo.json", server, {"ok"})
            go_listener_completion(listener_witness, spec, exchange["session"], final=False)
        # After both echo directions finish, keep the Forge host alive until the
        # retained Go stream closes. Concurrent host stops can race that close
        # with Yamux GOAWAY; such an error must not be suppressed as success.
        client.request_stop()
        cleanup = client.close() if spec.completion_barrier else []
        if spec.completion_barrier:
            if cleanup:
                raise RuntimeError("plugin-node donor completion cleanup failed: " + "; ".join(cleanup))
            check_pair({"exchange_witness": exchange_witness,
                        "result": _load(work / "dialer.json")}, spec, exchange["session"])
        server.request_stop()
        if not spec.completion_barrier:
            cleanup += client.close()
        cleanup += server.close()
        if cleanup:
            raise RuntimeError("plugin-node owned cleanup failed: " + "; ".join(cleanup))
        endpoints = {}
        for role, implementation, owner in (("dialer", spec.dialer, client), ("listener", spec.listener, server)):
            path = work / f"{role}.json"
            # The unchanged Go QUIC listener exports ready+terminal, not a final
            # session receipt. Never synthesize one from its configured address.
            endpoints[role] = {"implementation": implementation, "process": owner.evidence(),
                               "result": _load(path) if path.is_file() else None}
            if path.is_file():
                endpoints[role]["result_file"] = str(path)
            if role == "dialer" and exchange_witness is not None:
                endpoints[role]["exchange_witness"] = exchange_witness
                endpoints[role]["exchange_witness_file"] = str(work / "dialer.ready.json")
            if role == "listener" and listener_witness is not None:
                endpoints[role]["listener_witness"] = listener_witness
                endpoints[role]["listener_witness_file"] = str(work / "listener.echo.json")
        return endpoints

    if spec.dns:
        evidence_path = work / "dns.json"
        with DnsaddrServer(address, peer, evidence_path) as resolver:
            endpoints = complete(resolver.root, resolver.nameserver)
            resolver.require_chain()
        dns_record = {"evidence_file": str(evidence_path), "result": _load(evidence_path)}
    else:
        endpoints = complete(address)
    return {"suite": "plugin-node", "scenario_id": spec.scenario_id, "spec": asdict(spec),
            "peer_id": peer, "listen_address": address, "endpoints": endpoints, "dns": dns_record,
            "test_key_hashes": {"kind": "sha256-decoded-public-test-key", "listener": decoded_test_key_hash(listener_key),
                                "dialer": decoded_test_key_hash(dial_key)},
            "limitations": (["Private donor argv challenge cannot carry NUL; full binary pattern is the public echo_large lane."]
                            if spec.private and not spec.control else []) +
                           (["Unchanged QUIC listener CLI does not export a detailed server-side security/Identify receipt."]
                            if spec.listener != "forge" and spec.transport == "quic" else []) +
                           (["Go echo_large dialer exports actual ConnState, checked echo and same-connection automatic Identify, not an upgrade-phase trace."]
                            if spec.dialer == "go" and not spec.private and spec.transport != "quic" else []) +
                           (["Go QUIC echo_large uses an opt-in test-CLI StopFile completion barrier; this is harness synchronization, not a go-libp2p patch or immediate-disconnect success. Original immediate-exit RED artifacts remain separate."]
                            if spec.go_completion_barrier else []) +
                           (["Pinned Rust fixture release enables Ed25519 host identities, not rsa; the shared Ed25519 fixture is not RSA interop evidence."]
                            if "rust" in (spec.dialer, spec.listener) else [])}
