"""Closed official-plugin AutoNAT selection over the native process/network owner."""

from dataclasses import asdict, dataclass
from pathlib import Path
import platform

from autonat_cases import Case as NativeCase, run_case as native_case
from process_lifecycle import current_scope

BACKENDS = ("mdbx", "rocksdb")
PAIRS = (("forge", "rust", 1), ("rust", "forge", 1),
         ("forge", "go", 2), ("go", "forge", 2))


@dataclass(frozen=True)
class Case:
    backend: str
    client: str
    server: str
    version: int
    transport: str = "tcp"

    def __post_init__(self):
        if self.backend not in BACKENDS or self.transport != "tcp" or type(self.version) is not int \
                or (self.client, self.server, self.version) not in PAIRS:
            raise ValueError("unknown configured AutoNAT case; only public TCP Noise v1 Rust/v2 Go")

    @property
    def native(self):
        return NativeCase(self.client, self.server, self.version, self.transport)

    @property
    def scenario_id(self):
        return f"plugin-autonat/{self.backend}/{self.native.identifier}"

    @property
    def forge_role(self):
        return "client" if self.client == "forge" else "server"


def case_specs(backend):
    if backend not in BACKENDS:
        raise ValueError("configured AutoNAT requires an actual ObjectDB backend, no fallback")
    return tuple(Case(backend, *pair) for pair in PAIRS)


def case_root(root, spec):
    return Path(root).resolve() / ("plugin-autonat-" + spec.backend)


def platform_preflight():
    """Only an eligibility check; the existing owner verifies actual isolation."""
    system = platform.system()
    return {"source": "python.platform.system", "required_platform": "Linux", "observed_platform": system,
            "status": "eligible" if system == "Linux" else "not_run",
            "reason": None if system == "Linux" else "NOT_RUN: isolated configured AutoNAT requires Linux; no host-network fallback"}


def not_run_record(spec, preflight):
    return {"suite": "plugin-autonat", "scenario_id": spec.scenario_id, "spec": asdict(spec),
            "status": "not_run", "preflight": dict(preflight), "native": None,
            "forge_store": None, "errors": [preflight["reason"]]}


def run_case(spec, binaries, root, *, wait_json, command_attempt):
    from plugin_autonat_evidence import configured_pair, validate_case, validate_terminal_sources

    if current_scope() is not None:
        raise RuntimeError("configured AutoNAT requires its existing top-level native case owner")
    preflight = platform_preflight()
    if preflight["status"] != "eligible":
        return not_run_record(spec, preflight)
    fresh = []

    def forge_arguments(role, store):
        expected = case_root(root, spec) / spec.native.identifier / (spec.forge_role + "-store")
        if role != spec.forge_role or store != expected or store.is_symlink() or store.exists() or fresh:
            raise ValueError("configured AutoNAT requires one new owned Forge store before spawn")
        fresh.append({"source": "python.fixture.pre-spawn-store-check", "actor": role,
                      "path": str(store), "existed_before_spawn": False})
        return ("--store-backend", spec.backend)

    # Public-only cases never consume either private-network argument. Keep the
    # native owner and its unchanged cleanup/capture order rather than wrapping it.
    native = native_case(spec.native, binaries, case_root(root, spec), pnet_key=None, pnet_fingerprint="",
                         wait_json=wait_json, command_attempt=command_attempt,
                         forge_cli_arguments=forge_arguments, semantic_checker=configured_pair)
    if current_scope() is not None:
        raise RuntimeError("native AutoNAT returned before its owned teardown")
    native["acceptance_scenario_ids"] = []
    record = {"suite": "plugin-autonat", "scenario_id": spec.scenario_id, "spec": asdict(spec),
              "preflight": preflight, "native": native,
              "forge_store": fresh[0] if len(fresh) == 1 else None, "errors": []}
    errors = validate_case(record, spec) + validate_terminal_sources(record, binaries, root)
    record["errors"] = errors
    record["status"] = "observed" if not errors else "failed"
    return record
