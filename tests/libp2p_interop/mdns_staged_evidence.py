"""Strict staged receipts; unavailable donor diagnostics remain unavailable."""

from mdns_evidence import SCHEMA, _integer, _ready, _terminal


def quiet_receipt(value, ready, implementation, role, service, terminal):
    peer = _ready(ready, implementation, role, service)
    _terminal(terminal)
    if (value.get("schema") != SCHEMA or value.get("implementation") != implementation
            or value.get("role") != role or value.get("local_peer_id") != peer
            or value.get("service_name") != service or value.get("outcome") != "quiet"
            or value.get("status") != "ok" or value.get("capture_phase") != "after_stop"
            or value.get("stop_reason") != "stop_file" or value.get("cleanup_complete") is not True
            or value.get("failure") is not None or value.get("error") is not None):
        raise ValueError("quiet receipt identity/outcome/cleanup mismatch")
    if ready.get("outcome") != "quiet":
        raise ValueError("quiet readiness required")
    start = _integer(value.get("ready_at_unix_ms"), "ready timestamp", 1)
    stop = _integer(value.get("stop_requested_at_unix_ms"), "stop timestamp", start)
    if start != ready.get("ready_at_unix_ms") or stop < start:
        raise ValueError("quiet timestamps changed")
    _integer(value.get("observed_milliseconds"), "quiet duration", 3000, 45000)
    counters = ["authenticated_connections", "echo_streams", "application_bytes_sent",
                "application_bytes_received"]
    if implementation == "forge":
        if (value.get("basis") != "runtime_metrics_and_gater_with_discovery_snapshot"
                or value.get("service_errors") != "unavailable_via_public_api"):
            raise ValueError("incorrect Forge quiet evidence basis")
        counters += ["mdns_observations_snapshot", "peer_dial_gate_calls", "inbound_gate_calls",
                     "direct_attempts", "handshakes_completed"]
    elif implementation == "go":
        if (value.get("basis") != "lifetime_notifier_and_network_notifications"
                or value.get("discovery_callback_scope") != "entered_callbacks_through_final_snapshot"
                or value.get("donor_callback_join") != "unavailable_native_detached_notifications"
                or value.get("cleanup_scope") != "mdns_service_host_and_admitted_echo_handlers"
                or value.get("discovery_overflow") is not False):
            raise ValueError("incorrect Go quiet evidence basis")
        counters += ["discovery_callbacks", "discovered_peers", "dial_attempts"]
    else:
        raise ValueError("unsupported quiet implementation")
    for name in counters:
        _integer(value.get(name), name, 0, 0)


def lifecycle_receipt(value, *, epoch, pid, peer):
    _integer(epoch, "requested epoch", 1, 2**64 - 1)
    _integer(pid, "owned PID", 1)
    if (value.get("schema") != "forge.mdns.lifecycle.v1"
            or type(value.get("epoch")) is not int or value["epoch"] != epoch
            or type(value.get("pid")) is not int or value["pid"] != pid
            or value.get("local_peer_id") != peer or value.get("request") != "snapshot"
            or value.get("capture_phase") != "after_echo_before_stop"
            or value.get("capture_error") != "" or value.get("failure") is not None
            or value.get("error") is not None
            or value.get("service_errors") != "unavailable_via_public_api"):
        raise ValueError("lifecycle receipt identity/epoch/capture mismatch")
    for name in ("mdns_observations", "active_operations", "memory", "file_descriptors",
                 "active_dials", "inbound_connections", "outbound_connections",
                 "inbound_streams", "outbound_streams", "sessions_opened", "sessions_closed"):
        _integer(value.get(name), name)
    _integer(value.get("failed_refreshes"), "failed refreshes", 0, 0)
    # Every sample is after the initial echo, including intermediate retries.
    _integer(value.get("sessions_opened"), "original authenticated session", 1, 1)
    _integer(value.get("sessions_closed"), "closed authenticated sessions", 0, 0)


def churn_sequence(samples, transitions, *, pid, peer):
    if len(samples) < 3 or len(samples) > 180 or len(transitions) != 2:
        raise ValueError("incomplete or oversized churn chronology")
    previous = 0
    last_time = -1
    selected = {}
    for sample in samples:
        epoch = sample["epoch"]
        if epoch != previous + 1 or sample["requested_at"] < last_time:
            raise ValueError("non-monotonic snapshot chronology")
        if sample["received_at"] < sample["requested_at"]:
            raise ValueError("receipt predates request")
        lifecycle_receipt(sample["receipt"], epoch=epoch, pid=pid, peer=peer)
        previous, last_time = epoch, sample["received_at"]
        if sample["receipt"]["mdns_observations"] == {"initial": 1, "down": 0, "up": 1}.get(sample["stage"]):
            selected[sample["stage"]] = sample
    if set(selected) != {"initial", "down", "up"}:
        raise ValueError("missing 1 -> 0 -> 1 transition")
    for before, after, transition, state in zip(("initial", "down"), ("down", "up"), transitions, (False, True)):
        if (transition.get("up") is not state or transition.get("checked") is not True
                or not selected[before]["received_at"] <= transition["began_at"]
                <= transition["finished_at"] <= selected[after]["requested_at"]):
            raise ValueError("interface transition outside epoch boundary")
