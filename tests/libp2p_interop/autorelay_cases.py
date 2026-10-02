"""PR9 live cases using the existing runner, native fixtures and process owner."""

from dataclasses import asdict, dataclass
import json
from pathlib import Path
import time

from autonat_cases import _snapshot
from autorelay_evidence import validate_case
from process_lifecycle import enter_scope, exit_scope, spawn_owned
from provenance import reject_duplicate_json_keys


@dataclass(frozen=True)
class Case:
    source: str
    relay: str
    destination: str
    transport: str
    kind: str

    @property
    def identifier(self):
        return f"autorelay-{self.kind}-{self.transport}-{self.source}-{self.relay}-{self.destination}"


def case_specs():
    return tuple(
        Case(source, relay, destination, transport, kind)
        for transport in ("quic", "tcp", "tcp-tls")
        for source, relay, destination, kind in (
            ("go", "go", "forge", "lifecycle"),
            ("rust", "rust", "forge", "lifecycle"),
            ("go", "forge", "rust", "service"),
            ("rust", "forge", "go", "service"),
        )
    )


def _read(path):
    if path.stat().st_size > 1024 * 1024:
        raise ValueError("AutoRelay evidence exceeds 1 MiB")
    value = json.loads(path.read_text(), object_pairs_hook=reject_duplicate_json_keys)
    if not isinstance(value, dict):
        raise ValueError("AutoRelay evidence must be an object")
    return value


def _await(owner, path, predicate, seconds=10):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if owner.process.poll() is not None:
            raise RuntimeError(f"AutoRelay actor exited before observation: {owner.log_file}")
        if path.is_file():
            value = _read(path)
            if predicate(value):
                return value
        time.sleep(0.05)
    raise TimeoutError(f"AutoRelay observation deadline: {path}")


def _latest(value):
    rows = value.get("observations", [])
    return rows[-1] if rows else {}


def _lease(value, peer):
    return next((r for r in _latest(value).get("reservations", [])
                 if r.get("relay_peer_id") == peer and r.get("expires_unix_ms", 0) > _latest(value).get("unix_ms", 0)), None)


def _circuits(value, peer=None):
    return [a for a in _latest(value).get("addresses", []) if "/p2p-circuit" in a
            and (peer is None or f"/p2p/{peer}/p2p-circuit" in a)]


def run_case(spec, binaries, root, *, wait_json, command_attempt, start_destination, run_relay_dial,
             claims_for_case):
    scope, token = enter_scope()
    work = root / spec.identifier
    owners, raw, echoes, faults = {}, {}, [], []
    errors = []
    started = time.monotonic()
    try:
        work.mkdir(parents=True, exist_ok=False)

        def launch(label, implementation, command, extra=()):
            result = work / f"{label}.json"
            argv = [str(binaries[implementation]), command, "--scenario", "autorelay",
                    "--transport", spec.transport, "--ready-file", str(work / f"{label}-ready.json"),
                    "--stop-file", str(work / f"{label}.stop"), "--result-file", str(result), *extra]
            if implementation == "forge":
                argv += ["--store-dir", str(work / f"{label}-store")]
            attempt = command_attempt(argv, work / f"{label}.log", spec.identifier, 1, label, 60)
            owner = spawn_owned(argv, work / f"{label}.log", work / f"{label}.stop", attempt)
            owners[label] = owner
            owner.ready = wait_json(work / f"{label}-ready.json", 10)
            return owner, result

        service_cmd = "autorelay-service" if spec.relay == "forge" else "autorelay-relay"
        extra = [] if spec.relay == "forge" else ["--relay-ttl-seconds", "8"]
        relay, relay_result = launch("relay", spec.relay, service_cmd, extra)
        relay_peer = relay.ready["peer_id"]
        relay_addr = relay.ready["listen_addrs"][0]

        def echo(phase, ready, circuit=None):
            echo_work = work / phase
            echo_work.mkdir()
            # Acquisition proofs pass the independently received Identify address,
            # not a runner-constructed circuit address or a self-query result.
            address = circuit.split("/p2p-circuit")[0] if circuit else ready["listen_addrs"][0]
            result = run_relay_dial(binaries[spec.source], spec.source, "autorelay",
                                    destination.ready["peer_id"], ready["peer_id"], address, echo_work,
                                    transport=spec.transport)
            echoes.append({"phase": phase, "result_file": result["result_file"]})

        if spec.kind == "service":
            destination = start_destination(binaries[spec.destination], spec.destination,
                                            relay_addr, relay_peer, work, transport=spec.transport,
                                            scenario="autorelay")
            owners["destination"] = destination
            echo("service_echo", relay.ready)
            _await(relay, relay_result, lambda value: _latest(value).get("relay_bytes", 0) > 0)
        else:
            seed_file = work / "seeds.txt"
            seed_file.write_text("")
            destination, destination_result = launch("destination", "forge", "autorelay-destination",
                                                      ["--seed-file", str(seed_file)])
            probe = work / "observer-probe.txt"
            observer, observer_result = launch("observer", spec.source, "autorelay-observe",
                                               ["--addr", destination.ready["listen_addrs"][0],
                                                "--peer-id", destination.ready["peer_id"],
                                                "--probe-file", str(probe)])

            def identify(revision):
                probe.write_text(revision)
                value = _await(observer, observer_result,
                               lambda v: any(e.get("revision") == revision for e in v.get("events", [])))
                return next(e for e in value["events"] if e.get("revision") == revision)

            def await_push(peer, after=0):
                def received(value):
                    for event in value.get("events", []):
                        if event.get("kind") != "identify_push" or event.get("unix_ms", 0) <= after:
                            continue
                        addresses = event.get("addresses", [])
                        if (peer and any(f"/p2p/{peer}/p2p-circuit" in a for a in addresses)) or (
                                peer is None and not any("/p2p-circuit" in a for a in addresses)):
                            return True
                    return False
                _await(observer, observer_result, received)

            identify("before_acquire")
            seed_file.write_text(relay_addr + "\n")
            acquired = _await(destination, destination_result,
                              lambda v: _lease(v, relay_peer) is not None and bool(_circuits(v, relay_peer)))
            initial_expiry = _lease(acquired, relay_peer)["expires_unix_ms"]
            await_push(relay_peer)
            advertised = identify("acquired")
            circuit = next(a for a in advertised["addresses"] if f"/p2p/{relay_peer}/p2p-circuit" in a)
            echo("acquired_echo", relay.ready, circuit)
            _await(destination, destination_result, lambda v: _lease(v, relay_peer) is not None
                   and _lease(v, relay_peer)["expires_unix_ms"] > initial_expiry, 12)
            # A second native echo after the first wire lease would have expired.
            remaining = initial_expiry / 1000 - time.time() + 0.15
            if remaining > 0:
                time.sleep(min(remaining, 9))
            echo("renewed_echo", relay.ready, circuit)
            faults.append({"kind": "relay_stop", "peer_id": relay_peer, "unix_ms": time.time_ns() // 1_000_000})
            if relay.close():
                raise RuntimeError("relay fault injection did not stop cleanly")
            _await(destination, destination_result,
                   lambda v: not _circuits(v, relay_peer) and _lease(v, relay_peer) is None)
            await_push(None, faults[-1]["unix_ms"])
            identify("withdrawn")
            replacement_impl = "rust" if spec.relay == "go" else "go"
            replacement, replacement_result = launch("replacement", replacement_impl, "autorelay-relay",
                                                       ["--relay-ttl-seconds", "8"])
            replacement_peer = replacement.ready["peer_id"]
            seed_file.write_text(relay_addr + "\n" + replacement.ready["listen_addrs"][0] + "\n")
            _await(destination, destination_result,
                   lambda v: _lease(v, replacement_peer) is not None and bool(_circuits(v, replacement_peer)))
            await_push(replacement_peer)
            advertised = identify("replacement")
            circuit = next(a for a in advertised["addresses"] if f"/p2p/{replacement_peer}/p2p-circuit" in a)
            echo("replacement_echo", replacement.ready, circuit)
            faults.append({"kind": "relay_stop", "peer_id": replacement_peer, "unix_ms": time.time_ns() // 1_000_000})
            if replacement.close():
                raise RuntimeError("replacement relay did not stop cleanly")
            _await(destination, destination_result, lambda v: not _circuits(v) and _lease(v, replacement_peer) is None)
            await_push(None, faults[-1]["unix_ms"])
            identify("replacement_withdrawn")
            if destination.close():
                raise RuntimeError("Forge destination shutdown did not join")
    except Exception as error:
        errors.append(f"execution: {error}")
    finally:
        cleanup = scope.close()
        for attempt in scope.attempts:
            terminal = attempt.get("terminal_status")
            if isinstance(terminal, dict):
                attempt["exit_code"] = terminal.get("exit_code")
        for role, owner in owners.items():
            try:
                raw[role] = {"ready": _snapshot(owner, "--ready-file"), "process": owner.evidence()}
                if "--result-file" in owner.command:
                    raw[role]["result"] = _snapshot(owner, "--result-file")
            except Exception as error:
                errors.append(f"capture {role}: {error}")
        for echo in echoes:
            try:
                path = Path(echo["result_file"])
                owner = next(p for p in scope.processes if "--result-file" in p.command
                             and p.command[p.command.index("--result-file") + 1] == str(path))
                echo["result"] = _snapshot(owner, "--result-file")
                echo["process"] = owner.evidence()
            except Exception as error:
                errors.append(f"capture echo: {error}")
        artifact = {"suite": "autorelay", "scenario": "autorelay", "scenario_id": spec.identifier, "case": asdict(spec),
                    "acceptance_scenario_ids": claims_for_case(spec), "raw": raw, "echoes": echoes, "faults": faults,
                    "processes": scope.evidence(), "attempts": scope.attempts,
                    "errors": errors, "cleanup_errors": cleanup, "elapsed_seconds": time.monotonic() - started}
        if not errors and not cleanup:
            errors.extend(validate_case(artifact))
        artifact["status"] = "passed" if not errors and not cleanup else "failed"
        exit_scope(token)
    return artifact


def run_suite(binaries, root, **support):
    for spec in case_specs():
        yield run_case(spec, binaries, root, **support)
