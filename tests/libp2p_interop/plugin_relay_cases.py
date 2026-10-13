"""Configured TCP AutoRelay selection over the existing native case owner."""

from dataclasses import asdict, dataclass
from pathlib import Path

from autorelay_cases import Case as NativeCase, case_specs as native_specs, run_case as native_case
from process_lifecycle import current_scope

BACKENDS = ("mdbx", "rocksdb")


@dataclass(frozen=True)
class Case:
    backend: str
    source: str
    relay: str
    destination: str
    kind: str
    transport: str = "tcp"

    def __post_init__(self):
        if self.backend not in BACKENDS or self.transport != "tcp" or self.native not in native_specs():
            raise ValueError("unknown configured AutoRelay case")

    @property
    def native(self):
        return NativeCase(self.source, self.relay, self.destination, self.transport, self.kind)

    @property
    def scenario_id(self):
        return f"plugin-relay/{self.backend}/{self.native.identifier}"

    @property
    def forge_role(self):
        return "destination" if self.kind == "lifecycle" else "relay"


def case_specs(backend):
    if backend not in BACKENDS:
        raise ValueError("configured AutoRelay requires an actual ObjectDB backend, no fallback")
    return tuple(Case(backend, spec.source, spec.relay, spec.destination, spec.kind)
                 for spec in native_specs() if spec.transport == "tcp")


def case_root(root, spec):
    return Path(root).resolve() / ("plugin-relay-" + spec.backend)


def run_case(spec, binaries, root, *, wait_json, command_attempt, start_destination, run_relay_dial):
    from plugin_relay_evidence import configured_native_case, validate_case, validate_terminal_sources

    if current_scope() is not None:
        raise RuntimeError("configured AutoRelay must use the existing top-level native case owner")
    fresh = []

    def forge_arguments(label, command, store):
        expected = case_root(root, spec) / spec.native.identifier / (spec.forge_role + "-store")
        if label != spec.forge_role or command != ("autorelay-destination" if spec.kind == "lifecycle" else "autorelay-service") \
                or store != expected or store.is_symlink() or store.exists() or fresh:
            raise ValueError("configured AutoRelay requires one new owned Forge store before spawn")
        fresh.append({"source": "python.fixture.pre-spawn-store-check", "actor": label,
                      "path": str(store), "existed_before_spawn": False})
        return ("--store-backend", spec.backend)

    native = native_case(spec.native, binaries, case_root(root, spec), wait_json=wait_json,
                         command_attempt=command_attempt, start_destination=start_destination,
                         run_relay_dial=run_relay_dial, claims_for_case=lambda _: [],
                         forge_cli_arguments=forge_arguments, semantic_checker=configured_native_case)
    if current_scope() is not None:
        raise RuntimeError("native AutoRelay case returned before its owned teardown")
    record = {"suite": "plugin-relay", "scenario_id": spec.scenario_id, "spec": asdict(spec),
              "native": native, "forge_store": fresh[0] if len(fresh) == 1 else None, "errors": []}
    errors = validate_case(record, spec)
    errors += validate_terminal_sources(record, binaries, root)
    record["errors"] = errors
    record["status"] = "observed" if not errors else "failed"
    return record
