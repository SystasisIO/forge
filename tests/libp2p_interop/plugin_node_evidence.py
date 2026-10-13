"""Closed configured-plugin evidence, not Stage6 capability or readiness promotion."""

import hashlib
import json
from pathlib import Path
import re

from autorelay_wire import varint
from plugin_node_cases import Case, decoded_test_key_hash, listener_socket, socket
from private_profile_evidence import donor_observation, framed_receipt
from provenance import reject_duplicate_json_keys
from upgrade_evidence import observed_connections, connection_phases, negotiation
from rust_upgrade_evidence import _lifecycle as rust_task_lifecycle

ECHO = "/forge/interop/relay-echo/1"
AUTHENTICATION = {"tcp": "noise", "tcp-tls": "libp2p_tls", "quic": "quic_tls",
                  "tcp-pnet-noise": "noise", "tcp-pnet-tls": "libp2p_tls"}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def counter(value, minimum=0, maximum=2**64 - 1):
    return type(value) is int and minimum <= value <= maximum


def load_json(path):
    return json.loads(Path(path).read_text(), object_pairs_hook=reject_duplicate_json_keys,
                      parse_constant=lambda value: (_ for _ in ()).throw(ValueError("nonfinite JSON")))


def rust_build_environment(environment):
    """Receipt of explicit compilation inputs, not a dump of inherited secrets."""
    names = {
        "CARGO_NET_OFFLINE", "RUSTUP_OFFLINE", "CARGO_INCREMENTAL", "CARGO_BUILD_JOBS",
        "CARGO_BUILD_TARGET", "CARGO_BUILD_RUSTFLAGS", "CARGO_BUILD_RUSTDOCFLAGS",
        "RUSTUP_TOOLCHAIN", "RUSTC", "RUSTC_WRAPPER", "RUSTC_WORKSPACE_WRAPPER",
        "RUSTFLAGS", "CARGO_ENCODED_RUSTFLAGS", "RUSTDOCFLAGS", "CARGO_ENCODED_RUSTDOCFLAGS",
    }
    profile = r"CARGO_PROFILE_[A-Z0-9_]+_(?:DEBUG|DEBUG_ASSERTIONS|INCREMENTAL|OPT_LEVEL|OVERFLOW_CHECKS|LTO|CODEGEN_UNITS|PANIC|STRIP|RPATH|SPLIT_DEBUGINFO|BUILD_OVERRIDE_[A-Z_]+)"
    target = r"CARGO_TARGET_[A-Z0-9_]+_(?:RUSTFLAGS|LINKER)"
    result = {key: value for key, value in sorted(environment.items())
              if key in names or re.fullmatch(profile, key) or re.fullmatch(target, key)}
    require(all(isinstance(value, str) for value in result.values())
            and all(result.get(key) == "true" for key in ("CARGO_NET_OFFLINE", "RUSTUP_OFFLINE")),
            "configured-plugin Cargo receipt needs the actual offline compilation environment")
    return result


def accepted_configuration(payload, spec, role, expected_key_hash):
    require(payload.get("fixture_kind") == "configured-plugin" and payload.get("suite") == "plugin-node"
            and payload.get("configuration_accepted") is True and payload.get("store_backend") == spec.backend,
            "receipt is not a started official configured plugin with the requested backend")
    settings = payload.get("accepted_configuration", {})
    require(settings.get("source") == "official-shell.accepted-document" and settings.get("secret_material") == "redacted"
            and settings.get("identity_source") == "Secrets.get_bytes" and settings.get("identity_key_type") == "ed25519"
            and settings.get("backend_fallback") is False,
            "missing redacted official configuration receipt")
    document = settings.get("document", {})
    plugins = document.get("plugins", {})
    require("crypto" not in plugins, "Secrets source/material must never appear in the acceptance receipt")
    stores = plugins.get("db", {}).get("store", {}).get("stores")
    require(isinstance(stores, list) and len(stores) == 1 and stores[0].get("name") == "peers"
            and stores[0].get("driver") == spec.backend and isinstance(stores[0].get("path"), str)
            and stores[0].get("object") == {"family": "objectdb", "write-policy": "single-writer"},
            "accepted backend/family differs from the configured ObjectDB lane")
    node = plugins.get("net", {}).get("p2p", {}).get("node", {})
    require(node.get("allow-insecure-test-mode") is False and node.get("peer-store", {}).get("store") == "peers"
            and node.get("identity") == {"certificate-secret": "identity/certificate", "private-key-secret": "identity/private-key"}
            and node.get("path", {}).get("policy") == "direct-only"
            and node.get("topology", {}).get("mode") == "static-only"
            and node.get("relay") == {"client-enabled": False, "server-enabled": False, "auto-discovery-enabled": False},
            "accepted identity/security/direct-only policy is not the declared official lane")
    require(node.get("listen") == ["/ip4/127.0.0.1/udp/0/quic-v1" if spec.transport == "quic" else "/ip4/127.0.0.1/tcp/0"]
            and node.get("tcp", {}).get("security") == ("tls" if spec.transport.endswith("tls") else "noise"),
            "accepted transport selector differs from the lane")
    require(payload.get("psk_fingerprint") == expected_key_hash, "decoded test PSK hash differs from the owned launcher input")
    if expected_key_hash:
        require(settings.get("psk_source") == "Secrets.get_bytes" and node.get("private-network") == {
            "psk-secret": "network/psk", "internet-egress": "deny-external"}, "private PSK bypassed Secrets or changed egress")
    else:
        require(settings.get("psk_source") == "absent" and "private-network" not in node, "missing/public key control secretly configured PSK")
    if spec.dns and role == "dialer":
        require(node.get("dns", {}).get("resolution", {}).get("max-dns-lookups") == 2,
                "DNS lane did not accept its shared two-lookup budget")
    else:
        require("dns" not in node, "non-DNS lane has an undeclared resolver override")
    require("PRIVATE KEY" not in json.dumps(settings) and "BEGIN CERTIFICATE" not in json.dumps(settings),
            "receipt leaks identity material")
    return node


def plugin_exchange(payload, spec, role, listener_peer):
    require(payload.get("status") == "ok" and payload.get("errors") == [] and payload.get("final") is True,
            "plugin exchange is failed or pre-join mutable evidence")
    require(payload.get("lifecycle") == {"plugins_joined": True, "scheduler_joined": True}, "plugin owners were not both joined")
    return _plugin_exchange_facts(payload, spec, role, listener_peer)


def plugin_exchange_witness(payload, spec, listener_peer, role="listener"):
    """Pre-stop write/close facts only, never joined plugin acceptance."""
    require(payload.get("status") == "ok" and payload.get("errors") == [] and payload.get("final") is False
            and payload.get("configuration_accepted") is True
            and payload.get("lifecycle") == {"plugins_joined": False, "scheduler_joined": False},
            "missing actual pre-stop plugin completion witness")
    return _plugin_exchange_facts(payload, spec, role, listener_peer)


def _plugin_exchange_facts(payload, spec, role, listener_peer):
    exchange = payload.get("exchange", {})
    require(exchange.get("source") == ("official-node.protocol-api.open" if role == "dialer" else "official-node.protocol-handler")
            and exchange.get("protocol") == ECHO and exchange.get("stream_closed") is True
            and exchange.get("response_write_completed") is True, "no completed official plugin protocol exchange")
    expected = {"bytes": len(spec.payload), "sha256": hashlib.sha256(spec.payload).hexdigest(), "contains_zero": 0 in spec.payload}
    require(exchange.get("request") == expected and exchange.get("response") == expected,
            "actual binary request/response hashes or byte counts differ from the pinned CLI challenge")
    session = exchange.get("session", {})
    local, remote = session.get("local_peer_id"), session.get("remote_peer_id")
    require(session.get("source") == "node.protocol-api.stream-and-node.diagnostics-api.session"
            and counter(session.get("session_id"), 1) and local == payload.get("local_peer_id")
            and isinstance(local, str) and local and isinstance(remote, str) and remote and local != remote
            and session.get("identify_state") == "identified" and session.get("path") == "direct"
            and session.get("direction") == ("outbound" if role == "dialer" else "inbound")
            and session.get("authentication") == session.get("stream_authentication") == AUTHENTICATION[spec.transport],
            "Identify/authenticated stream is not bound to one actual admitted direct session")
    socket(session.get("local_address"), spec.transport == "quic")
    socket(session.get("remote_address"), spec.transport == "quic")
    if spec.transport != "quic":
        require(session.get("muxer") == "/yamux/1.0.0", "actual TCP session did not negotiate Yamux")
    else:
        require(isinstance(session.get("muxer"), str) and session["muxer"] != "/yamux/1.0.0", "QUIC was falsely labeled Yamux")
    require((remote if role == "dialer" else local) == listener_peer, "actual authenticated session differs from the owned listener")
    if role == "dialer":
        require(exchange.get("expected_peer_id") == listener_peer, "connect did not bind its expected peer")
    return exchange


def _go_dialer_facts(payload, spec, session, status):
    """Actual published connection/Identify facts, never invented upgrade phases."""
    connection, stream = payload.get("application_connection_id"), payload.get("application_stream_id")
    require(payload.get("implementation") == "go" and payload.get("role") == "dialer"
            and payload.get("scenario") == "echo_large" and payload.get("status") == status
            and payload.get("echo_ok") is True and counter(payload.get("payload_bytes"), 1)
            and payload["payload_bytes"] == len(spec.payload)
            and "upgrade_observation" not in payload
            and payload.get("protocol") == payload.get("application_protocol") == ECHO
            and isinstance(connection, str) and connection and isinstance(stream, str) and stream and stream != connection,
            "Go actual completed binary application exchange is missing")
    quic = spec.transport == "quic"
    local, remote = socket(payload.get("connection_local_addr"), quic), socket(payload.get("connection_remote_addr"), quic)
    require(payload.get("local_peer_id") == session["remote_peer_id"]
            and payload.get("authenticated_remote_peer_id") == session["local_peer_id"]
            and local == session["remote_address"] and remote == session["local_address"]
            and payload.get("negotiated_transport") == ("/quic-v1" if quic else "tcp"),
            "Go actual application identity/socket/transport disagrees with the plugin session")
    require(payload.get("identify_event_basis") == "automatic_identify_separate_exchange_same_connection"
            and payload.get("identify_event_connection_id") == connection and payload.get("signed_peer_record") is True,
            "Go automatic Identify is not bound to the actual application connection")
    if quic:
        # QUIC embeds TLS; the unchanged donor ConnState has no TCP upgrader IDs.
        require(payload.get("negotiated_security") == payload.get("negotiated_muxer") == "",
                "Go QUIC was falsely labeled a TCP security/muxer upgrade")
    else:
        require(payload.get("negotiated_security") == ("/tls/1.0.0" if spec.transport.endswith("tls") else "/noise")
                and payload.get("negotiated_muxer") == "/yamux/1.0.0", "Go actual TCP ConnState security/muxer disagrees")


def _go_dialer(payload, spec, session):
    _go_dialer_facts(payload, spec, session, "ok")
    if spec.go_completion_barrier:
        require(payload.get("final") is True and payload.get("echo_completion") == {
            "source": "go-fixture.echo-completion-barrier.v1", "mode": "stop-file", "phase": "host_closed",
            "stop_observed": True, "stream_close_completed": True, "host_close_completed": True}
            and all(type(payload["echo_completion"][key]) is bool for key in (
                "stop_observed", "stream_close_completed", "host_close_completed")) and "error" not in payload,
            "Go completion barrier has no checked post-stop host-close terminal receipt")
    else:
        require("echo_completion" not in payload, "Go default lane gained an undeclared completion barrier")


def go_completion_witness(payload, spec, session):
    require(spec.go_completion_barrier and payload.get("final") is False
            and payload.get("echo_completion") == {
                "source": "go-fixture.echo-completion-barrier.v1", "mode": "stop-file", "phase": "await_stop",
                "stop_observed": False, "stream_close_completed": False, "host_close_completed": False}
            and all(type(payload["echo_completion"][key]) is bool for key in (
                "stop_observed", "stream_close_completed", "host_close_completed")) and "error" not in payload,
            "Go interim exchange witness was relabeled terminal/closed evidence")
    _go_dialer_facts(payload, spec, session, "exchange_observed")


def go_completion_pair(endpoint, spec, session):
    witness, terminal = endpoint.get("exchange_witness"), endpoint.get("result")
    require(isinstance(witness, dict) and isinstance(terminal, dict), "missing separate Go exchange witness and terminal receipt")
    go_completion_witness(witness, spec, session)
    _go_dialer(terminal, spec, session)
    facts = lambda payload: {key: value for key, value in payload.items() if key not in ("status", "final", "echo_completion")}
    require(facts(witness) == facts(terminal), "Go terminal exchange identity/socket/Identify facts changed after the interim witness")


def go_listener_completion(payload, spec, session, *, final):
    require(spec.go_listener_completion and isinstance(payload, dict)
            and payload.get("implementation") == "go" and payload.get("role") == "listener"
            and payload.get("scenario") == "echo_large" and payload.get("protocol") == ECHO
            and payload.get("status") == "ok" and payload.get("final") is final and "error" not in payload,
            "missing successful opt-in Go listener completion")
    completion = payload.get("echo_listener_completion", {})
    require(completion == {"source": "go-fixture.echo-listener-completion.v1", "mode": "orderly-fin",
                           "request_eof_observed": True, "stream_close_completed": True, "host_close_completed": final}
            and all(type(completion[key]) is bool for key in
                    ("request_eof_observed", "stream_close_completed", "host_close_completed")),
            "Go listener did not observe actual EOF and checked close")
    require(counter(payload.get("payload_bytes"), 1) and payload["payload_bytes"] == len(spec.payload)
            and payload.get("payload_sha256") == hashlib.sha256(spec.payload).hexdigest(),
            "Go listener actual payload differs from the binary challenge")
    require(payload.get("local_peer_id") == session["remote_peer_id"]
            and payload.get("authenticated_remote_peer_id") == session["local_peer_id"]
            and socket(payload.get("connection_local_addr"), True) == session["remote_address"]
            and socket(payload.get("connection_remote_addr"), True) == session["local_address"],
            "Go listener stream identity/socket differs from the actual plugin session")
    connection, stream = payload.get("application_connection_id"), payload.get("application_stream_id")
    require(isinstance(connection, str) and connection and isinstance(stream, str) and stream and stream != connection,
            "Go listener has no actual stream/connection binding")


def go_listener_completion_pair(endpoint, spec, session):
    witness, terminal = endpoint.get("listener_witness"), endpoint.get("result")
    go_listener_completion(witness, spec, session, final=False)
    go_listener_completion(terminal, spec, session, final=True)
    facts = lambda payload: {key: value for key, value in payload.items() if key not in ("final", "echo_listener_completion")}
    require(facts(witness) == facts(terminal), "Go listener completion facts changed after the EOF witness")


def _rust_completion(payload, spec, session, *, final):
    require(spec.rust_completion_barrier and isinstance(payload, dict)
            and payload.get("implementation") == "rust" and payload.get("role") == "dialer"
            and payload.get("scenario") == "echo_large" and payload.get("protocol") == ECHO
            and payload.get("status") == ("ok" if final else "exchange_observed")
            and payload.get("final") is final and payload.get("echo_ok") is True
            and counter(payload.get("payload_bytes"), 1) and payload["payload_bytes"] == len(spec.payload)
            and "error" not in payload and "upgrade_observation" not in payload,
            "missing actual Rust QUIC completion-barrier exchange")
    completion = payload.get("echo_completion", {})
    expected = {"source": "rust-fixture.echo-completion-barrier.v1", "mode": "stop-file",
                "phase": "joined_after_swarm_drop" if final else "await_stop",
                "stop_observed": final, "stream_close_completed": final,
                "swarm_dropped": final, "fixture_owned_tasks_joined": final,
                "exchange_observations": 1, "stop_observations": int(final), "stream_close_completions": int(final)}
    require(completion == expected and all(type(completion.get(key)) is type(value) for key, value in expected.items()),
            "Rust completion snapshot is missing, relabeled or not joined")
    digest = {"bytes": len(spec.payload), "sha256": hashlib.sha256(spec.payload).hexdigest()}
    exchange = payload.get("exchange", {})
    require(exchange == {"source": "rust-fixture.actual-echo-io.v1", "write": digest, "read": digest}
            and counter(exchange["write"]["bytes"], 1) and counter(exchange["read"]["bytes"], 1),
            "Rust actual read/write differs from the full binary challenge")
    connection = payload.get("connection", {})
    identifier = connection.get("connection_id")
    require(connection.get("source") == "rust-libp2p.SwarmEvent.ConnectionEstablished"
            and isinstance(identifier, str) and identifier and connection.get("direction") == "outbound"
            and connection.get("local_peer_id") == session["remote_peer_id"]
            and connection.get("remote_peer_id") == session["local_peer_id"]
            and "local_address" in connection and connection["local_address"] is None,
            "Rust connection identity/event is missing or invents an unavailable local endpoint")
    address = connection.get("remote_address")
    actual_remote = listener_socket(address, session["local_peer_id"], True) if isinstance(address, str) and "/p2p/" in address \
        else socket(address, True)
    require(actual_remote == session["local_address"], "Rust actual remote QUIC address differs from the plugin listener")
    if final:
        # Existing fixture-owned tasks only. This is not proof of every internal
        # upstream QUIC task's graceful shutdown or the separate PR12 close gate.
        rust_task_lifecycle(payload, completion)
    else:
        require("fixture_task_lifecycle" not in payload, "Rust interim witness claims terminal task evidence")


def rust_completion_witness(payload, spec, session):
    _rust_completion(payload, spec, session, final=False)


def rust_completion_pair(endpoint, spec, session):
    witness, terminal = endpoint.get("exchange_witness"), endpoint.get("result")
    rust_completion_witness(witness, spec, session)
    _rust_completion(terminal, spec, session, final=True)
    facts = lambda payload: {key: value for key, value in payload.items()
                             if key not in ("status", "final", "echo_completion", "fixture_task_lifecycle")}
    require(facts(witness) == facts(terminal), "Rust terminal changed the independently recorded exchange/connection")


def _tcp_donor(payload, spec, role, session):
    implementation = spec.dialer if role == "dialer" else spec.listener
    local, remote = session["remote_peer_id"], session["local_peer_id"]
    security = "/tls/1.0.0" if spec.transport.endswith("tls") else "/noise"
    framed = varint(len(spec.payload)) + spec.payload
    direction = "outbound" if role == "dialer" else "inbound"
    if spec.private:
        if role == "dialer":
            require(framed_receipt(payload.get("request")) == framed_receipt(payload.get("response")) == spec.payload,
                    "private donor did not read and echo the actual binary argv challenge")
        donor_observation(payload, implementation, local, remote, security, ECHO, framed, framed, direction)
        return
    if implementation == "go" and role == "dialer":
        _go_dialer(payload, spec, session)
        return
    proof = payload.get("upgrade_observation", {})
    if implementation == "go":
        connections = observed_connections(proof)
    else:
        lifecycle = payload.get("fixture_task_lifecycle", {})
        require(proof.get("source") == "rust-libp2p.public-connection-upgrades.v1"
                and proof.get("finalized_after_swarm_drop") is True and proof.get("fixture_owned_tasks_joined") is True
                and proof.get("overflow") is False and lifecycle.get("fixture_owned_tasks_joined") is True
                and lifecycle.get("overflow") is False and lifecycle.get("errors") == [], "Rust donor trace/tasks not finalized")
        connections = proof.get("connections", [])
    require(isinstance(connections, list) and 1 <= len(connections) <= 16, "donor connection observation unbounded")
    matches = [c for c in connections if c.get("authenticated_local_peer_id") == local
               and c.get("authenticated_remote_peer_id") == remote]
    require(len(matches) == 1, "donor actual authenticated connection is absent or ambiguous")
    connection = matches[0]
    require(connection.get("selected_security") == security and connection.get("selected_muxer") == "/yamux/1.0.0"
            and connection.get("direction") == (direction.capitalize() if implementation == "go" else direction)
            and connection.get("local_address") == session["remote_address"]
            and connection.get("remote_address") == session["local_address"], "independent donor socket/security/identity disagrees")
    if implementation == "go":
        connection_phases(connection, security, direction.capitalize())
    else:
        require(connection.get("security_complete") is True and connection.get("muxer_complete") is True
                and connection.get("muxer_drop_observed") is True, "Rust native security/muxer delegates did not complete/join")
    streams = connection.get("streams", [])
    require(isinstance(streams, list) and len(streams) <= 32, "donor application trace unbounded")
    candidates = [s for s in streams if s.get("protocol") == ECHO
                  and s.get("direction") == (direction.capitalize() if implementation == "go" else direction)]
    require(len(candidates) == 1, "binary echo is not bound to one donor substream")
    stream = candidates[0]
    digest = hashlib.sha256(framed).hexdigest()
    if implementation == "go":
        require(stream.get("failed") is False and stream.get("reset") is False
                and stream.get("application_io_complete" if role == "dialer" else "response_write_complete") is True,
                "donor echo stream failed or never completed")
        negotiation(connection["events"], "application_multistream", stream["stream_trace_id"], ECHO, direction.capitalize())
        for side in ("read", "write"):
            require(stream.get(side + "_frames") == 1 and stream.get(side + "_framed_bytes") == len(framed)
                    and stream.get(side + "_framed_sha256") == digest, "donor actual binary frame differs")
    else:
        require(stream.get("parser_error") is None and stream.get("io_failed") is False and stream.get("drop_observed") is True,
                "Rust echo stream failed or retained its task")
        for side in ("read", "write"):
            value = stream.get(side, {})
            require(value.get("frames") == 1 and value.get("complete_frames") is True
                    and value.get("invalid_or_over_limit") is False and value.get("framed_bytes") == len(framed)
                    and value.get("framed_sha256") == digest, "Rust actual binary frame differs")


def negative_pair(record, spec, plugin_role):
    endpoints = record["endpoints"]
    plugin = endpoints[plugin_role]["result"]
    donor_role = "listener" if plugin_role == "dialer" else "dialer"
    donor = endpoints[donor_role]["result"]
    require(plugin.get("final") is True and plugin.get("errors") == []
            and plugin.get("lifecycle") == {"plugins_joined": True, "scheduler_joined": True}
            and plugin.get("status") == ("rejection_candidate" if plugin_role == "dialer" else "rejected")
            and plugin.get("exchange") is None, "private control crossed application boundary or did not join")
    for payload in (plugin, donor):
        require(payload.get("control_kind") == spec.control and payload.get("correlation_token") == spec.scenario_id,
                "private rejection does not bind the exact control and fresh process correlation")
    require(donor.get("status") == "rejected" and donor.get("rejected_before_identify") is True
            and donor.get("counter_source") == {"go": "go-libp2p.connection-gater", "rust": "rust-libp2p.swarm-events"}[
                endpoints[donor_role]["implementation"]]
            and counter(donor.get("attempted_connections"), 1, 4), "negative control never contacted the independent donor")
    for field in ("established_connections", "identify_streams", "application_streams"):
        require(counter(donor.get(field), 0, 0), "donor negative control crossed the security/Identify boundary")
    rejection = plugin.get("rejection", {})
    if plugin_role == "listener":
        require(rejection.get("source") == "node.diagnostics-api-and-configured-gater"
                and counter(rejection.get("attempted_connections"), 1, 4)
                and counter(rejection.get("handshakes_failed"), 1, 4), "plugin lacks ingress plus actual failed-handshake witness")
        for field in ("secured_callbacks", "handshakes_completed", "sessions_opened", "protocol_streams_opened",
                      "protocol_streams_accepted", "inbound_connections"):
            require(counter(rejection.get(field), 0, 0), "plugin control crossed security or failed to release inbound resources")
    else:
        # A typed failed connect alone is never evidence. The independent fresh
        # donor above must have observed ingress and no security completion.
        # The case admits exactly one direct endpoint. A global timeout cancels
        # that attempt without attributing a completed root failure; a rejected
        # decoded handshake or remote pre-auth close is an attributable root
        # failure. Owner cancellation and un-attributed closure never qualify.
        outcome = tuple(rejection.get(field) for field in ("error_code", "path_direct_attempts", "direct_failures"))
        require(rejection.get("source") == "node.connect-and-diagnostics-api"
                and rejection.get("error_category") == "forge.net.p2p"
                and all(type(value) is int for value in outcome)
                and outcome in ((12, 1, 0), (4, 1, 1), (14, 1, 1)),
                "outgoing PSK rejection is not a bounded native direct failure")
        for field in ("handshakes_completed", "sessions_opened", "protocol_streams_opened"):
            require(counter(rejection.get(field), 0, 0), "failed PSK dial secretly admitted a secure session")
    require(donor.get("expected_peer_id") == (record["peer_id"] if donor_role == "dialer" else ""),
            "donor negative dial target differs from the owned plugin listener")


def validate_case(record, spec):
    try:
        require(record.get("suite") == "plugin-node" and record.get("scenario_id") == spec.scenario_id
                and record.get("spec") == vars(spec), "case registration differs from the closed plugin-node matrix")
        endpoints = record.get("endpoints", {})
        require(set(endpoints) == {"listener", "dialer"}, "both independent endpoint owners are required")
        plugin_role = "dialer" if spec.dialer == "forge" else "listener"
        hashes = record.get("test_key_hashes", {})
        require(hashes.get("kind") == "sha256-decoded-public-test-key", "test-key digest provenance missing")
        if spec.private:
            require(isinstance(hashes.get("listener"), str) and re.fullmatch(r"[a-f0-9]{64}", hashes["listener"]),
                    "private listener has no actual decoded test-key hash")
            if spec.control == "missing_key":
                require(hashes.get("dialer") == "", "missing-key control actually supplied a key")
            elif spec.control == "mismatched_key":
                require(isinstance(hashes.get("dialer"), str) and re.fullmatch(r"[a-f0-9]{64}", hashes["dialer"])
                        and hashes["dialer"] != hashes["listener"], "mismatched-key control reused the correct key")
            else:
                require(hashes.get("dialer") == hashes["listener"], "positive PSK endpoints do not share the same decoded key")
        else:
            require(hashes.get("listener") == hashes.get("dialer") == "", "public lane contains an undeclared PSK")
        for role, implementation in (("dialer", spec.dialer), ("listener", spec.listener)):
            endpoint = endpoints[role]
            require(endpoint.get("implementation") == implementation, "endpoint implementation relabeled")
            process = endpoint.get("process", {})
            require(counter(process.get("pid"), 1) and process.get("terminal_status") == {"exit_code": 0, "termination": "graceful"},
                    "endpoint not gracefully joined (forced termination is never acceptance)")
            require(type(process["terminal_status"]["exit_code"]) is int, "boolean terminal status is not an exit code")
            payload = endpoint.get("result")
            if implementation == "forge":
                require(isinstance(payload, dict) and payload.get("schema_version") == 1
                        and payload.get("role") == role and payload.get("scenario") == spec.donor_scenario
                        and isinstance(payload.get("current_phase"), str) and payload["current_phase"], "missing final plugin receipt")
                accepted_configuration(payload, spec, role, record["test_key_hashes"][role])
            elif spec.control or spec.private or spec.transport != "quic" or role == "dialer":
                require(isinstance(payload, dict) and payload.get("implementation") == implementation
                        and payload.get("role") == role and payload.get("scenario") == spec.endpoint_scenario(implementation, role)
                        and payload.get("status") == ("rejected" if spec.control else "ok"), "missing independent donor receipt")
        plugin = endpoints[plugin_role]["result"]
        listener_ready = endpoints["listener"]["process"].get("ready", {})
        require(listener_ready.get("peer_id") == record.get("peer_id")
                and record.get("listen_address") in listener_ready.get("listen_addrs", []), "dial target not owned by the recorded listener")
        expected_socket = listener_socket(record["listen_address"], record["peer_id"], spec.transport == "quic")
        if spec.control:
            negative_pair(record, spec, plugin_role)
        else:
            exchange = plugin_exchange(plugin, spec, plugin_role, record["peer_id"])
            donor_role = "listener" if plugin_role == "dialer" else "dialer"
            donor = endpoints[donor_role]["result"]
            if spec.transport != "quic":
                _tcp_donor(donor, spec, donor_role, exchange["session"])
                if spec.go_completion_barrier:
                    go_completion_pair(endpoints[donor_role], spec, exchange["session"])
            elif donor_role == "dialer":
                if endpoints[donor_role]["implementation"] == "go":
                    go_completion_pair(endpoints[donor_role], spec, exchange["session"])
                else:
                    rust_completion_pair(endpoints[donor_role], spec, exchange["session"])
            elif spec.go_listener_completion:
                go_listener_completion_pair(endpoints[donor_role], spec, exchange["session"])
            if not spec.dns:
                require(exchange["session"]["remote_address" if plugin_role == "dialer" else "local_address"] == expected_socket,
                        "actual authenticated socket differs from the listener")
            if spec.dns:
                dns = record.get("dns", {}).get("result", {})
                root = dns.get("root")
                require(isinstance(root, str) and root.endswith("/p2p/" + record["peer_id"])
                        and exchange.get("dial_address") == root, "configured DNS input/expected peer differs")
                domain = root.split("/")[2]
                names = [f"_dnsaddr.{domain}", f"_dnsaddr.target.{domain}"]
                require(dns.get("records") == {names[0]: f"dnsaddr=/dnsaddr/target.{domain}/p2p/{record['peer_id']}",
                        names[1]: "dnsaddr=" + record["listen_address"]}, "DNS chain does not resolve to the owned listener")
                queries = dns.get("queries", [])
                require(isinstance(queries, list) and 2 <= len(queries) <= 4
                        and {q.get("name") for q in queries if q.get("type") == 16 and q.get("answered") is True} == set(names),
                        "configured resolver did not perform both actual bounded TXT lookups")
                nameserver = dns.get("nameserver", "").split(":")
                node = plugin["accepted_configuration"]["document"]["plugins"]["net"]["p2p"]["node"]
                require(len(nameserver) == 2 and node["dns"]["resolver"]["nameservers"] == [
                    {"address": nameserver[0], "port": int(nameserver[1])}], "accepted resolver is not the owned DNS server")
                require(exchange["session"]["remote_address"] == expected_socket,
                        "resolved candidate differs from the actual authenticated numeric connection")
        return []
    except (ValueError, TypeError, KeyError, AttributeError, IndexError) as error:
        return ["plugin-node evidence: " + str(error)]


def validate_terminal_sources(record, binary_paths, artifact_root, source_dir):
    """Bind every final payload to its own command and captured post-join file."""
    errors = []
    try:
        spec = Case(**record["spec"])
        owners = record.get("owned_processes")
        require(isinstance(owners, list) and len(owners) == 2
                and len({owner.get("pid") for owner in owners}) == 2,
                "case must own exactly two distinct joined processes")
        for role, endpoint in record["endpoints"].items():
            process = endpoint["process"]
            require(sum(owner == process for owner in owners) == 1, "endpoint view differs from its unique case process owner")
            argv = process["command"]
            require(argv[0] == str(binary_paths[endpoint["implementation"]]) and argv[1] == ("dial" if role == "dialer" else "listen"),
                    "endpoint command differs from the hashed executable/role")
            require(len(argv) >= 4 and len(argv) % 2 == 0, "malformed endpoint argv")
            options = dict(zip(argv[2::2], argv[3::2]))
            require(len(options) * 2 == len(argv) - 2 and options.get("--transport") == spec.transport
                    and options.get("--scenario") == spec.endpoint_scenario(endpoint["implementation"], role),
                    "duplicate or relabeled endpoint flags")
            barrier = spec.completion_barrier and role == "dialer"
            require(options.get("--echo-completion-barrier") == ("stop-file" if barrier else None),
                    "donor completion synchronization differs from its exact opt-in lane")
            if barrier:
                witness_path = Path(endpoint["exchange_witness_file"]).resolve()
                witness_path.relative_to(artifact_root)
                require(options.get("--ready-file") == str(witness_path), "Go interim witness source differs from its launcher")
                captures = [entry for entry in process["outputs"] if entry.get("argument") == "--ready-file"
                            and entry.get("path") == str(witness_path) and entry.get("exists") is True]
                require(len(captures) == 1, "Go interim witness has no unique owned terminal capture")
                captured = Path(captures[0]["log_file"]).resolve()
                captured.relative_to(artifact_root)
                require(load_json(witness_path) == load_json(captured) == endpoint["exchange_witness"],
                        "Go interim witness differs from its actual owned file/capture")
            else:
                require("exchange_witness" not in endpoint and "exchange_witness_file" not in endpoint,
                        "default lane contains an undeclared Go exchange witness")
            orderly = spec.go_listener_completion and role == "listener"
            require(options.get("--echo-listener-completion") == ("orderly-fin" if orderly else None),
                    "Go listener completion differs from its exact opt-in lane")
            if orderly:
                witness_path = Path(endpoint["listener_witness_file"]).resolve()
                witness_path.relative_to(artifact_root)
                require(options.get("--echo-witness-file") == str(witness_path), "listener EOF witness differs from launcher")
                captures = [entry for entry in process["outputs"] if entry.get("argument") == "--echo-witness-file"
                            and entry.get("path") == str(witness_path) and entry.get("exists") is True]
                require(len(captures) == 1, "listener EOF witness lacks its owned capture")
                captured = Path(captures[0]["log_file"]).resolve()
                captured.relative_to(artifact_root)
                require(load_json(witness_path) == load_json(captured) == endpoint["listener_witness"],
                        "listener EOF witness differs from the captured source")
            else:
                require("--echo-witness-file" not in options and "listener_witness" not in endpoint
                        and "listener_witness_file" not in endpoint, "undeclared listener EOF witness")
            for field in ("--store-dir", "--stop-file", "--result-file", "--ready-file", "--echo-witness-file"):
                if field in options:
                    Path(options[field]).resolve().relative_to(artifact_root)
            input_key = Path(options["--pnet-key-file"]) if "--pnet-key-file" in options else None
            require(decoded_test_key_hash(input_key) == record["test_key_hashes"][role],
                    "reported test-key hash differs from the actual launcher input")
            if spec.private:
                expected_name = "mismatched-swarm.key" if role == "dialer" and spec.control == "mismatched_key" else "swarm.key"
                if role == "dialer" and spec.control == "missing_key":
                    require(input_key is None, "missing-key launcher secretly supplied PSK")
                else:
                    require(input_key is not None and input_key.resolve() == (source_dir / "fixtures/pnet" / expected_name).resolve(),
                            "private launcher did not select its pinned test-key fixture")
            else:
                require(input_key is None, "public launcher supplied an undeclared PSK")
            if role == "dialer":
                expected_address = record["dns"]["result"]["root"] if spec.dns else record["listen_address"]
                require(options.get("--peer-id") == record["peer_id"] and options.get("--addr") == expected_address,
                        "dial source target differs from the independent listener/DNS chain")
            if endpoint["implementation"] == "forge":
                require(options.get("--store-backend") == spec.backend, "backend selector differs from the accepted document")
                doc = endpoint["result"]["accepted_configuration"]["document"]
                require(doc["plugins"]["db"]["store"]["stores"][0]["path"] == options.get("--store-dir"), "accepted store path not owned by launcher")
            if spec.control:
                require(options.get("--pnet-control") == spec.control and options.get("--pnet-correlation") == spec.scenario_id,
                        "negative source argv is not correlated")
            if role == "listener":
                ready = Path(options["--ready-file"]).resolve()
                captures = [entry for entry in process["outputs"] if entry.get("argument") == "--ready-file"
                            and entry.get("path") == str(ready) and entry.get("exists") is True]
                require(len(captures) == 1, "listener readiness has no unique owned terminal capture")
                captured = Path(captures[0]["log_file"]).resolve()
                captured.relative_to(artifact_root)
                require(load_json(ready) == load_json(captured) == process["ready"],
                        "inline listener identity/address differs from the actual ready-file capture")
            payload = endpoint.get("result")
            if payload is not None:
                original = Path(endpoint["result_file"]).resolve()
                original.relative_to(artifact_root)
                require(options.get("--result-file") == str(original), "result source differs from its launcher")
                matches = [entry for entry in process["outputs"] if entry.get("argument") == "--result-file"
                           and entry.get("path") == str(original) and entry.get("exists") is True]
                require(len(matches) == 1, "final result has no unique terminal-owned capture")
                captured = Path(matches[0]["log_file"]).resolve()
                captured.relative_to(artifact_root)
                require(load_json(original) == load_json(captured) == payload, "pre-stop/in-memory result differs from its final owned capture")
        if spec.dns:
            source = Path(record["dns"]["evidence_file"]).resolve()
            source.relative_to(artifact_root)
            require(load_json(source) == record["dns"]["result"], "DNS raw source differs from inline evidence")
    except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError) as error:
        errors.append("plugin-node sources: " + str(error))
    return errors
