"""Pure, fail-closed cross-process AutoNAT result checks.

The caller owns artifact hashes, launcher/result binding, network isolation and
closed-port setup. Pass both immutable fixture results and independently observed
process terminal statuses. This validator performs no I/O and proves neither
Internet NAT traversal nor raw nonce bytes hidden by donor/public APIs.
"""

from __future__ import annotations

import ipaddress
import json
from typing import Any


PROTOCOLS = {1: "/libp2p/autonat/1.0.0", 2: "/libp2p/autonat/2/dial-request"}
REVISIONS = {
    "go": "9cfe2cc00be5b20a0be737f002c99f81b92255c5",
    "rust": "22fb4c784fc55ad8b15d05fdc9f98d663107d4cb",
}
GO_API_BASIS = {
    1: "NewAutoNATClient.DialBack + bounded_copy_of_actual_v1_response",
    2: "autonatv2.New/Start/GetReachability: donor_verified_result",
}
RUST_API_BASIS = {
    1: "v1::Behaviour::OutboundProbe::Response: donor_verified_result",
    2: "v2::client::Behaviour::Event: donor_verified_result",
}
# Accepted only for Forge client -> Go service v1. This basis makes no claim
# that Forge observed an inbound gater event (TLS can complete before that hook).
FORGE_GO_V1_PROOF_BASIS = "go_independent_authenticated_probe_and_forge_native_public_vote"


def _string(value: object, limit: int = 512) -> bool:
    return (
        isinstance(value, str) and 0 < len(value) <= limit
        and not any(c.isspace() for c in value)
    )


def _integer(value: object, minimum: int = 0, maximum: int = 128) -> bool:
    return type(value) is int and minimum <= value <= maximum


def _address(value: object, transport: str, peer: object = None,
             *, outbound_quic_bind: bool = False) -> str | None:
    if not _string(value):
        return None
    parts = value.split("/")
    if len(parts) >= 3 and parts[-2] == "p2p":
        if not _string(peer, 256) or parts[-1] != peer:
            return None
        parts = parts[:-2]
    expected = 6 if transport == "quic" else 5
    if len(parts) != expected or parts[0] or parts[1] not in ("ip4", "ip6"):
        return None
    if parts[3] != ("udp" if transport == "quic" else "tcp"):
        return None
    if transport == "quic" and parts[5] != "quic-v1":
        return None
    try:
        ip = ipaddress.ip_address(parts[2])
        port = int(parts[4])
    except ValueError:
        return None
    if parts[1] != f"ip{ip.version}" or str(port) != parts[4] or not 0 < port < 65536:
        return None
    wildcard = outbound_quic_bind and transport == "quic" and ip.is_unspecified
    if (
        (not wildcard and (not ip.is_global or ip.is_reserved))
        or ip.is_multicast or "%" in parts[2]
    ):
        return None
    if ip.version == 6 and not wildcard and (
        ip not in ipaddress.ip_network("2000::/3")
        or ip in ipaddress.ip_network("2002::/16")
        or ip in ipaddress.ip_network("2001::/32")
    ):
        return None
    return f"/ip{ip.version}/{ip}/{parts[3]}/{port}" + ("/quic-v1" if transport == "quic" else "")


def _wildcard(value: str | None) -> bool:
    return value is not None and value.split("/")[2] in ("0.0.0.0", "::")


def _local_matches(local: str | None, observed_remote: str) -> bool:
    if not _wildcard(local):
        return local == observed_remote
    # Preserve the actual unspecified bind. The other process supplies the IP;
    # family, transport and nonzero UDP port must still match exactly.
    left, right = local.split("/"), observed_remote.split("/")
    return not _wildcard(observed_remote) and left[1] == right[1] and left[3:] == right[3:]


def _corroborate_wildcards(records: list[dict], counterpart: list[dict],
                           owner_peer: str, remote_peer: str, errors: list[str]) -> None:
    for record in records:
        if _wildcard(record["local_addr"]) and not any(
            record["direction"] == "outbound" and record["authenticated_peer"] == remote_peer
            and other["direction"] == "inbound" and other["authenticated_peer"] == owner_peer
            and record["remote_addr"] == other["local_addr"]
            and _local_matches(record["local_addr"], other["remote_addr"])
            for other in counterpart
        ):
            errors.append("outbound QUIC wildcard bind lacks exact authenticated concrete counterpart")


def _unique_pairs(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate JSON field")
        result[key] = value
    return result


def _object(value: object) -> dict[str, Any] | None:
    if isinstance(value, str) and len(value) <= 32768:
        try:
            value = json.loads(value, object_pairs_hook=_unique_pairs)
        except (ValueError, RecursionError):
            return None
    return value if isinstance(value, dict) else None


def _terminal(value: object, label: str, errors: list[str]) -> None:
    if not isinstance(value, dict) or set(value) != {"exit_code", "termination"} or not (
        type(value.get("exit_code")) is int and value["exit_code"] == 0
        and value.get("termination") == "graceful"
    ):
        errors.append(f"{label} requires independent exit 0 without termination escalation")


def _bounded_event(value: object) -> bool:
    if not isinstance(value, dict):
        return False
    try:
        encoded = json.dumps(value, separators=(",", ":"), ensure_ascii=False, allow_nan=False)
        return len(encoded.encode("utf-8")) <= 8192
    except (TypeError, ValueError, RecursionError, UnicodeError):
        return False


def _dial_data(value: object) -> bool:
    return _integer(value, 0, 0) or _integer(value, 30000, 100000)


def _rust_events(result: dict, version: int, transport: str, requested: str,
                 client_peer: str, service_peer: str, control_source: str | None,
                 errors: list[str]) -> None:
    role = result.get("role")
    events = result.get("donor_events")
    if not isinstance(events, list) or len(events) > 64 or any(not _bounded_event(e) for e in events):
        return  # The common trace check reports this failure.
    client = role == "dialer"
    protocol = PROTOCOLS[version]
    semantic = [e for e in events if e.get("protocol") in PROTOCOLS.values()]
    if version == 1:
        request_kinds = ("v1_outbound_request", "v1_inbound_request")
        paired_kinds = request_kinds + ("v1_outbound_response", "v1_inbound_response")
        correlated_ids = [e.get("probe_id") for e in semantic if e.get("kind") in paired_kinds]
        first_request = next((i for i, e in enumerate(semantic)
                              if e.get("kind") in request_kinds), len(semantic))
        # A preliminary donor selection miss never opened a request. Do not
        # excuse a failure of the active ProbeId, or infer NoServer from Debug.
        semantic = [e for i, e in enumerate(semantic) if not (
            i < first_request and e.get("protocol") == protocol
            and e.get("kind") == "v1_outbound_error" and e.get("error_kind") == "NoServer"
            and "peer" in e and e["peer"] is None and e.get("success") is False
            and _string(e.get("probe_id"), 256) and e["probe_id"] not in correlated_ids
        )]
    if any(e.get("protocol") != protocol or e.get("success") is False for e in semantic):
        errors.append(f"{role} Rust trace contradicts the successful protocol result")
    if version == 1:
        prefix = "v1_outbound" if client else "v1_inbound"
        probes = [e for e in semantic if e.get("kind") != "v1_status_changed"]
        requests = [e for e in probes if e.get("kind") == prefix + "_request"]
        responses = [e for e in probes if e.get("kind") == prefix + "_response"]
        expected_count = 1 if client else 0
        if (
            len(probes) != 2 or len(requests) != 1 or len(responses) != 1
            or not _integer(result.get("v1_request_events"), expected_count, expected_count)
            or not _integer(result.get("v2_result_events"), 0, 0)
        ):
            errors.append(f"{role} Rust v1 requires one ordered typed Request/Response pair and coherent counters")
            return
        request, response = requests[0], responses[0]
        peer = service_peer if client else client_peer
        if (
            probes != [request, response] or not _string(request.get("probe_id"), 256)
            or request.get("probe_id") != response.get("probe_id")
            or request.get("peer") != peer or response.get("peer") != peer
            or response.get("success") is not True
            or _address(response.get("address"), transport, client_peer) != requested
        ):
            errors.append(f"{role} Rust v1 event peer/ProbeId/address correlation failed")
        if not client:
            addresses = request.get("addresses")
            if (
                not isinstance(addresses, list) or not 1 <= len(addresses) <= 16
                or requested not in [_address(a, transport, client_peer) for a in addresses]
                or any(_address(a, transport, client_peer) is None for a in addresses)
            ):
                errors.append("listener Rust v1 Request lacks the requested dialback address")
        return
    kind = "v2_client_result" if client else "v2_server_result"
    if not semantic or any(e.get("kind") != kind for e in semantic):
        errors.append(f"{role} Rust v2 lacks typed result events for its role")
        return
    if (
        not _integer(result.get("v1_request_events"), 0, 0)
        or not _integer(result.get("v2_result_events"),
                        len(semantic) if client else 0, len(semantic) if client else 0)
    ):
        errors.append(f"{role} Rust v2 event counters disagree with the trace")
    for index, event in enumerate(semantic):
        amount = event.get("bytes_sent" if client else "data_amount")
        tested = _address(event.get("tested_addr"), transport, client_peer)
        if (
            event.get("success") is not True
            or event.get("server" if client else "client") != (service_peer if client else client_peer)
            or tested is None
            or ((client or index == len(semantic) - 1) and tested != requested)
            or not _dial_data(amount)
            or (client and amount != result.get("dial_data_bytes"))
        ):
            errors.append(f"{role} Rust v2 event peer/address/success/dial-data correlation failed")
        if tested is not None and control_source is not None:
            # Rust's service charges for any changed endpoint; Forge's service
            # requires payment for a changed IP. Use the independently paired
            # control socket, never an uncorroborated client-reported source.
            payment_required = (tested.split("/")[2] != control_source.split("/")[2]
                                if client else tested != control_source)
            if payment_required and not _integer(amount, 30000, 100000):
                errors.append(f"{role} Rust v2 lacks required dial-data payment")


def _connections(result: dict, field: str, transport: str, label: str, errors: list[str],
                 *, v1_authenticated_inbound: bool = False) -> list[dict]:
    implementation = result["implementation"]
    values = result.get(field)
    limit = 64 if implementation == "forge" else 128
    if not isinstance(values, list) or len(values) > limit:
        errors.append(f"{label}.{field} is missing or exceeds its trace bound")
        return []
    fields = {"direction", "authenticated", "authenticated_peer", "local_addr", "remote_addr"}
    fields |= {"stage"} if implementation == "forge" else {"connection_id"}
    if implementation == "rust":
        fields |= {"fresh_requested_inbound", "closed"}
    records = []
    identifiers = set()
    for entry in values:
        if not isinstance(entry, dict) or set(entry) != fields:
            errors.append(f"{label}.{field} has an invalid connection shape")
            continue
        if (
            entry["authenticated"] is not True
            or not _string(entry["authenticated_peer"], 256)
            or entry["direction"] not in ("inbound", "outbound")
        ):
            errors.append(f"{label}.{field} has unauthenticated or invalid direction evidence")
            continue
        remote = _address(entry["remote_addr"], transport, entry["authenticated_peer"])
        local_peer = result.get("peer_id") if implementation == "rust" else result.get("local_peer_id")
        local = _address(
            entry["local_addr"], transport, local_peer,
            outbound_quic_bind=implementation in ("forge", "go") and entry["direction"] == "outbound",
        )
        hidden_local = (
            implementation == "rust" and entry["direction"] == "outbound"
            and entry["local_addr"] is None
        )
        if remote is None or (local is None and not hidden_local):
            errors.append(f"{label}.{field} requires public numeric transport addresses")
            continue
        if implementation == "forge":
            if entry["stage"] not in ("secured", "upgraded"):
                errors.append(f"{label}.{field} has an invalid gater stage")
                continue
            if entry["stage"] != "upgraded":
                # Rust v1 answers after its fresh authenticated dial succeeds;
                # the receiver need not finish its own muxer upgrade first.
                # Keep the real secured stage, once, only for that paired proof.
                if not (v1_authenticated_inbound and entry["direction"] == "inbound"
                        and dict(entry, stage="upgraded") not in values):
                    continue
            elif dict(entry, stage="secured") not in values:
                errors.append(f"{label}.{field} upgrade lacks matching authentication")
        else:
            identifier = entry["connection_id"]
            if not _string(identifier, 256) or identifier in identifiers:
                errors.append(f"{label}.{field} has a missing or duplicate connection id")
                continue
            identifiers.add(identifier)
        if implementation == "rust" and (
            entry["closed"] is not True or type(entry["fresh_requested_inbound"]) is not bool
        ):
            errors.append(f"{label}.{field} lacks terminal Rust connection evidence")
        records.append(dict(entry, local_addr=local, remote_addr=remote))
    return records


def _result(value: object, role: str, version: int, transport: str, errors: list[str]) -> dict | None:
    if not isinstance(value, dict) or value.get("implementation") not in ("forge", "go", "rust"):
        errors.append(f"{role} result is missing or has an unknown implementation")
        return None
    implementation = value["implementation"]
    if (
        value.get("role") != role or value.get("status") != "ok"
        or type(value.get("version")) is not int or value.get("version") != version
        or value.get("scenario") != f"autonat_v{version}"
        or value.get("protocol") != PROTOCOLS[version] or value.get("transport") != transport
    ):
        errors.append(f"{role} result has mismatched role/version/protocol/transport or failed status")
    for field in (
        "error", "cleanup_error", "handler_error", "handler_join_error", "trace_error",
        "trace_overflow_error", "connection_gater_trace_failure",
    ):
        if value.get(field) is not None:
            errors.append(f"{role} reports {field}")
    if value.get("cleanup_errors") not in (None, []):
        errors.append(f"{role} reports cleanup errors")
    if (
        (value.get("policy_denied") is not None and value["policy_denied"] is not False)
        or value.get("internet_egress") == "deny"
        or (transport == "tcp-pnet" and value.get("internet_egress") != "allow")
    ):
        errors.append(f"{role} policy denial is not reachability evidence")
    if implementation == "rust":
        required = ("trace_complete", "swarm_closed", "connection_drain", "joined_swarm_executor")
        nonce = "not_applicable_v1" if version == 1 else "not_exposed_by_donor_api"
        events = value.get("donor_events")
        if not isinstance(events, list) or len(events) > 64 or any(not _bounded_event(e) for e in events):
            errors.append(f"{role} Rust donor event trace is missing or unbounded")
    elif implementation == "forge":
        required = ("hosts_closed", "handlers_joined", "connection_gater_trace_complete")
        nonce = "not_exposed_by_public_API"
        after = _object(value.get("diagnostics_after_stop"))
        before = _object(value.get("diagnostics_before_stop"))
        if after is None or before is None:
            errors.append(f"{role} Forge diagnostics are missing or malformed")
        else:
            resources = after.get("resources")
            state = after.get("reachability")
            initial = before.get("reachability")
            if (
                after.get("lifecycle_phase") != "stopped"
                or not _integer(after.get("active_sessions"), 0, 0)
                or not isinstance(resources, dict)
                or any(not _integer(resources.get(key), 0, 0) for key in (
                    "system_memory", "inbound_connections", "outbound_connections",
                    "active_dials", "active_service_scopes",
                ))
                or not isinstance(state, dict)
                or any(not _integer(state.get(key), 0, 0)
                       for key in ("pending_probes", "active_handlers"))
            ):
                errors.append(f"{role} Forge diagnostics do not prove terminal resource cleanup")
            enabled = f"{'service' if role == 'listener' else 'client'}_v{version}_enabled"
            if (
                not isinstance(initial, dict) or initial.get("internet_egress_allowed") is not True
                or initial.get(enabled) is not True
            ):
                errors.append(f"{role} Forge diagnostics do not enable the selected AutoNAT role")
    else:
        required = ("hosts_closed", "handlers_joined", "trace_complete")
        nonce = "not_exposed_by_donor_api"
    if any(value.get(field) is not True for field in required):
        errors.append(f"{role} lacks complete trace or joined/closed owner evidence")
    if value.get("nonce_proof") != nonce:
        errors.append(f"{role} must retain its API's honest nonce visibility")
    if implementation != "forge" and value.get("donor_revision") != REVISIONS[implementation]:
        errors.append(f"{role} donor revision differs from the pinned fixture contract")
    return value


def validate_autonat_evidence(
    client: object,
    server: object,
    *,
    version: int,
    transport: str,
    expected_service_peer: str,
    expected_requested_address: str,
    client_terminal_status: object,
    server_terminal_status: object,
    expected_outcome: str = "reachable",
) -> list[str]:
    """Validate one independently terminated pair, not an aggregate PASS label.

    Rust outbound local endpoints/nonce bytes and exact v2 internal attempt counts
    are intentionally not claimed. Its authenticated peer + requested target +
    fresh inbound ConnectionId are paired with the other process instead.
    Negative v1 currently has an explicit Forge-client/Go-service fixture only.
    Positive Forge -> Go v1 also accepts FORGE_GO_V1_PROOF_BASIS without claiming
    a client inbound event. All other pairs, including v2, require both traces.
    """
    errors: list[str] = []
    if (
        type(version) is not int or version not in PROTOCOLS
        or transport not in ("tcp", "tcp-tls", "tcp-pnet", "quic")
        or expected_outcome not in ("reachable", "unreachable")
    ):
        return ["unsupported AutoNAT version, transport or expected outcome"]
    requested = _address(expected_requested_address, transport)
    if requested is None or not _string(expected_service_peer, 256):
        return ["runner must bind a public numeric requested address and exact service peer"]
    _terminal(client_terminal_status, "client", errors)
    _terminal(server_terminal_status, "server", errors)
    client = _result(client, "dialer", version, transport, errors)
    server = _result(server, "listener", version, transport, errors)
    if client is None or server is None:
        return errors
    ci, si = client["implementation"], server["implementation"]
    cp = client.get("peer_id" if ci == "rust" else "local_peer_id")
    sp = server.get("peer_id" if si == "rust" else "local_peer_id")
    if (
        not _string(cp, 256) or cp == sp or sp != expected_service_peer
        or server.get("service_enabled") is not True
    ):
        errors.append("pair requires independent client and exact enabled service identity")
    if _address(client.get("requested_addr"), transport, cp) != requested:
        errors.append("client requested address differs from the runner's exact target")
    if ci == "rust":
        if client.get("authenticated_peer") != sp or (version == 2 and client.get("candidate_reported") is not True):
            errors.append("Rust control lacks exact authenticated Identify/candidate evidence")
    elif (
        client.get("control_authenticated_peer") != sp or client.get("observer_peer_id") != sp
        or client.get("control_identify_completed") is not True
    ):
        errors.append("client control auth/Identify is not bound to the service")

    cc = _connections(client, "actual_connections", transport, "client", errors,
                      v1_authenticated_inbound=version == 1 and ci == "forge" and si == "rust")
    sc = _connections(server, "actual_connections", transport, "server", errors)
    controls = [c for c in cc if c["direction"] == "outbound" and c["authenticated_peer"] == sp]
    if ci != "forge":
        controls = [c for c in controls if c["connection_id"] == client.get("control_connection_id")]
    if len(controls) != 1:
        errors.append("client requires exactly one authenticated outbound control connection")
        return errors
    control = controls[0]
    control_local = control["local_addr"]
    if ci == "rust":
        control_local = _address(client.get("observer_reported_control_addr"), transport, cp)
        if control_local is None:
            errors.append("Rust Identify observed control endpoint is missing")
    if ci == "go" and (
        _address(client.get("control_local_addr"), transport, cp, outbound_quic_bind=True) != control_local
        or _address(client.get("control_remote_addr"), transport, sp) != control["remote_addr"]
    ):
        errors.append("Go control socket fields disagree with the actual connection")
    service_controls = [
        c for c in sc if c["direction"] == "inbound" and c["authenticated_peer"] == cp
        and c["local_addr"] == control["remote_addr"] and _local_matches(control_local, c["remote_addr"])
    ]
    if len(service_controls) != 1:
        errors.append("server does not independently corroborate the control connection")
    control_source = service_controls[0]["remote_addr"] if len(service_controls) == 1 else None

    if si == "go":
        probes = _connections(server, "probe_connections", transport, "server", errors)
        dialer_peer = server.get("probe_peer_id")
        if (
            not _string(dialer_peer, 256) or dialer_peer in (cp, sp)
            or not _integer(server.get("hosts_created"), 2, 2)
        ):
            errors.append("Go service must own an independent authenticated probe host")
        completed = server.get("service_completed_requests")
        if not isinstance(completed, list) or len(completed) > 64:
            errors.append("Go service completion trace is missing or unbounded")
            completed = []
        if version == 1 and completed:
            errors.append("Go v1 must not substitute v2 service completion events")
        if version == 2 and expected_outcome == "reachable" and not any(
            isinstance(e, dict) and e.get("response_status") == "OK" and e.get("dial_status") == "OK"
            and type(e.get("dial_data_required")) is bool and e.get("error") is None
            and (control_source is not None and (
                requested.split("/")[2] == control_source.split("/")[2]
                or e["dial_data_required"] is True
            ))
            and _address(e.get("dialed_addr"), transport, cp) == requested for e in completed
        ):
            errors.append("Go v2 service lacks the exact successful completed request")
    else:
        probes = sc
        dialer_peer = sp
    _corroborate_wildcards(cc, sc, cp, sp, errors)
    _corroborate_wildcards(sc, cc, sp, cp, errors)
    if si == "go":
        _corroborate_wildcards(probes, cc, dialer_peer, cp, errors)

    if ci in ("go", "rust"):
        inbound = _connections(client, "fresh_inbound_connections", transport, "client", errors)
        if (
            not _integer(client.get("fresh_inbound_count"))
            or client["fresh_inbound_count"] != len(inbound)
            or any(
                c not in cc or c["connection_id"] == control.get("connection_id")
                or c["direction"] != "inbound"
                or (ci == "rust" and c["fresh_requested_inbound"] is not True)
                for c in inbound
            )
        ):
            errors.append("fresh inbound evidence is not a distinct traced post-arm connection")
    else:
        inbound = [c for c in cc if c["direction"] == "inbound"]
    dialbacks = [c for c in inbound if c["authenticated_peer"] == dialer_peer and c["local_addr"] == requested]
    paired = [
        p for p in probes if p["direction"] == "outbound" and p["authenticated_peer"] == cp
        and p["remote_addr"] == requested and any(
            (_local_matches(p["local_addr"], c["remote_addr"]) if p["local_addr"] is not None else
             c["remote_addr"].split("/")[2] == control["remote_addr"].split("/")[2])
            and not (_local_matches(p["local_addr"], control["remote_addr"])
                     and _local_matches(control_local, requested))
            for c in dialbacks
        )
    ]
    if ci == "go" and client.get("authenticated_peer") != dialer_peer:
        errors.append("Go dialback identity disagrees with the paired probe host")
    # Independent Go probe-host ownership and authentication are checked above;
    # the Forge v1 public vote is checked below, not inferred from a PASS label.
    go_v1_probe = (
        version == 1 and ci == "forge" and si == "go" and not inbound and len(cc) == 1
        and any(p["direction"] == "outbound" and p["authenticated_peer"] == cp
                and not _wildcard(p["local_addr"]) and p["remote_addr"] == requested for p in probes)
    )

    negative = expected_outcome == "unreachable"
    if negative:
        if version != 1 or ci != "forge" or si != "go":
            errors.append("explicit negative evidence is supported only for Forge v1 against Go")
        if (
            client.get("expected_outcome") != "unreachable"
            or client.get("candidate_basis") != "explicit_unreachable_negative_control"
            or client.get("reached") is not False or client.get("probe_state") != "private"
            or client.get("v1_vote") != "private"
            or not _integer(client.get("probe_elapsed_ms"), 14000, 2**63 - 1)
            or not _integer(client.get("v2_fixture_candidate_count"), 0, 0)
        ):
            errors.append("negative v1 requires the typed private vote and real elapsed >= 14 seconds")
        if control_source is None or requested.split("/")[2] != control_source.split("/")[2]:
            errors.append("negative v1 candidate must use the actual control source IP")
        if (
            inbound or any(c["direction"] == "outbound" for c in probes)
            or client.get("response_addr") is not None or client.get("effective_state") == "public"
            or client.get("autonat_v1_state") == "public" or server.get("reached") is not False
            or server.get("response_addr") is not None
        ):
            errors.append("negative v1 contradicts authenticated dialback or positive evidence")
    else:
        if (
            client.get("expected_outcome") not in (None, "reachable")
            or client.get("candidate_basis") == "explicit_unreachable_negative_control"
            or client.get("reached") is not True or not (paired or go_v1_probe)
        ):
            errors.append("positive result lacks paired fresh authenticated dialback to the exact target")

    if ci == "forge":
        if (
            not _integer(client.get("autonat_probe_api_calls"), 1, 256)
            or client.get("response_addr") is not None
            or client.get("response_addr_proof") != "not_exposed_by_public_API"
        ):
            errors.append("Forge result must expose actual API calls without inventing a wire response")
        if version == 1:
            if (
                client.get("v1_vote") != ("private" if negative else "public")
                or client.get("probe_state") != client.get("v1_vote")
                or client.get("autonat_v2_address_state") is not None
                or client.get("current_v2_address_state") is not None
            ):
                errors.append("Forge v1 vote is missing or conflated with v2 state")
        else:
            address_state = _object(client.get("autonat_v2_address_state"))
            if (
                address_state is None or set(address_state) != {"address", "state"}
                or _address(address_state.get("address"), transport, cp) != requested
                or address_state.get("state") != "public"
                or address_state != _object(client.get("current_v2_address_state"))
                or client.get("v1_vote") is not None or client.get("probe_state") != "public"
                or client.get("autonat_v1_state") != "unknown"
            ):
                errors.append("Forge v2 result is not scoped to the exact verified address")
    else:
        basis = GO_API_BASIS if ci == "go" else RUST_API_BASIS
        if client.get("donor_api_basis") != basis[version] or _address(client.get("response_addr"), transport, cp) != requested:
            errors.append("donor result lacks the version-specific successful address binding")
        if ci == "go" and (
            not _integer(client.get("autonat_probe_api_calls"), 1, 256)
            or (version == 2 and not _integer(client.get("response_index"), 0, 0))
        ):
            errors.append("Go result lacks actual API count or selected v2 index")
        if ci == "rust":
            basis = "matched_response_event" if version == 1 else "tested_addr_from_verified_event_not_raw_wire_response"
            v1_count = 1 if version == 1 else 0
            if (
                client.get("response_addr_basis") != basis
                or client.get("autonat_probe_api_calls") is not None
                or not _integer(client.get("v1_request_events"), v1_count, v1_count)
                or not _integer(client.get("v2_result_events"),
                                1 if version == 2 else 0, 64 if version == 2 else 0)
            ):
                errors.append("Rust result must retain per-version events and honest hidden attempt count")
            if version == 2 and not _dial_data(client.get("dial_data_bytes")):
                errors.append("Rust v2 verified event lacks bounded dial-data evidence")
    for result in (client, server):
        if result["implementation"] == "rust":
            _rust_events(result, version, transport, requested, cp, sp, control_source, errors)
    return errors
