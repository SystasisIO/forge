"""Configured boundary checks composing the mandatory independent AutoNAT proof."""

import json
from pathlib import Path

from autonat_acceptance import validate_pair as validate_native_pair
from autonat_evidence import _address, _connections, _object, validate_autonat_evidence
from plugin_autonat_cases import Case, case_root
from plugin_node_evidence import counter, load_json, require
from pubsub_evidence import _same_json


def configured_pair(client, server, **options):
    # Only this explicit caller selects the boundary. fixture_kind alone cannot
    # opt a payload out of the raw validator's stronger native-stop contract.
    errors = validate_autonat_evidence(client, server, forge_boundary="configured-plugin", **options)
    if errors:
        return errors
    try:
        forge = client if client["implementation"] == "forge" else server
        _snapshots(forge, options["version"], forge is server)
        if forge is client:
            _control(forge, options["expected_service_peer"])
        return []
    except (ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return ["configured AutoNAT snapshots: " + str(error)]


def _snapshots(result, version, service):
    require(counter(result.get("autonat_probe_api_calls"), 0, 0),
            "configured AutoNAT must use autonomous native startup, never a manual probe")
    snapshots = [_object(result.get(field)) for field in ("diagnostics_before_connect", "diagnostics_before_stop")]
    roles = {f"{role}_v{v}_enabled": service if role == "service" else not service
             for role in ("client", "service") for v in (version,)}
    roles.update({f"{role}_v{3 - version}_enabled": False for role in ("client", "service")})
    metric_keys = ("reachability_checks", "reachability_public", "reachability_private")
    for snapshot in snapshots:
        require(isinstance(snapshot, dict) and snapshot.get("source") == "official-node.diagnostics_api.snapshot"
                and snapshot.get("lifecycle_phase") in
                ("listening", "bootstrapping", "maintenance") and counter(snapshot.get("active_sessions")),
                "missing actual live official diagnostics snapshot")
        state = snapshot.get("reachability", {})
        require(all(state.get(key) is enabled for key, enabled in roles.items())
                and state.get("internet_egress_allowed") is True,
                "actual diagnostics replace or broaden the configured AutoNAT role")
        require(all(counter(state.get(key)) for key in ("pending_probes", "waiting_probes", "active_handlers", "probe_errors")),
                "actual reachability operational counters are missing or malformed")
        require(isinstance(snapshot.get("metrics"), dict)
                and all(counter(snapshot["metrics"].get(key)) for key in metric_keys),
                "actual reachability metrics are missing or malformed")
        require(not any(key in state for key in ("successful_probes", "probes_succeeded", "successful_probe_count")),
                "native API exposes no public successful-probe counter")
        require(isinstance(snapshot.get("resources"), dict) and all(counter(snapshot["resources"].get(key))
                for key in ("system_memory", "inbound_connections", "outbound_connections", "active_dials", "active_service_scopes")),
                "actual snapshot resource counters are missing or malformed")
        host = state.get("host", {})
        require(host.get("autonat_v1") == "unknown" and isinstance(host.get("autonat_v2"), list),
                "one-observer fixture must retain the default v1 quorum and actual v2 address scope")
        _host(host)
    initial, before = snapshots
    require(all(before["metrics"][key] >= initial["metrics"][key] for key in metric_keys),
            "native cumulative reachability metric went backwards")
    require(initial["active_sessions"] == 0 and all(initial["resources"][key] == 0 for key in
            ("inbound_connections", "outbound_connections", "active_dials"))
            and all(initial["reachability"][key] == 0 for key in
                    ("pending_probes", "waiting_probes", "active_handlers"))
            and all(initial["metrics"][key] == 0 for key in metric_keys),
            "before-connect snapshot is not a fresh idle official node")
    require(before["reachability"]["probe_errors"] >= initial["reachability"]["probe_errors"],
            "native cumulative probe-error counter went backwards")
    require(initial["reachability"]["probe_errors"] == before["reachability"]["probe_errors"] == 0,
            "configured fixture reports a native probe error, not clean paired success")
    if service:
        # These counters belong to outbound client exchanges, not served dialbacks.
        require(all(before["metrics"][key] == 0 for key in metric_keys),
                "service-only fixture reports unconfigured client reachability checks")
    else:
        require(before["reachability"]["pending_probes"] == before["reachability"]["waiting_probes"] == 0,
                "client pre-stop snapshot still has pending or waiting probes")
        require(all(before["metrics"][key] > initial["metrics"][key]
                    for key in ("reachability_checks", "reachability_public"))
                and before["metrics"]["reachability_private"] == initial["metrics"]["reachability_private"],
                "client snapshot lacks a completed public reachability check without private results")
    if not service and version == 1:
        require(result.get("v1_vote") is None and result.get("probe_state") == "unknown"
                and before["reachability"]["host"].get("effective") == "unknown",
                "paired v1 RPC/dialback must not fabricate a public native quorum vote")
    if not service and version == 2:
        state = _object(result.get("current_v2_address_state"))
        require(isinstance(state, dict) and state in before["reachability"]["host"]["autonat_v2"],
                "v2 public address is not present in the actual pre-stop native host snapshot")


def _host(host):
    require(isinstance(host, dict) and counter(host.get("generation")) and host.get("resync_required") is False
            and host.get("phase") in ("listening", "bootstrapping", "maintenance", "stopping", "stopped")
            and host.get("effective") in ("unknown", "public", "private", "blocked", "relay_only")
            and host.get("autonat_v1") == "unknown", "missing bounded actual host-event state")
    addresses = host.get("autonat_v2")
    require(isinstance(addresses, list) and len(addresses) <= 64 and all(
        isinstance(state, dict) and set(state) == {"address", "state"}
        and _address(state.get("address"), "tcp") is not None
        and state.get("state") in ("unknown", "public", "private", "blocked", "relay_only") for state in addresses),
        "host-event v2 address state is malformed or unbounded")


def _control(result, peer):
    errors = []
    connections = _connections(result, "actual_connections", "tcp", "Forge", errors)
    controls = [connection for connection in connections if connection["direction"] == "outbound"
                and connection["authenticated_peer"] == peer]
    session = result.get("control_session", {})
    require(not errors and len(controls) == 1 and session.get("source") == "official-node.api.connect-and-node.diagnostics_api.session"
            and counter(session.get("session_id"), 1) and session.get("local_peer_id") == result["local_peer_id"]
            and session.get("remote_peer_id") == peer and session.get("path") == "direct"
            and session.get("direction") == "outbound" and session.get("authentication") == "noise"
            and session.get("authentication_basis") == "native_session_upgrade_output"
            and session.get("muxer") == "/yamux/1.0.0" and session.get("identify_state") == "identified"
            and session.get("identified") is True and session.get("closed") is False
            and session.get("local_address") == controls[0]["local_addr"]
            and session.get("remote_address") == controls[0]["remote_addr"],
            "official control session differs from its actual authenticated gater peer/socket")


def _configuration(result, spec):
    require(result.get("fixture_kind") == "configured-plugin" and result.get("suite") == "plugin-node"
            and result.get("configuration_accepted") is True and result.get("store_backend") == spec.backend
            and result.get("final") is True and result.get("complete") is True
            and result.get("status") == "ok" and result.get("errors") == []
            and _same_json(result.get("lifecycle"), {"plugins_joined": True, "scheduler_joined": True}),
            "Forge did not accept/start/join the requested official AutoNAT plugin")
    require(result.get("protocol_basis") == "configured_selected_protocol_not_negotiation_capture"
            and result.get("operation_basis") == "official_shell_startup_authenticated_connect_only"
            and result.get("operation_source") == "official-shell.startup-and-node.api.connect"
            and result.get("observation_scope") == "after-official-startup-before-connect-through-pre-stop; no-native-post-stop-snapshot",
            "configured AutoNAT escaped the autonomous official API/observation boundary")
    require(_same_json(result.get("diagnostics_after_detach"), {
        "phase": "autonat.post-join.diagnostics-detached", "error_category": "forge.plugins.net.p2p.node",
        "error_code": 1, "secret_material": "redacted", "source": "retained-official-node.diagnostics_api.snapshot",
        "rejected": True, "after_plugins_joined": True, "after_scheduler_joined": True, "native_snapshot_read": False}),
        "retained diagnostics did not actually reject after both official joins")
    events = result.get("host_events", {})
    require(events.get("source") == "official-node.events_api.host_events" and events.get("initial_consumed") is True
            and counter(events.get("initial_generation")) and events.get("active_after_join") is False
            and "read_after_join" in events and events["read_after_join"] is None
            and events.get("after_plugins_joined") is True and events.get("after_scheduler_joined") is True,
            "retained official host-event subscription was not consumed/closed/drained after joins")
    require(events.get("initial", {}).get("generation") == events["initial_generation"],
            "initial host-event generation disagrees with its consumed event")
    queued = events.get("queued_events_drained_after_join")
    require(isinstance(queued, list) and len(queued) <= 64, "joined host-event capture is missing or unbounded")
    for host in (events.get("initial"), events.get("before_stop"), *queued):
        _host(host)
    settings = result.get("accepted_configuration", {})
    require(settings.get("source") == "official-shell.accepted-document" and settings.get("secret_material") == "redacted"
            and settings.get("identity_source") == "Secrets.get_bytes" and settings.get("identity_key_type") == "ed25519"
            and settings.get("psk_source") == "absent" and settings.get("backend_fallback") is False,
            "missing redacted accepted configuration/Secrets identity")
    plugins = settings.get("document", {}).get("plugins", {})
    require("crypto" not in plugins and "PRIVATE KEY" not in json.dumps(settings)
            and "BEGIN CERTIFICATE" not in json.dumps(settings), "AutoNAT receipt exposes Secrets material/source")
    stores = plugins.get("db", {}).get("store", {}).get("stores")
    require(isinstance(stores, list) and len(stores) == 1 and stores[0].get("name") == "peers"
            and stores[0].get("driver") == spec.backend and isinstance(stores[0].get("path"), str)
            and stores[0].get("object") == {"family": "objectdb", "write-policy": "single-writer"},
            "AutoNAT did not accept the named real ObjectDB backend")
    node = plugins.get("net", {}).get("p2p", {}).get("node", {})
    require(node.get("allow-insecure-test-mode") is False and node.get("peer-store", {}).get("store") == "peers"
            and node.get("identity") == {"certificate-secret": "identity/certificate", "private-key-secret": "identity/private-key"}
            and node.get("topology", {}).get("mode") == "static-only" and "private-network" not in node
            and "quic" not in node and node.get("tcp", {}).get("security") == "noise"
            and node.get("path", {}).get("policy") == "direct-only",
            "accepted AutoNAT lane differs from public static direct TCP Noise")
    require(all(node.get("relay", {}).get(key) is False for key in
                ("client-enabled", "server-enabled", "auto-discovery-enabled")),
            "AutoNAT receipt silently enables relay roles")
    policy = node.get("reachability", {})
    service = spec.forge_role == "server"
    require(all(policy.get(f"{role}-v{v}-enabled") is (v == spec.version and (service if role == "service" else not service))
                for role in ("client", "service") for v in (1, 2)), "accepted AutoNAT roles do not match the exercised API")
    require(counter(policy.get("min-reachability-observers", 3), 3, 3),
            "configured v1 fixture changed the native three-observer reachability quorum")
    require(counter(policy.get("min-observers", 4), 4, 4) and policy.get("ping-enabled") is False
            and all(node.get(group, {}).get("enabled") is False for group in ("pubsub", "mdns", "peer-exchange")),
            "configured AutoNAT changed native observer defaults or enabled uncontrolled services")
    return stores[0], node


def validate_case(record, spec):
    try:
        require(record.get("suite") == "plugin-autonat" and record.get("scenario_id") == spec.scenario_id
                and _same_json(record.get("spec"), vars(spec)) and record.get("errors") == []
                and record.get("status", "observed") == "observed", "wrong configured AutoNAT selection")
        require(_same_json(record.get("preflight"), {"source": "python.platform.system", "required_platform": "Linux",
                "observed_platform": "Linux", "status": "eligible", "reason": None}),
                "isolated Linux AutoNAT was NOT_RUN, not observed")
        native = record["native"]
        require(native.get("suite") == "autonat" and native.get("scenario_id") == spec.native.identifier
                and native.get("status") == "passed" and native.get("errors") == native.get("cleanup_errors") == []
                and native.get("acceptance_scenario_ids") == [], "native pair failed or carries raw Stage6 promotion claims")
        client, server = native["client"], native["server"]
        require(client.get("implementation") == spec.client and server.get("implementation") == spec.server,
                "configured client/service implementations were replaced")
        terminals = {attempt["kind"]: attempt["terminal_status"] for attempt in native["attempts"]}
        errors = configured_pair(client, server, version=spec.version, transport=spec.transport,
            expected_service_peer=native["expected_service_peer"], expected_requested_address=native["expected_requested_address"],
            client_terminal_status=terminals["client"], server_terminal_status=terminals["server"])
        require(not errors, "; ".join(errors))
        _configuration(native[spec.forge_role], spec)
        return []
    except (ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return ["plugin-autonat evidence: " + str(error)]


def _load(path, work):
    path = Path(path)
    require(path.is_absolute() and path.resolve().is_relative_to(work) and str(path.resolve()) == str(path)
            and not path.is_symlink() and path.stat().st_size <= 1024 * 1024,
            "AutoNAT source escaped its owned case/bound")
    value = load_json(path)
    require(isinstance(value, dict), "AutoNAT source is not a JSON object")
    return value


def validate_terminal_sources(record, binaries, artifact_root):
    """Bind original/captured sources and CLI through the unchanged raw verifier."""
    try:
        spec = Case(**record["spec"])
        root = case_root(artifact_root, spec)
        work = root / spec.native.identifier
        native = record["native"]
        store = work / (spec.forge_role + "-store")
        require(_same_json(record.get("forge_store"), {"source": "python.fixture.pre-spawn-store-check",
            "actor": spec.forge_role, "path": str(store), "existed_before_spawn": False}),
            "missing exact pre-spawn fresh Forge store witness")
        require(store.is_dir() and not store.is_symlink() and store.resolve() == store,
                "Forge ObjectDB store is missing or aliased")
        accepted, node = _configuration(native[spec.forge_role], spec)
        require(accepted["path"] == str(store), "accepted ObjectDB path differs from its owned launcher")
        owners = native["owned_processes"]
        for owner in owners:
            for output in owner["outputs"]:
                require(output.get("exists") is True and
                        _same_json(_load(output["path"], work), _load(output["log_file"], work)),
                        "AutoNAT result/readiness differs from its post-join capture")
        forge_owner = owners[1 if spec.forge_role == "client" else 0]
        if spec.forge_role == "server":
            ready = forge_owner["ready"]
            require(ready.get("fixture_kind") == "configured-plugin" and ready.get("configuration_accepted") is True
                    and ready.get("store_backend") == spec.backend
                    and _same_json(ready.get("accepted_configuration"), native["server"]["accepted_configuration"]),
                    "owned Forge readiness differs from its accepted terminal configuration")
        flags = dict(zip(forge_owner["command"][6::2], forge_owner["command"][7::2]))
        result = native[spec.forge_role]
        listener = result["requested_addr"] if spec.forge_role == "client" else forge_owner["ready"]["listen_addrs"][0]
        actual = listener.split("/p2p/")[0]
        reserved = spec.forge_role == "client" and spec.version == 2
        listen = actual if reserved else actual.rsplit("/", 1)[0] + "/0"
        require(node.get("listen") == [listen] and (node.get("advertised-endpoints") == [actual] if reserved else
                                                  "advertised-endpoints" not in node),
                "accepted public AutoNAT listener/candidate differs from its actual namespace endpoint")
        reservation = result.get("probe_port_reservation")
        require(_same_json(reservation, {"source": "boost.asio.tcp.acceptor.local_endpoint",
            "requested_address": "/ip4/11.0.0.3/tcp/0", "reserved_address": actual,
            "released_before_startup": True, "node_listener_proof": False}) if reserved else reservation is None,
            "v2 declarative port reservation is missing or mislabeled as a node listener proof")
        require(flags.get("--store-dir") == str(store) and flags.get("--store-backend") == spec.backend,
                "Forge backend/store CLI differs from accepted configuration")
        errors = validate_native_pair(native, spec.native, root, binaries, lambda path: _load(path, work), "", None,
            forge_cli_arguments=lambda role, owned_store: {"--store-backend": spec.backend}, semantic_checker=configured_pair)
        require(not errors, "; ".join(errors))
        return []
    except (OSError, RuntimeError, ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        return ["plugin-autonat terminal sources: " + str(error)]
