"""Fail-closed PR9 evidence checks. No registry promotion or fixture self-claims."""

import re

from autorelay_wire import validate_push_frame, validate_reservation_frame

HOP = "/libp2p/circuit/relay/0.2.0/hop"
IDENTIFY = "/ipfs/id/1.0.0"
ECHO = "/forge/interop/relay-echo/1"
PUSH = "/ipfs/id/push/1.0.0"
TRANSPORTS = ("quic", "tcp", "tcp-tls")


def _require(condition, message):
    if not condition:
        raise ValueError(message)


def _integer(value, low=0, high=2**63 - 1):
    return type(value) is int and low <= value <= high


def _peer(value):
    # Full cryptographic identity is established by the native transport/Identify;
    # the validator rejects ambiguous textual bindings, not just empty strings.
    return isinstance(value, str) and re.fullmatch(r"[a-zA-Z0-9]{20,128}", value) is not None


def circuit(value, relay, target, transport):
    _require(isinstance(value, str) and len(value) <= 512, "missing bounded circuit address")
    match = re.fullmatch(r"/ip4/127\.0\.0\.1/(tcp|udp)/([1-9][0-9]{0,4})(/quic-v1)?"
                         r"/p2p/([^/]+)/p2p-circuit/p2p/([^/]+)", value)
    _require(match is not None, "non-canonical circuit address")
    kind, port, quic, actual_relay, actual_target = match.groups()
    _require(int(port) <= 65535 and actual_relay == relay and actual_target == target
             and kind == ("udp" if transport == "quic" else "tcp")
             and bool(quic) == (transport == "quic"), "circuit identity/transport mismatch")
    return value


def _terminal(process):
    _require(isinstance(process, dict) and _integer(process.get("pid"), 1), "missing process ownership")
    terminal = process.get("terminal_status", {})
    _require(type(terminal.get("exit_code")) is int and terminal["exit_code"] == 0
             and terminal.get("termination") == "graceful", "process not gracefully joined")
    command = process.get("command")
    _require(isinstance(command, list) and len(command) >= 2
             and all(isinstance(x, str) and 0 < len(x) <= 4096 for x in command), "missing launch command")


def _options(process):
    args = process["command"][2:]
    _require(len(args) % 2 == 0 and len(set(args[::2])) == len(args[::2]), "ambiguous launch options")
    return dict(zip(args[::2], args[1::2]))


def _actor(actor, implementation, role, transport):
    _require(isinstance(actor, dict), f"missing {role} actor")
    _terminal(actor.get("process"))
    ready = actor.get("ready", {})
    _require(ready.get("implementation") == implementation and ready.get("status") == "ready"
             and _peer(ready.get("peer_id")), f"{role} readiness identity mismatch")
    result = actor.get("result")
    _require(isinstance(result, dict) and result.get("implementation") == implementation
             and result.get("role") == role and result.get("peer_id") == ready["peer_id"]
             and result.get("transport") == transport and result.get("scenario") == "autorelay"
             and result.get("complete") is True and result.get("error") is None
             and result.get("overflow", False) is False, f"{role} incomplete/mismatched result")
    options = _options(actor["process"])
    _require(options.get("--transport") == transport and options.get("--scenario") == "autorelay",
             "actor differs from actual launch configuration")
    lifecycle = result.get("fixture_task_lifecycle")
    if implementation == "rust":
        _require(isinstance(lifecycle, dict) and lifecycle.get("fixture_owned_tasks_joined") is True
                 and lifecycle.get("overflow") is False and lifecycle.get("errors") == [],
                 "Rust public-executor/handler tasks not joined")
    return ready["peer_id"], result


def _connection(row, peer, transport, *, relayed=False):
    _require(isinstance(row, dict) and row.get("peer_id") == peer and row.get("connection_id") not in (None, ""),
             "missing authenticated connection identity")
    remote = row.get("remote_addr")
    _require(isinstance(remote, str) and ("/p2p-circuit" in remote) == relayed,
             "connection was not on the required direct/circuit path")
    if relayed:
        return
    if row.get("negotiated_transport") == "/quic-v1":
        _require(transport == "quic" and "/quic-v1" in remote, "QUIC native receipt mismatch")
    elif "upgrade_observation" in row:
        observation = row["upgrade_observation"]
        _require(isinstance(observation, dict) and observation.get("overflow") is False,
                 "Rust negotiation trace overflow")
        matches = [c for c in observation.get("connections", [])
                   if c.get("authenticated_remote_peer_id") == peer
                   and c.get("security_complete") is True and c.get("muxer_complete") is True]
        _require(len(matches) == 1 and matches[0].get("selected_security") == ("/tls/1.0.0" if transport == "tcp-tls" else "/noise")
                 and matches[0].get("selected_muxer") == "/yamux/1.0.0" and "/tcp/" in remote,
                 "Rust native security/muxer receipt is absent or ambiguous")
    else:
        _require(row.get("negotiated_transport") == "tcp"
                 and row.get("negotiated_security") == ("/tls/1.0.0" if transport == "tcp-tls" else "/noise")
                 and row.get("negotiated_muxer") == "/yamux/1.0.0" and "/tcp/" in remote,
                 "Go native security/muxer receipt mismatch")


def _observations(result, target, transport):
    _require(result.get("reservations_basis") == "diagnostics.snapshot.relay_reservations",
             "reservations are not authoritative live node ownership")
    rows = result.get("observations")
    _require(isinstance(rows, list) and 4 <= len(rows) <= 602, "missing bounded Forge observations")
    previous = -1
    for row in rows:
        _require(isinstance(row, dict) and _integer(row.get("elapsed_ms"), max(0, previous), 65_000)
                 and _integer(row.get("unix_ms"), 1) and type(row.get("stopped")) is bool,
                 "non-monotonic or unbounded observation clock")
        previous = row["elapsed_ms"]
        _require(isinstance(row.get("addresses"), list) and len(row["addresses"]) <= 16
                 and isinstance(row.get("reservations"), list) and len(row["reservations"]) <= 4,
                 "missing bounded addresses/leases")
        manager = row.get("autorelay", {})
        _require(all(type(manager.get(k)) is bool for k in ("enabled", "running", "permitted", "stopping"))
                 and all(_integer(manager.get(k)) for k in (
                     "candidates", "pending_reservations", "reservations", "automatic_reservations", "waiting_refreshes",
                     "refreshes", "attempts", "successes", "failures", "renewals", "invalidated_completions"))
                 and manager.get("max_candidates") == 4 and manager.get("max_parallel_reservations") == 1
                 and manager.get("target_reservations") == 1 and manager["candidates"] <= 4
                 and manager["pending_reservations"] <= 1 and manager["waiting_refreshes"] <= 4,
                 "AutoRelay manager bounds/counters missing")
        active = row["reservations"]
        _require(len(active) <= 1 and manager["reservations"] == len(active)
                 and manager["automatic_reservations"] == len(active),
                 "live leases disagree with automatic node ownership/target")
        for lease in active:
            _require(_peer(lease.get("relay_peer_id")) and _integer(lease.get("id"), 1)
                     and _integer(lease.get("expires_unix_ms"), row["unix_ms"] + 1, row["unix_ms"] + 9000)
                     and _integer(lease.get("ttl_ms"), 1, 9000)
                     and all(_integer(lease.get(k), 1) for k in ("max_streams", "max_bytes", "max_queued_bytes"))
                     and type(lease.get("voucher_present")) is bool
                     and isinstance(lease.get("endpoints"), list) and 1 <= len(lease["endpoints"]) <= 16,
                     "live reservation is incomplete, expired or over native TTL")
            limit = lease.get("remote_limit")
            _require("remote_limit" in lease and (limit is None or isinstance(limit, dict)
                     and _integer(limit.get("duration_seconds")) and _integer(limit.get("data_bytes"))),
                     "live reservation omitted native remote limits")
            _require(manager["enabled"] is True and manager["running"] is True and manager["permitted"] is True,
                     "reservation is not owned by a running permitted AutoRelay manager")
        for address in row["addresses"]:
            if "/p2p-circuit" not in address:
                continue
            matches = [r for r in active if f"/p2p/{r.get('relay_peer_id')}/p2p-circuit" in address]
            _require(len(matches) == 1, "advertised circuit has no active reservation")
            lease = matches[0]
            circuit(address, lease["relay_peer_id"], target, transport)
            _require(address.split("/p2p-circuit")[0] in lease["endpoints"],
                     "advertised circuit is not a live lease endpoint")
            _require(any(s.get("peer_id") == lease["relay_peer_id"] and s.get("identified") is True
                         and s.get("direct") is True and s.get("closed") is False for s in row.get("sessions", [])),
                     "reservation candidate lacks authenticated direct Identify")
            _require(any(p.get("peer_id") == lease["relay_peer_id"] and HOP in p.get("protocols", [])
                         for p in row.get("peers", [])), "candidate did not Identify exact relay hop")
    _require(rows[0].get("phase") == "before_start" and not rows[0]["reservations"]
             and not any("/p2p-circuit" in a for a in rows[0]["addresses"]), "acquisition precondition contaminated")
    _require(rows[-2].get("phase") == "stopped" and rows[-1].get("phase") == "post_stop"
             and rows[-1]["elapsed_ms"] - rows[-2]["elapsed_ms"] >= 1000,
             "shutdown lacks measured delayed post-stop observation")
    for row in rows[-2:]:
        _require(row["stopped"] is True and not row["reservations"]
                 and not any("/p2p-circuit" in a for a in row["addresses"])
                 and not any(s.get("closed") is False for s in row.get("sessions", [])), "shutdown retained relay state")
        manager = row["autorelay"]
        _require(manager["running"] is False and manager["stopping"] is True and manager["permitted"] is False
                 and manager["pending_reservations"] == 0 and manager["waiting_refreshes"] == 0,
                 "AutoRelay work did not join after stop")
    _require(rows[-1].get("discovery_attempts") == rows[-2].get("discovery_attempts"), "manager worked after stop")
    _require(all(rows[-1]["autorelay"][k] == rows[-2]["autorelay"][k] for k in (
        "refreshes", "attempts", "successes", "failures", "renewals", "invalidated_completions")),
        "AutoRelay counters changed after joined stop")
    return rows


def _native_reservations(result, target, transport):
    rows = result.get("events")
    _require(isinstance(rows, list) and len(rows) <= 128, "missing native relay event trace")
    if result["implementation"] == "rust":
        controls = [r for r in rows if r.get("kind") == "connection" and r.get("peer_id") == target]
        _require(len(controls) == 1, "Rust relay lacks unique authenticated Forge control")
        _connection(controls[0], target, transport)
        accepted = [r for r in rows if r.get("kind") == "reservation_accepted" and r.get("peer_id") == target
                    and r.get("protocol") == HOP and r.get("basis") == "native_relay_ReservationReqAccepted"]
        _require(accepted and accepted[0].get("renewed") is False, "Rust native reservation not accepted")
    else:
        accepted = [r for r in rows if r.get("kind") == "reservation_response" and r.get("peer_id") == target]
        _require(accepted, "Go native reserve response missing")
        for row in accepted:
            _require(row.get("protocol") == HOP and row.get("capture_complete") is True
                     and type(row.get("status")) is int and row["status"] == 100
                     and _integer(row.get("expires_unix_ms"), row.get("unix_ms", 0) + 1),
                     "Go reserve lacks exact successful native wire response")
            _connection(row, target, transport)
    _require(all(_integer(r.get("unix_ms"), 1) for r in accepted), "native reservation time missing")
    return accepted


def _echo(echo, source, relay, target, transport, observed=None):
    _terminal(echo.get("process"))
    result = echo.get("result", {})
    _require(result.get("implementation") == source and result.get("scenario") == "autorelay"
             and result.get("status") == "ok" and result.get("relay_echo") is True
             and result.get("protocol") == ECHO and _integer(result.get("echo_bytes"), 1, 4096)
             and result.get("relay_peer") == relay and result.get("target_peer") == target
             and _peer(result.get("local_peer_id")) and result["local_peer_id"] not in (relay, target)
             and _integer(result.get("unix_ms"), 1), "independent relay echo identity/protocol/bytes missing")
    actual = circuit(result.get("relayed_addr"), relay, target, transport)
    if observed is not None:
        _require(actual in observed["addresses"], "echo circuit was not independently advertised")
    _connection(result.get("echo_connection"), target, transport, relayed=True)
    remote = result["echo_connection"]["remote_addr"]
    _require(remote.removesuffix("/p2p/" + target) == actual.removesuffix("/p2p/" + target),
             "actual circuit connection differs from advertised relay route")
    _connection(result.get("relay_connection"), relay, transport)
    options = _options(echo["process"])
    _require(options.get("--transport") == transport and options.get("--scenario") == "autorelay"
             and options.get("--peer-id") == target and options.get("--relay-peer-id") == relay
             and options.get("--relay-addr") == actual.split("/p2p-circuit")[0], "echo differs from launch/advertised circuit")
    return result


def _native_pushes(observation, target, transport):
    rows = observation.get("events")
    _require(isinstance(rows, list) and len(rows) <= 128, "missing bounded observer events")
    pushes, identities = [], set()
    for row in rows:
        if row.get("kind") != "identify_push":
            continue
        _require(row.get("protocol") == PUSH and _integer(row.get("unix_ms"), 1)
                 and isinstance(row.get("addresses"), list) and len(row["addresses"]) <= 16,
                 "native Push lacks protocol/time/address evidence")
        _connection(row, target, transport)
        if observation["implementation"] == "go":
            update, completed = row.get("native_update_sequence"), row.get("native_completed_sequence")
            _require(row.get("basis") == "native_EvtPeerProtocolsUpdated_then_EvtPeerIdentificationCompleted"
                     and _integer(update, 1) and _integer(completed, update + 1),
                     "Go Push is not causally paired native events")
            identity = (row["connection_id"], update, completed)
        else:
            receipts = row.get("wire_receipts")
            _require(row.get("basis") == "native_Identify_Received_with_unique_inbound_push_wire_receipt"
                     and isinstance(receipts, list) and len(receipts) == 1, "Rust Push wire receipt missing/ambiguous")
            wire = receipts[0]
            body = wire.get("read", {})
            _require(wire.get("protocol") == PUSH and wire.get("direction") == "inbound"
                     and _integer(wire.get("connection_trace_id"), 1) and _integer(wire.get("stream_trace_id"), 1)
                     and body.get("complete_frames") is True and body.get("invalid_or_over_limit") is False
                     and _integer(body.get("framed_bytes"), 1, 4106) and body.get("frames") == 1
                     and isinstance(body.get("framed_sha256"), str)
                     and re.fullmatch(r"[a-f0-9]{64}", body["framed_sha256"]), "Rust Push has incomplete negotiated wire frame")
            validate_push_frame(wire, target, row["addresses"])
            identity = (wire["connection_trace_id"], wire["stream_trace_id"])
        _require(identity not in identities, "native Push receipt reused")
        identities.add(identity)
        pushes.append(row)
    _require(len(pushes) >= 4, "automatic native Identify Push receipts missing")
    return pushes


def validate_case(artifact):
    try:
        _validate_case(artifact)
    except (ValueError, KeyError, TypeError, AttributeError, IndexError) as error:
        return [str(error)]
    return []


def _validate_case(artifact):
    _require(isinstance(artifact, dict) and artifact.get("scenario") == "autorelay"
             and artifact.get("cleanup_errors") == [], "wrong scenario or cleanup errors")
    spec, raw = artifact["case"], artifact["raw"]
    source, relay_impl, dest_impl, transport, kind = (spec[k] for k in ("source", "relay", "destination", "transport", "kind"))
    _require(transport in TRANSPORTS and (source, relay_impl, dest_impl, kind) in (
        ("go", "go", "forge", "lifecycle"), ("rust", "rust", "forge", "lifecycle"),
        ("go", "forge", "rust", "service"), ("rust", "forge", "go", "service")), "unsupported composition")
    _require(artifact.get("scenario_id") == f"autorelay-{kind}-{transport}-{source}-{relay_impl}-{dest_impl}", "case identity mismatch")
    relay, service = _actor(raw["relay"], relay_impl, "service", transport)
    target = raw["destination"]["ready"]["peer_id"]
    _require(_peer(target) and target != relay, "destination identity collision")
    echoes = {e["phase"]: e for e in artifact["echoes"]}
    _require(len(echoes) == len(artifact["echoes"]), "duplicate echo phase")
    if kind == "service":
        _require(set(echoes) == {"service_echo"}, "service echo missing")
        _terminal(raw["destination"].get("process"))
        ready = raw["destination"]["ready"]
        _require(ready.get("implementation") == dest_impl and ready.get("protocol") == HOP
                 and ready.get("relay_peer_id") == relay and ready.get("status") == "ready", "donor reservation is not bound to Forge service")
        if dest_impl == "go":
            _require(ready.get("reservation_basis") == "native_relayclient.Reserve"
                     and _integer(ready.get("expires_unix_ms"), 1), "Go client did not reserve natively")
            _connection(ready.get("relay_connection"), relay, transport)
            if ready.get("voucher") is True:
                _require(ready.get("voucher_validated") is True and ready.get("voucher_relay") == relay
                         and ready.get("voucher_peer") == target
                         and _integer(ready.get("voucher_expiration"), 1)
                         and ready["voucher_expiration"] * 1000 == ready["expires_unix_ms"]
                         and ready.get("voucher_validation_basis") == "native_Reserve_ConsumeEnvelope_signature_then_fixture_identity_expiry_checks",
                         "signed voucher not validated/bound")
        else:
            _require(ready.get("reservation_accepted") is True
                     and ready.get("reservation_basis") == "native_relay_client_ReservationReqAccepted_and_NewListenAddr",
                     "Rust client lacks native acceptance/circuit event")
            _connection(ready.get("relay_connection"), relay, transport)
            receipt = ready.get("native_reservation_receipt", {})
            _require(receipt.get("basis") == "passive_native_client_HOP_STATUS_response"
                     and receipt.get("protocol") == HOP and receipt.get("status") == 100
                     and _integer(receipt.get("connection_trace_id"), 1) and _integer(receipt.get("stream_trace_id"), 1)
                     and _integer(receipt.get("unix_ms"), 1)
                     and _integer(receipt.get("expires_unix_ms"), receipt["unix_ms"] + 1, receipt["unix_ms"] + 9000)
                     and isinstance(receipt.get("addresses"), list) and 1 <= len(receipt["addresses"]) <= 16,
                     "Rust native accepted reservation lacks bounded wire status/expiry/addresses")
            validate_reservation_frame(receipt, relay, target)
            _require(any(echoes["service_echo"]["result"]["relayed_addr"].split("/p2p-circuit")[0] == address
                         for address in receipt["addresses"]), "Rust client circuit differs from native reservation endpoint")
        _echo(echoes["service_echo"], source, relay, target, transport)
        observations = service.get("observations", [])
        _require(any(_integer(r.get("relay_bytes"), 1) and _integer(r.get("service_reservations"), 1)
                     for r in observations), "Forge service did not measure actual relayed bytes/reservation")
        _require(observations[-1].get("stopped") is True and observations[-1].get("service_reservations") == 0,
                 "Forge service resources retained after stop")
        return
    destination, result = _actor(raw["destination"], "forge", "destination", transport)
    _require(destination == target and result.get("operation_basis") == "async_start_authenticated_connect_only"
             and raw["destination"]["process"]["command"][1] == "autorelay-destination", "manual Forge reservation path")
    rows = _observations(result, target, transport)
    observer, observation = _actor(raw["observer"], source, "observer", transport)
    _require(observer not in (relay, target), "observer is not independent")
    observed = {}
    for row in observation.get("events", []):
        if row.get("kind") != "identify":
            continue
        _require(row.get("basis") == "independent_authenticated_identify_stream" and row.get("protocol") == IDENTIFY
                 and row.get("revision") not in observed and _integer(row.get("unix_ms"), 1), "ambiguous Identify evidence")
        _connection(row, target, transport)
        _require(isinstance(row.get("addresses"), list), "Identify addresses missing")
        observed[row["revision"]] = row
    _require(set(observed) == {"before_acquire", "acquired", "withdrawn", "replacement", "replacement_withdrawn"}, "lifecycle Identify phases missing")
    for phase in ("before_acquire", "withdrawn", "replacement_withdrawn"):
        _require(not any("/p2p-circuit" in a for a in observed[phase]["addresses"]), "withdrawal still advertises circuit")
    replacement_impl = "rust" if relay_impl == "go" else "go"
    replacement, replacement_service = _actor(raw["replacement"], replacement_impl, "service", transport)
    _require(replacement not in (relay, target, observer), "replacement identity collision")
    _require(set(echoes) == {"acquired_echo", "renewed_echo", "replacement_echo"}, "lifecycle echo phases missing")
    initial = _echo(echoes["acquired_echo"], source, relay, target, transport, observed["acquired"])
    renewed = _echo(echoes["renewed_echo"], source, relay, target, transport, observed["acquired"])
    _echo(echoes["replacement_echo"], source, replacement, target, transport, observed["replacement"])
    leases = [(row, r) for row in rows for r in row["reservations"] if r.get("relay_peer_id") == relay]
    _require(leases, "automatic acquisition missing")
    first_row, first = leases[0]
    _require(first_row["reachability"] in ("unknown", "private", "relay_only"), "unsupported reachability acquisition")
    _require(any(r["expires_unix_ms"] > first["expires_unix_ms"] and row["unix_ms"] < first["expires_unix_ms"]
                 for row, r in leases), "no autonomous renewal before original expiry")
    _require(any(row["autorelay"]["renewals"] >= 1 for row in rows), "manager did not measure autonomous renewal")
    _require(first_row["unix_ms"] <= initial["unix_ms"] < first["expires_unix_ms"] < renewed["unix_ms"], "echo does not span original lease expiry")
    accepted = _native_reservations(service, target, transport)
    _require(len(accepted) >= 2, "relay did not independently observe renewal")
    if relay_impl == "rust":
        _require(any(r.get("renewed") is True for r in accepted[1:]), "native Rust renewal event missing")
    else:
        _require(any(r["expires_unix_ms"] > accepted[0]["expires_unix_ms"]
                     and r["unix_ms"] < accepted[0]["expires_unix_ms"] for r in accepted[1:]), "native Go expiry did not advance before expiry")
    _native_reservations(replacement_service, target, transport)
    faults = artifact.get("faults", [])
    _require(len(faults) == 2 and [f.get("peer_id") for f in faults] == [relay, replacement]
             and all(f.get("kind") == "relay_stop" and _integer(f.get("unix_ms"), 1) for f in faults), "relay loss fault receipts missing")
    _require(observed["before_acquire"]["unix_ms"] < first_row["unix_ms"]
             and renewed["unix_ms"] <= faults[0]["unix_ms"] <= observed["withdrawn"]["unix_ms"]
             < observed["replacement"]["unix_ms"] <= faults[1]["unix_ms"] <= observed["replacement_withdrawn"]["unix_ms"],
             "acquisition/loss/replacement/withdrawal order is not measured")
    pushes = _native_pushes(observation, target, transport)
    for relay_peer, label in ((relay, "acquired"), (replacement, "replacement")):
        _require(any(any(f"/p2p/{relay_peer}/p2p-circuit" in a for a in push["addresses"])
                     and push["unix_ms"] <= observed[label]["unix_ms"]
                     for push in pushes), "native Push did not asynchronously advertise active relay")
    for fault, label in zip(faults, ("withdrawn", "replacement_withdrawn")):
        _require(any(not any("/p2p-circuit" in a for a in push["addresses"])
                     and fault["unix_ms"] <= push["unix_ms"] <= observed[label]["unix_ms"] for push in pushes),
                 "native Push did not asynchronously withdraw lost relay")
    for push in pushes:
        for address in push["addresses"]:
            if "/p2p-circuit" not in address:
                continue
            _require(any(address in row["addresses"] and row["unix_ms"] <= push["unix_ms"] + 500
                         and any(r["expires_unix_ms"] > push["unix_ms"] for r in row["reservations"])
                         for row in rows), "native Push address has no measured active lease")
