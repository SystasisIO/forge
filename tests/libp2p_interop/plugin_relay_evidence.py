"""Official configuration/source checks composing unchanged AutoRelay validators."""

from copy import deepcopy
import json
from pathlib import Path
import re

from autorelay_acceptance import _actor_launch, _options, validate_record as validate_native_record
from autorelay_evidence import ECHO, validate_case as validate_native_case
from plugin_node_cases import listener_socket
from plugin_node_evidence import counter, load_json, require
from plugin_relay_cases import Case, case_root
from pubsub_evidence import _same_json


def configured_native_case(record):
    errors = validate_native_case(record, boundary="configured-plugin")
    if errors:
        return errors
    try:
        if record["case"]["kind"] == "lifecycle":
            _lifecycle_receipts(record)
        return []
    except (ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return ["configured Forge lifecycle receipts: " + str(error)]


def _direct_session(session, target, relay, local_address, remote_address):
    require(isinstance(session, dict) and session.get("source") == "official-node.diagnostics_api.session"
            and counter(session.get("session_id"), 1) and session.get("local_peer_id") == target
            and session.get("remote_peer_id") == relay and session.get("path") == "direct"
            and session.get("direction") == "outbound" and session.get("authentication") == "noise"
            and session.get("authentication_basis") == "native_session_upgrade_output"
            and session.get("muxer") == "/yamux/1.0.0" and session.get("identify_state") == "identified"
            and session.get("identified") is True and session.get("closed") is False
            and session.get("local_address") == local_address and session.get("remote_address") == remote_address,
            "seed/carrier lacks its actual authenticated outbound direct session")


def _lifecycle_receipts(native):
    raw = native["raw"]
    ready, result = raw["destination"]["ready"], raw["destination"]["result"]
    target = ready["peer_id"]
    require(result.get("local_peer_id") == target, "Forge local identity differs from its owned readiness")
    local_address = listener_socket(ready["listen_addrs"][0], target)
    seeds = result.get("seed_connections")
    require(isinstance(seeds, list) and len(seeds) == 2, "both Forge seed connection receipts are mandatory")
    carriers, carrier_ids = {}, set()
    rows = result["observations"]
    for role in ("relay", "replacement"):
        donor = raw[role]["ready"]
        relay = donor["peer_id"]
        matches = [seed for seed in seeds if seed.get("expected_peer_id") == relay]
        require(len(matches) == 1, "seed expected peer is not the actual unique donor relay identity")
        seed = matches[0]
        address = donor["listen_addrs"][0]
        require(seed.get("source") == "official-node.api.connect" and seed.get("dial_address") == address
                and counter(seed.get("unix_ms"), 1), "seed source/address/time differs from its actual donor")
        session = seed.get("session")
        _direct_session(session, target, relay, local_address, listener_socket(address, relay))
        identity = session["session_id"]
        require(identity not in carrier_ids, "relay/replacement seed reused a native carrier ID")
        require(any(row["unix_ms"] >= seed["unix_ms"] and any(
            observed.get("connection_id") == identity and observed.get("peer_id") == relay
            and observed.get("direct") is True and observed.get("identified") is True
            and observed.get("closed") is False for observed in row.get("sessions", [])) for row in rows),
            "seed carrier ID/peer was never observed in the live Forge snapshots")
        carriers[relay], carrier_ids = seed, carrier_ids | {identity}
    echoes = result.get("echoes")
    require(isinstance(echoes, list) and len(echoes) == 3, "all three completed Forge lifecycle echoes are mandatory")
    require(result.get("echo_admission_closed") is True and counter(result.get("pending_echoes"), 0, 0)
            and counter(result.get("admitted_echoes"), 3, 3), "Forge echo admission/callback work was not closed and drained")
    donors = {echo["phase"]: echo["result"] for echo in native["echoes"]}
    inner_ids, source_peers = set(), set()
    for phase in ("acquired_echo", "renewed_echo", "replacement_echo"):
        donor = donors[phase]
        peer, relay = donor["local_peer_id"], donor["relay_peer"]
        require(peer not in source_peers, "lifecycle donor echoes did not use independent source identities")
        # Every donor dialer owns a new identity. The observer peer is not the
        # source of these three incoming application streams.
        matches = [echo for echo in echoes if echo.get("session", {}).get("remote_peer_id") == peer]
        require(len(matches) == 1, "Forge echo source peer is missing, duplicated or differs from the actual donor")
        echo = matches[0]
        require(echo.get("source") == "official-node.protocol-handler" and echo.get("protocol") == ECHO
                and echo.get("response_write_completed") is True and echo.get("stream_closed") is True,
                "Forge lifecycle echo did not complete its actual response write and stream close")
        session = echo["session"]
        seed = carriers[relay]
        require(session.get("source") == "official-node.protocol-handler-and-diagnostics_api"
                and counter(session.get("session_id"), 1) and session["session_id"] not in inner_ids | carrier_ids
                and session.get("local_peer_id") == target and session.get("relay_peer_id") == relay
                and session.get("path") == "relay" and session.get("direction") == "inbound"
                and session.get("authentication") == session.get("stream_authentication") == "noise"
                and session.get("authentication_basis") == "native_inner_session_upgrade_output"
                and session.get("muxer") == "/yamux/1.0.0" and session.get("identify_state") == "identified"
                and session.get("identified") is True and session.get("closed") is False
                and "local_address" not in session and "remote_address" not in session,
                "Forge echo is not the actual authenticated incoming circuit session")
        require(counter(session.get("carrier_session_id"), 1)
                and session["carrier_session_id"] == seed["session"]["session_id"]
                and _same_json(session.get("carrier"), seed["session"]),
                "Forge echo carrier ID/peer/socket differs from its actual seed connection")
        require(session.get("circuit_address") == donor["relayed_addr"].split("/p2p-circuit")[0]
                + "/p2p-circuit/p2p/" + peer, "Forge incoming circuit differs from the donor's actual relay route")
        request, response = echo.get("request", {}), echo.get("response", {})
        require(isinstance(request, dict) and set(request) == {"bytes", "sha256", "contains_zero"}
                and counter(request.get("bytes"), donor["echo_bytes"], donor["echo_bytes"])
                and isinstance(request.get("sha256"), str) and re.fullmatch(r"[a-f0-9]{64}", request["sha256"])
                and type(request.get("contains_zero")) is bool and _same_json(request, response),
                "Forge actual request/response bytes/hash differ from each other or the donor byte count")
        require(counter(echo.get("unix_ms"), seed["unix_ms"], rows[-1]["unix_ms"]),
                "Forge completed echo receipt is outside its live seed/pre-stop interval")
        inner_ids.add(session["session_id"])
        source_peers.add(peer)


def _configuration(result, ready, spec):
    require(all(value.get("fixture_kind") == "configured-plugin" and value.get("store_backend") == spec.backend
                and value.get("configuration_accepted") is True for value in (result, ready)),
            "Forge AutoRelay is not the requested accepted official plugin")
    require(result.get("suite") == "plugin-node" and result.get("final") is True
            and result.get("status") == "ok" and result.get("errors") == []
            and _same_json(result.get("lifecycle"), {"plugins_joined": True, "scheduler_joined": True}),
            "official AutoRelay plugin/scheduler owners were not joined")
    require(result.get("operation_basis") == "official_shell_startup_authenticated_connect_only"
            and result.get("operation_source") == "official-shell.startup-and-node.api.connect"
            and result.get("observation_scope") == "after-official-startup-before-seeds-through-pre-stop; no-native-post-stop-snapshot",
            "configured observations escaped the official lifecycle/API boundary")
    require(_same_json(result.get("diagnostics_after_detach"), {
        "phase": "autorelay.post-join.diagnostics-detached", "error_category": "forge.plugins.net.p2p.node",
        "error_code": 1, "secret_material": "redacted", "source": "retained-official-node.diagnostics_api.snapshot",
        "rejected": True, "after_plugins_joined": True, "after_scheduler_joined": True, "native_snapshot_read": False}),
        "retained diagnostics did not actually reject after both official joins")
    events = result.get("host_events", {})
    generation = events.get("initial_generation")
    require(type(generation) is int and 0 <= generation <= 2**64 - 1 and _same_json(events, {
        "source": "official-node.events_api.host_events", "initial_consumed": True, "initial_generation": generation,
        "active_after_join": False, "read_after_join": None, "after_plugins_joined": True, "after_scheduler_joined": True}),
        "retained host-event subscription was not consumed/closed/drained after both joins")
    settings = result.get("accepted_configuration", {})
    require(settings.get("source") == "official-shell.accepted-document" and settings.get("secret_material") == "redacted"
            and settings.get("identity_source") == "Secrets.get_bytes" and settings.get("identity_key_type") == "ed25519"
            and settings.get("psk_source") == "absent" and settings.get("backend_fallback") is False,
            "missing redacted accepted configuration/Secrets identity")
    plugins = settings.get("document", {}).get("plugins", {})
    require("crypto" not in plugins and "PRIVATE KEY" not in json.dumps(settings)
            and "BEGIN CERTIFICATE" not in json.dumps(settings), "AutoRelay receipt exposes Secrets material/source")
    stores = plugins.get("db", {}).get("store", {}).get("stores")
    require(isinstance(stores, list) and len(stores) == 1 and stores[0].get("name") == "peers"
            and stores[0].get("driver") == spec.backend and isinstance(stores[0].get("path"), str)
            and stores[0].get("object") == {"family": "objectdb", "write-policy": "single-writer"},
            "AutoRelay did not accept the named ObjectDB backend")
    node = plugins.get("net", {}).get("p2p", {}).get("node", {})
    require(node.get("allow-insecure-test-mode") is False and node.get("peer-store", {}).get("store") == "peers"
            and node.get("identity") == {"certificate-secret": "identity/certificate", "private-key-secret": "identity/private-key"}
            and node.get("topology", {}).get("mode") == "static-only" and "private-network" not in node
            and node.get("listen") == ["/ip4/127.0.0.1/tcp/0"] and node.get("tcp", {}).get("security") == "noise",
            "accepted AutoRelay identity/static TCP Noise policy differs from its lane")
    relay = node.get("relay", {})
    client = spec.kind == "lifecycle"
    require(relay.get("client-enabled") is client and relay.get("server-enabled") is (not client)
            and relay.get("auto-discovery-enabled") is client,
            "accepted relay roles do not enable the exercised native manager/service")
    require(node.get("path", {}).get("policy") == ("relay-only" if client else "direct-only"),
            "accepted path policy disables the required native circuit behavior")
    if client:
        require(all(type(relay.get(key)) is int and relay[key] == value for key, value in {
            "target-reservations": 1, "max-candidates": 4, "max-parallel-reservations": 1,
            "refresh-margin-ms": 3000, "candidate-backoff-ms": 1000}.items()),
            "accepted AutoRelay manager bounds differ from the native fixture policy")
    else:
        require(type(relay.get("reservation-ttl-ms")) is int and relay["reservation-ttl-ms"] == 8000,
                "accepted relay service does not own the exercised eight-second leases")
    require(node.get("reachability", {}).get("client-v1-enabled") is False
            and node.get("reachability", {}).get("client-v2-enabled") is False
            and node.get("reachability", {}).get("service-v1-enabled") is False
            and node.get("reachability", {}).get("service-v2-enabled") is False,
            "uncontrolled AutoNAT changed the declared lease-lifecycle fixture")
    return stores[0]


def validate_case(record, spec):
    try:
        require(record.get("suite") == "plugin-relay" and record.get("scenario_id") == spec.scenario_id
                and _same_json(record.get("spec"), vars(spec)) and record.get("errors") == []
                and record.get("status", "observed") == "observed", "wrong configured AutoRelay selection")
        native = record["native"]
        require(native.get("suite") == "autorelay" and native.get("scenario_id") == spec.native.identifier
                and _same_json(native.get("case"), vars(spec.native)) and native.get("status") == "passed"
                and native.get("errors") == native.get("cleanup_errors") == []
                and native.get("acceptance_scenario_ids") == [], "native AutoRelay failed or carries raw promotion claims")
        errors = configured_native_case(native)
        require(not errors, "; ".join(errors))
        actor = native["raw"][spec.forge_role]
        _configuration(actor["result"], actor["ready"], spec)
        return []
    except (ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return ["plugin-relay evidence: " + str(error)]


def _load(path, work):
    path = Path(path)
    require(path.is_absolute() and path.resolve().is_relative_to(work) and str(path.resolve()) == str(path)
            and not path.is_symlink() and path.stat().st_size <= 1024 * 1024, "AutoRelay source escaped its owned case/bound")
    value = load_json(path)
    require(isinstance(value, dict), "AutoRelay source is not a JSON object")
    return value


def validate_terminal_sources(record, binaries, artifact_root):
    """After the existing raw case teardown; no new owner or provenance engine."""
    try:
        spec = Case(**record["spec"])
        root = case_root(artifact_root, spec)
        work = root / spec.native.identifier
        native = record["native"]
        actor = native["raw"][spec.forge_role]
        owner = actor["process"]
        store = work / (spec.forge_role + "-store")
        require(_same_json(record.get("forge_store"), {"source": "python.fixture.pre-spawn-store-check",
            "actor": spec.forge_role, "path": str(store), "existed_before_spawn": False}),
            "missing exact pre-spawn fresh Forge store witness")
        require(store.is_dir() and not store.is_symlink() and store.resolve() == store,
                "Forge ObjectDB store is missing or aliased")
        accepted_store = _configuration(actor["result"], actor["ready"], spec)
        require(accepted_store["path"] == str(store), "accepted ObjectDB path differs from its owned launcher")
        implementation, action, log, options = _actor_launch(spec.native, spec.forge_role, work, native["raw"])
        options["--store-backend"] = spec.backend
        require(implementation == "forge" and owner["command"][:2] == [str(binaries["forge"]), action]
                and _options(owner["command"]) == options and owner["log_file"] == str(log),
                "official Forge binary/CLI/backend changed")
        for process in native["processes"]:
            for output in process["outputs"]:
                require(output.get("exists") is True, "missing joined AutoRelay output capture")
                require(_same_json(_load(output["path"], work), _load(output["log_file"], work)),
                        "AutoRelay result/readiness differs from its post-join capture")
        # Validate the sole new Forge flag before adapting a copy to the raw
        # validator. Keep every original receipt/file and policy fact intact.
        view = deepcopy(native)
        projected = [view["raw"][spec.forge_role]["process"], *view["processes"], *view["attempts"]]
        matched = [process for process in projected if process.get("pid") == owner["pid"]]
        require(all(process["command"] == owner["command"] for process in matched),
                "Forge owner/attempt/backend argv disagrees")
        for process in matched:
            argv = process["command"]
            if "--store-backend" in argv:
                index = argv.index("--store-backend")
                process["command"] = argv[:index] + argv[index + 2:]
        errors = validate_native_record(view, spec.native, root, binaries, lambda path: _load(path, work),
                                        semantic_checker=configured_native_case)
        require(not errors, "; ".join(errors))
        return []
    except (OSError, RuntimeError, ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return ["plugin-relay terminal sources: " + str(error)]
