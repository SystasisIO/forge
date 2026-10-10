"""Two-host QUIC shutdown diagnostic. Never produces acceptance evidence.

Uses existing, independently built fixtures. Nonzero actor exits remain errors;
an expected reproduction does not turn a failed native outcome into success.
"""

import argparse
import hashlib
import json
from pathlib import Path
import secrets
import time

from process_lifecycle import StopBudget, enter_scope, exit_scope, spawn_owned
from provenance import sha256_file
from pubsub_cases import _await, _has, _mesh_is, _read, _ready


def run_pair(pair, order, binaries, root):
    work = root / ("-".join(pair) + "-" + order)
    work.mkdir()
    token = secrets.token_hex(16)
    actors, sequences, errors, stops = {}, {}, [], []
    scope, scope_token = enter_scope()
    deadline = time.monotonic() + 30
    try:
        for role, implementation in zip(("victim", "sink"), pair):
            files = {name: work / (role + "." + name) for name in ("ready", "result", "stop", "control")}
            argv = [str(binaries[implementation]), "pubsub-live", "--version", "1.1", "--transport", "quic",
                    "--actor", role, "--case-token", token, "--store-dir", str(work / (role + ".store"))]
            for name, path in files.items():
                argv += ["--" + name + "-file", str(path)]
            owner = spawn_owned(argv, work / (role + ".log"), files["stop"], stop_budget=StopBudget(8, 0, 2))
            actors[role] = (owner, files, implementation)
            sequences[role] = 0
            owner.ready = _await(owner, files["ready"], lambda value: _ready(
                value, _read(files["result"]) if files["result"].exists() else None,
                implementation, role, token), deadline)

        def wait(role, predicate):
            owner, files, _ = actors[role]
            return _await(owner, files["result"], predicate, deadline)

        def command(role, kind, **fields):
            sequences[role] += 1
            with actors[role][1]["control"].open("a") as output:
                output.write(json.dumps(dict(sequence=sequences[role], kind=kind, **fields)) + "\n")
            return wait(role, lambda value: _has(value, "command_done", command_sequence=sequences[role]))

        def peer(role):
            return actors[role][0].ready["peer_id"]

        command("victim", "connect", peer_id=peer("sink"), address=actors["sink"][0].ready["listen_addrs"][0])
        wait("victim", lambda value: _mesh_is(value, [peer("sink")]))
        wait("sink", lambda value: _mesh_is(value, [peer("victim")]))
        for sender, receiver in (("victim", "sink"), ("sink", "victim")):
            payload = "accept:" + token + ":" + sender
            command(sender, "publish", payload=payload)
            wait(receiver, lambda value: _has(value, "delivery",
                                             payload_sha256=hashlib.sha256(payload.encode()).hexdigest()))
        for role in actors:
            prepared = command(role, "prepare_shutdown", actor=role, case_token=token, local_peer_id=peer(role))
            (work / (role + ".prepared.json")).write_text(json.dumps(prepared))

        roles = ("sink", "victim") if order == "sink-first" else ("victim", "sink")
        for role in roles:
            actors[role][0].request_stop()
            stops.append(dict(role=role, monotonic=time.monotonic()))
            if order != "both-published":
                actors[role][0].process.wait(timeout=10)
        for role in roles:
            actors[role][0].process.wait(timeout=10)
    except Exception as error:
        errors.append(type(error).__name__ + ": " + str(error))
    finally:
        # ProcessScope publishes each owned stop marker, waits and retains errors.
        # Its fallback kill is recorded as a failure, never a successful shutdown.
        cleanup = scope.close()
        exit_scope(scope_token)

    record = dict(pair=pair, requested_order=order, token=token, setup_errors=errors, cleanup_errors=cleanup,
                  result_errors=[], stop_publications=stops, diagnostic_only=True, actors={})
    for role, (owner, files, implementation) in actors.items():
        try:
            value = _read(files["result"], diagnostic=True)
            if (value.get("finalized") is not True or value.get("joined") is not True
                    or value.get("overflow") is not False or value.get("error") is not None):
                record["result_errors"].append(f"{role} native result is failed or incomplete")
            if (value.get("actor") != role or value.get("case_token") != token
                    or value.get("implementation") != implementation
                    or value.get("local_peer_id") != owner.ready.get("peer_id")):
                record["result_errors"].append(f"{role} native result identity mismatch")
        except Exception as error:
            record["result_errors"].append(f"{role} result: {error}")
            value = {}
        record["actors"][role] = dict(implementation=implementation, process=owner.evidence(), raw=value)
    (work / "diagnostic.json").write_text(json.dumps(record, indent=2))
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--forge", type=Path, required=True)
    parser.add_argument("--rust", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    binaries = {"forge": args.forge.resolve(strict=True), "rust": args.rust.resolve(strict=True)}
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    provenance = {name: dict(path=str(path), sha256=sha256_file(path)) for name, path in binaries.items()}
    (root / "binaries.json").write_text(json.dumps(provenance, indent=2))
    failed = False
    for pair in (("rust", "rust"), ("forge", "rust"), ("rust", "forge")):
        for order in ("victim-first", "sink-first", "both-published"):
            result = run_pair(pair, order, binaries, root)
            failed |= bool(result["setup_errors"] or result["cleanup_errors"] or result["result_errors"])
            print(json.dumps(dict(pair=pair, order=order, setup_errors=result["setup_errors"],
                                  result_errors=result["result_errors"], cleanup_errors=result["cleanup_errors"])), flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
