#!/usr/bin/env python3
"""Closed configured-plugin selections sharing canonical provenance, not Stage7 promotion."""

import argparse
from pathlib import Path
import sys
import time

sys.dont_write_bytecode = True

from check_stage6_acceptance import (
    is_timestamp, validate_donor_provenance, validate_evidence_index,
    validate_execution_provenance, validate_git_state,
)
from plugin_node_cases import case_specs
from plugin_node_evidence import load_json, rust_build_environment, validate_case, validate_terminal_sources
from plugin_dht_cases import case_specs as dht_case_specs
from plugin_dht_evidence import validate_case as validate_dht_case, validate_terminal_sources as validate_dht_sources
from plugin_pubsub_cases import case_specs as pubsub_case_specs
from plugin_pubsub_evidence import validate_case as validate_pubsub_case, validate_terminal_sources as validate_pubsub_sources
from plugin_relay_cases import case_specs as relay_case_specs
from plugin_relay_evidence import validate_case as validate_relay_case, validate_terminal_sources as validate_relay_sources
from plugin_autonat_cases import case_specs as autonat_case_specs
from plugin_autonat_evidence import validate_case as validate_autonat_case, validate_terminal_sources as validate_autonat_sources
from provenance import worktree_identity

SUITES = ("plugin-node", "plugin-dht", "plugin-pubsub", "plugin-relay", "plugin-autonat")


def _dht_terminal_sources(record, paths, artifact_root, source):
    return validate_dht_sources(record, paths, artifact_root)


def _pubsub_terminal_sources(record, paths, artifact_root, source):
    return validate_pubsub_sources(record, paths, artifact_root)


def _relay_terminal_sources(record, paths, artifact_root, source):
    return validate_relay_sources(record, paths, artifact_root)


def _autonat_terminal_sources(record, paths, artifact_root, source):
    return validate_autonat_sources(record, paths, artifact_root)


def _suite_functions(suite):
    if suite == "plugin-node":
        return case_specs, validate_case, validate_terminal_sources
    if suite == "plugin-dht":
        return dht_case_specs, validate_dht_case, _dht_terminal_sources
    if suite == "plugin-pubsub":
        return pubsub_case_specs, validate_pubsub_case, _pubsub_terminal_sources
    if suite == "plugin-relay":
        return relay_case_specs, validate_relay_case, _relay_terminal_sources
    if suite == "plugin-autonat":
        return autonat_case_specs, validate_autonat_case, _autonat_terminal_sources
    raise ValueError("unknown configured-plugin acceptance suite")


def registered_cases(backends, suite="plugin-node"):
    specs, _, _ = _suite_functions(suite)
    if not isinstance(backends, list) or not backends or any(not isinstance(backend, str) for backend in backends) \
            or len(set(backends)) != len(backends):
        raise ValueError("matrix needs an explicit nonempty unique backend selection")
    return {spec.scenario_id: spec for backend in backends for spec in specs(backend)}


def validate_matrix(records, backends, suite="plugin-node"):
    try:
        required = registered_cases(backends, suite)
        _, check_case, _ = _suite_functions(suite)
    except (ValueError, TypeError) as error:
        return [str(error)]
    if not isinstance(records, list):
        return ["matrix records must be an array"]
    ids = [record.get("scenario_id") for record in records if isinstance(record, dict)
           and isinstance(record.get("scenario_id"), str)]
    errors = []
    if suite == "plugin-autonat" and (len(backends) != 2 or set(backends) != {"mdbx", "rocksdb"}):
        errors.append("configured AutoNAT acceptance requires all 8 cases across both ObjectDB backends")
    if len(ids) != len(records) or len(ids) != len(set(ids)) or set(ids) != set(required):
        errors.append("configured-plugin matrix is missing, duplicated or contains unregistered cases")
    for record in records:
        if isinstance(record, dict) and isinstance(record.get("scenario_id"), str) and record["scenario_id"] in required:
            if record.get("status") != "observed":
                errors.append(record["scenario_id"] + ": case was not observed")
            spec = required[record["scenario_id"]]
            errors.extend(check_case(record, spec))
            if suite == "plugin-dht":
                proof = record.get("amino_evidence")
                if not isinstance(proof, dict) or proof != {
                    "kind": "causal-api-and-remote-io", "protocol": "/ipfs/kad/1.0.0", "negotiation_event_capture": False,
                    "value_proof": "forge.dht-api.selected" if spec.forge_role == "client" else "donor.get_value.golden-comparison",
                    "listener_snapshot_is_record_proof": False,
                } or proof.get("negotiation_event_capture") is not False or proof.get("listener_snapshot_is_record_proof") is not False:
                    errors.append(record["scenario_id"] + ": missing correctly scoped causal Amino/value evidence")
    return errors


def validate_build_commands(provenance, build_dir):
    """Bind the recorded test/build recipe to the compiled donor paths and inputs."""
    try:
        tools = provenance["tools"]
        commands = provenance["commands"]
        cargo, go = tools["cargo"]["path"], tools["go"]["path"]
        if not isinstance(commands, list) or cargo == go:
            return ["donor build command provenance is invalid"]
        rust = [entry for entry in commands if entry.get("command", [None])[0] == cargo]
        golang = [entry for entry in commands if entry.get("command", [None])[0] == go]
        expected_rust = [[cargo, "test", "--frozen"], [cargo, "build", "--release", "--frozen"]]
        expected_go = [[go, "mod", "verify"], [go, "test", "-mod=readonly", "-count=1", "-timeout=60s", "."],
                       [go, "build", "-mod=readonly", "-trimpath", "-o", str(build_dir / "go_fixture/go_fixture"), "."]]
        if [entry["command"] for entry in rust] != expected_rust or [entry["command"] for entry in golang] != expected_go:
            return ["donor frozen tests/build commands missing, reordered or modified"]
        expected_policy = {"GOTOOLCHAIN": "local", "GOPROXY": "off", "GOSUMDB": "off",
                           "GOFLAGS": "", "GOENV": "off", "GOWORK": "off"}
        if any(entry.get("cwd") != str(build_dir / "go_fixture") or entry.get("environment") != expected_policy for entry in golang):
            return ["Go build did not record the locked offline execution policy"]
        rust_environment = rust[0]["environment"]
        if rust_build_environment(rust_environment) != rust_environment or any(
                entry.get("cwd") != str(build_dir / "rust_fixture") or entry.get("environment") != rust_environment for entry in rust):
            return ["Rust test/build compilation environment receipt is inconsistent"]
        paths = provenance["binaries"]
        if paths["go"]["path"] != str(build_dir / "go_fixture/go_fixture") \
                or paths["rust"]["path"] != str(build_dir / "rust_fixture/target/release/forge-libp2p-rust-fixture"):
            return ["donor binary paths differ from the recorded build recipe"]
        return []
    except (ValueError, KeyError, TypeError, AttributeError, IndexError) as error:
        return ["donor build provenance: " + str(error)]


def validate(forge_root, artifact_path, expected_head, suite="plugin-node"):
    errors = []
    try:
        _, _, check_sources = _suite_functions(suite)
        forge_root = forge_root.resolve()
        artifact_path = artifact_path.resolve()
        artifact = load_json(artifact_path)
        commit_time, git_errors = validate_git_state(forge_root, expected_head)
        errors.extend(git_errors)
        current_identity = worktree_identity(forge_root).as_json()
        provenance = artifact["fixture_provenance"]
        inputs = provenance["runner_inputs"]
        source = forge_root / "tests/libp2p_interop"
        artifact_root = Path(artifact["artifact_root"]).resolve()
        if artifact.get("schema_version") != 2 or inputs.get("suite") != suite \
                or artifact.get("acceptance_manifest") is not None or inputs.get("acceptance_manifest") is not None:
            errors.append("not a distinct canonical " + suite + " artifact (Stage6 claims are forbidden)")
        if Path(inputs["forge_root"]).resolve() != forge_root or Path(inputs["source_dir"]).resolve() != source \
                or Path(inputs["build_dir"]).resolve() != artifact_path.parent \
                or artifact_root != artifact_path.parent / (suite + "-run") \
                or artifact_path.name != suite + "-artifacts.json":
            errors.append("artifact paths do not bind the canonical configured-plugin runner")
        build_info = provenance["fixture_build_info"]
        available = build_info.get("store_backends", [])
        backends = inputs.get("plugin_store_backends")
        selected = inputs.get("plugin_store_backend")
        if build_info.get("fixture_kind") != "configured-plugin" or selected not in ("all", "mdbx", "rocksdb") \
                or not isinstance(available, list) or not available or len(set(available)) != len(available) \
                or any(backend not in ("mdbx", "rocksdb") for backend in available) \
                or backends != (available if selected == "all" else [selected]) \
                or any(backend not in available for backend in backends):
            errors.append("backend selection differs from the configured binary; missing backend cannot pass")
        complete_count = {"plugin-dht": 16, "plugin-pubsub": 8, "plugin-relay": 8, "plugin-autonat": 8}.get(suite)
        if complete_count is not None and selected == "all" and (set(available) != {"mdbx", "rocksdb"}
                or not isinstance(backends, list) or len(backends) != 2 or set(backends) != {"mdbx", "rocksdb"}):
            errors.append(f"full {suite} selection requires exactly {complete_count} cases across both ObjectDB backends")
        paths, provenance_errors = validate_execution_provenance(provenance, expected_head, current_identity)
        errors.extend(provenance_errors)
        errors.extend(validate_donor_provenance(forge_root, provenance, {"donors_root": Path(inputs["donors_root"]).resolve()}))
        errors.extend(validate_build_commands(provenance, artifact_path.parent))
        # Reuse exact binary/worktree/pinned-donor and raw-index mechanics, not
        # Stage6 scenario registration, feature owners or its promotion wrapper.
        records = artifact.get("artifacts")
        errors.extend(validate_matrix(records, backends, suite))
        if isinstance(records, list):
            _, index_errors = validate_evidence_index(artifact_path, artifact_root, records,
                                                       artifact.get("evidence_index"), paths)
            errors.extend(index_errors)
            for record in records:
                if isinstance(record, dict) and record.get("suite") == suite:
                    errors.extend(check_sources(record, paths, artifact_root, source))
        if artifact.get("failures") != []:
            errors.append("runner has preflight/case/cleanup failures")
        start, end = artifact.get("started_at_unix"), artifact.get("finished_at_unix")
        if not is_timestamp(start) or not is_timestamp(end) or commit_time is None \
                or not commit_time <= start < end <= time.time():
            errors.append("execution timestamps do not bind the current immutable head")
        argv = artifact.get("runner_argv", [])
        if not isinstance(argv, list) or len(argv) < 2 or Path(argv[0]).resolve() != Path(sys.executable).resolve() \
                or Path(argv[1]).resolve() != source / "runner.py" or "--provenance-only" in argv:
            errors.append("execution is not the canonical live runner")
        else:
            flags = dict(zip(argv[2::2], argv[3::2]))
            if len(argv) % 2 != 0 or len(flags) * 2 != len(argv) - 2 or flags.get("--suite") != suite \
                    or set(flags) - {"--enabled", "--suite", "--forge-fixture", "--forge-root", "--source-dir", "--build-dir", "--donors-root", "--plugin-store-backend"} \
                    or flags.get("--enabled") not in ("1", "ON", "on", "true", "TRUE", "yes", "YES") \
                    or flags.get("--forge-fixture") != str(paths.get("forge")) \
                    or flags.get("--forge-root") != str(forge_root) or flags.get("--source-dir") != str(source) \
                    or flags.get("--build-dir") != str(artifact_path.parent) \
                    or flags.get("--donors-root") != str(Path(inputs["donors_root"]).resolve()) \
                    or flags.get("--plugin-store-backend", "all") != selected:
                errors.append("live runner argv differs from recorded suite/configured binary/source selection")
    except (OSError, RuntimeError, ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        errors.append("configured-plugin artifact cannot be validated: " + str(error))
    return errors


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--forge-root", type=Path, required=True)
    parser.add_argument("--artifact", type=Path, required=True)
    parser.add_argument("--head", required=True)
    parser.add_argument("--suite", choices=SUITES, default="plugin-node")
    args = parser.parse_args()
    errors = validate(args.forge_root, args.artifact, args.head, args.suite)
    for error in errors:
        print(args.suite.upper() + " EVIDENCE ERROR: " + error, file=sys.stderr)
    if errors:
        return 1
    labels = {"plugin-node": "initial configured-plugin matrix", "plugin-dht": "configured-plugin DHT value selection",
              "plugin-pubsub": "configured-plugin PubSub selection", "plugin-relay": "configured-plugin AutoRelay selection",
              "plugin-autonat": "configured-plugin isolated AutoNAT selection"}
    print(labels[args.suite] + " observed; no Stage6 capability or whole Stage7 promotion"
          + ("; no raw extension/scoring promotion" if args.suite == "plugin-pubsub" else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
