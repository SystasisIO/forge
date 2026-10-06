"""Structural unit doubles only. These artifacts are never live acceptance."""
import copy
import hashlib
import unittest
from unittest.mock import patch

from autorelay_wire import varint
from path_evidence import (RUST_SOURCE_WAVE_SCOPE, _echo, _handshakes, _nat, _session, challenge,
                           go_cancel_state, native_terminal, rust_source_wave_case, rust_source_wave_failure,
                           rust_source_wave_pending, validate_case, validate_frame)
from path_wire import address_bytes
from path_cases import case_specs
from path_network import PathNetwork
from dataclasses import asdict

TOKEN = "0123456789abcdef0123456789abcdef"


def frame(direction, kind, addresses, sequence=1):
    body = varint(8) + varint(kind)
    for address in addresses:
        encoded = address_bytes(address)
        body += varint(18) + varint(len(encoded)) + encoded
    wire = varint(len(body)) + body
    return {"kind": "dcutr_frame", "direction": direction, "message_type": kind, "addresses": addresses,
            "connection_id": "circuit", "remote_peer_id": "expected", "stream_id": "dcutr-1",
            "protocol": "/libp2p/dcutr", "sequence": sequence, "mono_ns": sequence * 100,
            "receipt": {"framed_hex": wire.hex(), direction: {"framed_bytes": len(wire),
                "framed_sha256": hashlib.sha256(wire).hexdigest(), "frames": 1,
                "complete_frames": True, "invalid_or_over_limit": False}}}


def session(path):
    return {"kind": "authenticated_connection", "remote_peer_id": "expected", "path": path,
            "sequence": 1, "connection_id": path, "authenticated": True,
            "local_address": "/ip4/10.1.0.2/udp/4001/quic-v1", "remote_address":
                "/ip4/11.0.0.3/udp/4001/quic-v1" + ("/p2p-circuit" if path == "relay" else ""),
            "transport": "circuit" if path == "relay" else "quic", "security": "/noise" if path == "relay" else "/tls/1.0.0",
            "muxer": "/yamux/1.0.0" if path == "relay" else None,
            "authentication_basis": "native_relay_inner_upgrade" if path == "relay" else "native_quic_authenticated_output"}


def unit_record():
    """A semantic double, explicitly rejected by the acceptance provenance gate."""
    spec = next(case_specs())
    peers = {"source": "forge_peer", "destination": "go_peer", "relay": "rust_peer"}
    namespace_token = "0" * 12
    addresses = {"relay": "11.0.0.1", "source_router": "11.0.0.2", "destination_router": "11.0.0.3",
                 "source": "10.1.0.2", "destination": "10.2.0.2"}
    namespaces = {role: f"path-{role}-{namespace_token}" for role in addresses}
    network = {"kind": "linux_dcutr_conntrack_nat", "state": "closed", "outer_namespace": "path-o-" + namespace_token,
               "outer_network": {"external_links": "absent"}, "participants": [{"role": role, "namespace": namespaces[role],
                   "addresses": [address]} for role, address in addresses.items()], "snapshots": [], "commands": []}
    for phase in ("before_connect", "barrier", "after_upgrade"):
        routers = {}
        for index, role in enumerate(("source_router", "destination_router"), 1):
            wan, other, lan = f"11.0.0.{index + 1}", f"11.0.0.{4 - index}", f"10.{index}.0.2"
            rules = f":FORWARD DROP\n-A INPUT -i eth0 -p udp -j DROP\n-A FORWARD -i eth0 -o lan0 -m conntrack --ctstate ESTABLISHED,RELATED -j ACCEPT\n-A POSTROUTING -j SNAT --to-source {wan}\n"
            flow = "" if phase == "before_connect" else f"ipv4 2 udp 17 29 src={lan} dst={other} sport=4001 dport=4001 src={other} dst={wan} sport=4001 dport=4001\n"
            routers[role] = {"rules": rules, "conntrack": flow}
            for tool, stdout in (("iptables-save", rules), ("conntrack", flow)):
                network["commands"].append({"command": ["/usr/sbin/ip", "netns", "exec", namespaces[role], "/usr/sbin/" + tool],
                                            "returncode": 0, "stdout": stdout, "test_double": True})
        network["snapshots"].append({"phase": phase, "routers": routers})
    record = {"schema_version": 1, "suite": "path", "scenario": "dcutr", "case": asdict(spec), "case_token": TOKEN,
              "raw": {}, "network": network, "errors": [], "cleanup_errors": [], "status": "passed",
              "application_actor": "destination", "test_double": True}
    for index, role in enumerate(("source", "destination", "relay"), 1):
        implementation = getattr(spec, role)
        events = []

        def add(kind, **fields):
            event = {"kind": kind, "source": implementation + ".unit_double", "sequence": len(events) + 1,
                     "mono_ns": (len(events) + 1) * 1_000_000, **fields}
            events.append(event)
            return event

        if role == "relay":
            add("native_relay_started")
        else:
            other = "destination" if role == "source" else "source"
            peer = peers[other]
            add("baseline", remote_peer_id=peer, direct_connection_ids=[])
            if implementation == "forge":
                add("native_snapshot", remote_peer_id=peer, hole_punch_successes=0)
            relay = session("relay")
            relay.pop("kind"); relay.pop("sequence")
            relay.update(remote_peer_id=peer, connection_id="circuit", relay_peer_id=peers["relay"],
                         direction="outbound" if role == "source" else "inbound")
            if implementation == "forge":
                relay.update(source="forge.authenticated_stream.diagnostics",
                    remote_address=f"/ip4/11.0.0.1/udp/4001/quic-v1/p2p/{peers['relay']}/p2p-circuit/p2p/{peer}",
                    endpoint_basis="logical_authenticated_circuit_route",
                    carrier_basis="forge.node.diagnostics.native_carrier", carrier_connection_id="carrier",
                    carrier_remote_peer_id=peers["relay"],
                    carrier_local_address=f"/ip4/{addresses[role]}/udp/4001/quic-v1",
                    carrier_remote_address="/ip4/11.0.0.1/udp/4001/quic-v1")
            add("authenticated_connection", **relay)

            def echo(phase, connection):
                payload = challenge(TOKEN, phase)
                if implementation != "forge" and phase != "relay_after":
                    add("application_open", stream_id="application-" + phase, connection_id=connection, connected_before=True,
                        opening_basis="native_existing_connection", phase=phase, dial_attempts_before=1, dial_attempts_after=1,
                        source="go.network.Conn.NewStream", dial_counter_basis="native_conn_new_stream_no_dial_api")
                return add("echo", phase=phase, connection_id=connection, remote_peer_id=peer,
                           stream_id="application-" + phase, protocol="/forge/interop/path-echo/1",
                           path="direct" if phase == "direct_after" else "relay", fresh_dial=False,
                           server=implementation == "forge", io_basis="retained_native_stream", read_bytes=len(payload), write_bytes=len(payload),
                           read_sha256=hashlib.sha256(payload).hexdigest(), write_sha256=hashlib.sha256(payload).hexdigest(),
                           dial_attempts_before=1, dial_attempts_after=1)

            echo("relay_before", "circuit")
            if implementation == "go":
                for direction, kind in (("write", 100), ("read", 100), ("write", 300)):
                    value = frame(direction, kind, ["/ip4/11.0.0.3/udp/4001/quic-v1"] if kind == 100 else [])
                    for key in ("kind", "sequence", "mono_ns"):
                        value.pop(key)
                    value["remote_peer_id"] = peer
                    add("dcutr_frame", **value)
                add("holepunch_trace", native_type="StartHolePunch", remote_peer_id=peer, rtt_ns=1_000_000)
                add("native_coordinated_authenticated", remote_peer_id=peer, connection_id="direct", authenticated=True,
                    security_role="server", security_role_basis="native_quic_secured_callback",
                    local_address=f"/ip4/{addresses[role]}/udp/4001/quic-v1",
                    remote_address=f"/ip4/{addresses[other + '_router']}/udp/4001/quic-v1",
                    secured_receipt={"source": "go.quic.transport.InterceptSecured",
                        "native_connection_basis": "network.Conn.As(**quic.Conn)", "native_connection_id": "unit-quic-owner",
                        "observed_mono_ns": (len(events) + 1) * 1_000_000, "local_peer_id": peers[role],
                        "remote_peer_id": peer, "direction": "inbound", "security_role": "server",
                        "local_address": f"/ip4/{addresses[role]}/udp/4001/quic-v1",
                        "remote_address": f"/ip4/{addresses[other + '_router']}/udp/4001/quic-v1"})
                add("holepunch_trace", native_type="EndHolePunch", remote_peer_id=peer, success=True)
            direct = session("direct")
            direct.pop("kind"); direct.pop("sequence"); direct["remote_peer_id"] = peer
            direct["local_address"] = f"/ip4/{addresses[role]}/udp/4001/quic-v1"
            direct["remote_address"] = f"/ip4/{addresses[other + '_router']}/udp/4001/quic-v1"
            if implementation == "forge":
                direct["source"] = "forge.authenticated_stream.diagnostics"
            add("authenticated_connection", **direct)
            if implementation == "forge":
                add("native_snapshot", remote_peer_id=peer, hole_punch_successes=1)
            echo("direct_after", "direct")
        ready = {"peer_id": peers[role], "implementation": implementation, "case_token": TOKEN,
                 "path_bindings": "native_public_diagnostics_v1" if implementation == "forge" else "actual_io_and_native_attempt_v1",
                 "listen_addrs": [f"/ip4/{addresses[role]}/udp/4001/quic-v1"]}
        result = {"schema_version": 1, "implementation": implementation, "case_token": TOKEN, "local_peer_id": peers[role],
                  "events": events, "overflow": False, "joined": True, "finalized": True, "error": None}
        record["raw"][role] = {"ready": ready, "result": result, "process": {"ready": ready, "pid": index,
            "terminal_status": {"exit_code": 0, "termination": "graceful"}, "outputs": [{"test_double": True}],
            "command": ["/usr/sbin/ip", "netns", "exec", namespaces[role], "/unit/not-a-live-binary", "path-live"]}}
    return record


def _unit_cancellation(spec):
    """Cancellation semantic doubles; never native cancellation/live evidence."""
    record = unit_record()
    donor = "destination" if spec.source == "forge" else "source"
    owner = donor if getattr(spec, donor) == "go" else ("source" if spec.source == "forge" else "destination")
    peers = {role: getattr(spec, role) + "_peer" for role in ("source", "destination", "relay")}
    record.update(case=asdict(spec), application_actor=donor)
    network = record["network"]
    network["faults"] = [{"kind": "peer_udp_drop", "before_circuit_connect": True}]
    fault = copy.deepcopy(network["snapshots"][0]); fault["phase"] = "after_fault"
    network["snapshots"].insert(1, fault)
    for role, other in (("source_router", "11.0.0.3"), ("destination_router", "11.0.0.2")):
        network["commands"].append({"command": ["/usr/sbin/ip", "netns", "exec", f"path-{role}-" + "0" * 12,
            "/usr/sbin/iptables", "-w", "2", "-I", "FORWARD", "1", "-i", "eth0", "-o", "lan0",
            "-p", "udp", "-s", other, "-j", "DROP"], "returncode": 0, "test_double": True})
    before, terminal = {}, {}
    for role in ("source", "destination", "relay"):
        actor = record["raw"][role]
        implementation = getattr(spec, role)
        actor["ready"].update(implementation=implementation, peer_id=peers[role],
            path_bindings="native_public_diagnostics_v1" if implementation == "forge" else "actual_io_and_native_attempt_v1")
        result = actor["result"]
        result.update(implementation=implementation, local_peer_id=peers[role], events=[])
        events = result["events"]

        def add(kind, **fields):
            events.append({"kind": kind, "source": implementation + ".unit_double", "sequence": len(events) + 1,
                           "mono_ns": (len(events) + 1) * 1_000_000, **fields})
            return events[-1]

        if role == "relay":
            add("native_relay_started")
            continue
        other = "destination" if role == "source" else "source"
        peer = peers[other]
        add("baseline", remote_peer_id=peer, direct_connection_ids=[])
        if implementation == "forge":
            add("native_snapshot", remote_peer_id=peer, hole_punch_successes=0, hole_punch_failures=0)
        relay = session("relay"); relay.pop("kind"); relay.pop("sequence")
        relay.update(remote_peer_id=peer, connection_id="circuit", relay_peer_id=peers["relay"],
                     direction="outbound" if role == "source" else "inbound")
        if implementation == "forge":
            relay.update(source="forge.authenticated_stream.diagnostics",
                remote_address=f"/ip4/11.0.0.1/udp/4001/quic-v1/p2p/{peers['relay']}/p2p-circuit/p2p/{peer}",
                endpoint_basis="logical_authenticated_circuit_route", carrier_basis="forge.node.diagnostics.native_carrier",
                carrier_connection_id="carrier", carrier_remote_peer_id=peers["relay"],
                carrier_local_address=f"/ip4/{'10.1.0.2' if role == 'source' else '10.2.0.2'}/udp/4001/quic-v1",
                carrier_remote_address="/ip4/11.0.0.1/udp/4001/quic-v1")
        if implementation == "rust":
            route = f"/ip4/11.0.0.1/udp/4001/quic-v1/p2p/{peers['relay']}/p2p-circuit"
            endpoint = ({"direction": "outbound", "upgrade_role": "outbound", "remote_address": route + "/p2p/" + peer}
                        if role == "source" else {"direction": "inbound", "upgrade_role": "inbound",
                                                  "local_address": route, "remote_address": "/p2p/" + peer})
            relay.update(source="rust.native_transport.authenticated_output", endpoint=endpoint,
                         remote_address=endpoint["remote_address"], local_address=endpoint.get("local_address"))
        add("authenticated_connection", **relay)
        if implementation == "rust":
            add("swarm_connection", source="rust.swarm.ConnectionEstablished", native_connection_id="2",
                remote_peer_id=peer, endpoint=endpoint)

        def echo(phase):
            if role == donor and phase == "relay_before":
                request_fields = {}
                if implementation == "rust":
                    requested = add("application_open_requested", source="rust.path_application.NotifyHandler.One.request", phase=phase,
                        remote_peer_id=peer, protocol="/forge/interop/path-echo/1", existing_connection_ids=["circuit"],
                        existing_direct_connection_ids=[], connected_before=True,
                        live_state_basis="native_authenticated_outputs_retained_until_ConnectionClosed",
                        native_connection_id="2", connection_id="circuit", path="relay", endpoint=endpoint,
                        target_binding_basis="exact_live_native_ConnectionId_authenticated_inner_relay",
                        open_api_basis="native_notify_handler_one_no_dial_path",
                        dial_attempts_before=0, dial_counter_basis="native_stream_behaviour_dial_requests")
                    request_fields = {"native_connection_id": "2", "remote_peer_id": peer,
                                      "requested_sequence": requested["sequence"], "requested_mono_ns": requested["mono_ns"],
                                      "open_api_basis": "native_notify_handler_one_no_dial_path"}
                    payload = challenge(TOKEN, phase)
                    for direction in ("write", "read"):
                        add("application_frame", source="rust.native_muxer.application.io", phase=phase,
                            connection_id="circuit", stream_id="application-relay", remote_peer_id=peer, direction=direction,
                            path="relay", payload_bytes=len(payload), payload_sha256=hashlib.sha256(payload).hexdigest())
                add("application_open", source="go.network.Conn.NewStream" if implementation == "go"
                    else "rust.path_application.NotifyHandler.One.actual_owner", connection_id="circuit", stream_id="application-relay",
                    phase=phase, opening_basis="native_existing_connection", connected_before=True,
                    dial_counter_basis="native_conn_new_stream_no_dial_api" if implementation == "go"
                    else "native_stream_behaviour_dial_requests", dial_attempts_before=0, dial_attempts_after=0, **request_fields)
            payload = challenge(TOKEN, phase)
            add("echo", source="rust.path_echo.io" if implementation == "rust" else implementation + ".unit_double",
                phase=phase, protocol="/forge/interop/path-echo/1", connection_id="circuit", stream_id="application-relay",
                remote_peer_id=peer, path="relay", fresh_dial=False, server=role != donor, io_basis="retained_native_stream",
                read_bytes=len(payload), write_bytes=len(payload), read_sha256=hashlib.sha256(payload).hexdigest(),
                write_sha256=hashlib.sha256(payload).hexdigest(), dial_attempts_before=1, dial_attempts_after=3)

        echo("relay_before")
        if role == donor:
            pattern = (("read", 100), ("write", 100), ("read", 300)) if role == "source" else (
                ("write", 100), ("read", 100), ("write", 300))
            for direction, kind in pattern:
                fields = frame(direction, kind, ["/ip4/11.0.0.2/udp/4001/quic-v1"] if kind == 100 else [])
                for key in ("kind", "sequence", "mono_ns"):
                    fields.pop(key)
                fields["remote_peer_id"] = peer
                if implementation == "go":
                    fields["source"] = "go.native_dcutr.io"
                add("dcutr_frame", **fields)
            if implementation == "go":
                add("holepunch_trace", source="go.holepunch.tracer", native_type="StartHolePunch",
                    local_peer_id=peers[role], remote_peer_id=peer, native_unix_ns=1791272877400000000,
                    addresses=["/ip4/11.0.0.2/udp/4001/quic-v1"], rtt_ns=1_000_000)
        before[role] = {**copy.deepcopy(result), "finalized": False, "joined": False}
        if role == owner:
            source = "go.holepunch.Service.Close.request" if implementation == "go" else "forge.node.async_cancel_hole_punch.request"
            add("cancellation_requested", source=source, remote_peer_id=peer)
            if implementation == "go":
                add("holepunch_trace", source="go.holepunch.tracer", native_type="EndHolePunch", remote_peer_id=peer,
                    local_peer_id=peers[role], native_unix_ns=1791272877410000000, success=False)
                add("native_handlers_drained", source="go.native_dcutr.handler_return", remote_peer_id=peer,
                    entered=1, completed=1, active=0)
                add("native_service_joined", source="go.holepunch.Service.Close.and_handler_drain", remote_peer_id=peer, cancelled=True)
            else:
                add("native_cancel_completed", source="forge.node.async_cancel_hole_punch.return", remote_peer_id=peer, accepted=True)
                add("native_service_joined", source="forge.node.async_cancel_hole_punch.join", remote_peer_id=peer, cancelled=True, accepted=True)
        if implementation == "rust":
            add("native_dcutr_event", remote_peer_id=peer, result={"native_success": False, "error": "unit natural terminal"})
            terminal[role] = copy.deepcopy(result)
        echo("relay_after")
    record["cancellation"] = {"observed_active_phase": True, "actor": owner, "before_request": before[owner],
                              "wire_actor": donor, "wire_before_request": before[donor]}
    if getattr(spec, donor) == "rust":
        record["cancellation"]["natural_terminal"] = terminal[donor]
    return record


def unit_cancellation(spec):
    return unit_source_wave(spec) if spec.source == "rust" else _unit_cancellation(spec)


def refresh_go_cancel_prefix(record):
    """Rebase structural doubles, never alter or repair captured live receipts."""
    role = record["cancellation"]["actor"]
    result = record["raw"][role]["result"]
    events = result["events"]
    for index, event in enumerate(events, 1):
        event.update(sequence=index, mono_ns=index * 1_000_000)
    request = next(e for e in events if e["kind"] == "cancellation_requested")
    prefix = {**copy.deepcopy(result), "finalized": False, "joined": False,
              "events": copy.deepcopy(events[:request["sequence"] - 1])}
    record["cancellation"].update(before_request=copy.deepcopy(prefix), wire_before_request=prefix)


def unit_go_retry_cancellation(spec):
    record = _unit_cancellation(spec)
    role = record["cancellation"]["actor"]
    events = record["raw"][role]["result"]["events"]
    start = next(e for e in events if e.get("native_type") == "StartHolePunch")
    end = copy.deepcopy(next(e for e in events if e.get("native_type") == "EndHolePunch"))
    frames = copy.deepcopy([e for e in events if e["kind"] == "dcutr_frame"])
    for event in frames:
        event["stream_id"] = "dcutr-2"
    events[events.index(start) + 1:events.index(start) + 1] = [end, *frames, copy.deepcopy(start)]
    refresh_go_cancel_prefix(record)
    return record


def captured_023_go_connect_before_auth_prefix():
    """Exact first nine root023 forge_to_go.cancelled events; not a PASS receipt."""
    peer = "12D3KooWHGHd5ACPwu5HRYA4rhGQbEziR3n7cnAGDUMngkA44fhi"
    local = "12D3KooWCfSPXJ9cmJ8CDdr9RqcxzA3MGXw4wPKxvG8J2nJAJT9L"
    relay = "12D3KooWNntxC1tmzUsLuCbNBMV7erMPnNo8nK6GzSkwBBDtmf4u"
    return {"schema_version": 1, "implementation": "go", "case_token": "25d8563d6f0c52e2938834bbbf6ab2af",
            "local_peer_id": local, "error": None, "overflow": False, "finalized": False, "joined": False,
            "events": [
                {"kind": "identify_observed_address", "source": "go.event.EvtPeerIdentificationCompleted",
                 "sequence": 1, "mono_ns": 6952834, "connection_id": "12D3KooWNn-1", "observer_peer_id": relay,
                 "connection_local_address": "/ip4/10.2.0.2/udp/55701/quic-v1",
                 "listener_address": "/ip4/10.2.0.2/udp/55701/quic-v1", "observed_address": "/ip4/11.0.0.3/udp/55701/quic-v1"},
                {"kind": "native_dcutr_protocol_ready", "source": "go.host.Mux.Protocols", "sequence": 2,
                 "mono_ns": 257286001, "protocol": "/libp2p/dcutr", "registered": True,
                 "protocols": ["/ipfs/ping/1.0.0", "/libp2p/circuit/relay/0.2.0/stop", "/ipfs/id/1.0.0",
                               "/ipfs/id/push/1.0.0", "/forge/interop/path-echo/1", "/libp2p/dcutr"]},
                {"kind": "baseline", "source": "go.network.connections", "sequence": 3, "mono_ns": 364488793,
                 "remote_peer_id": peer, "direct_connection_ids": []},
                {"kind": "control_completed", "source": "go.path_control.native_call", "sequence": 4,
                 "mono_ns": 364495668, "action": "bind", "control_sequence": 1},
                {"kind": "dcutr_frame", "source": "go.native_dcutr.io", "sequence": 5, "mono_ns": 432164585,
                 "connection_id": "12D3KooWHG-2", "stream_id": "12D3KooWHG-2-10", "remote_peer_id": peer,
                 "direction": "write", "message_type": 100, "protocol": "/libp2p/dcutr",
                 "addresses": ["/ip4/11.0.0.3/udp/55701/quic-v1"], "receipt": {
                     "framed_hex": "0f0864120b040b0000039102d995cd03", "write": {"framed_bytes": 16,
                         "framed_sha256": "a3d470ddee80c34bd39865df5b316bf08f777f5b73d6f6fdb0dc5a6c0a2cea70",
                         "frames": 1, "complete_frames": True, "invalid_or_over_limit": False}}},
                {"kind": "authenticated_connection", "source": "go.network.Conn.authenticated_output",
                 "sequence": 6, "mono_ns": 435028876, "connection_id": "12D3KooWHG-2", "remote_peer_id": peer,
                 "authenticated": True, "authentication_basis": "native_relay_inner_upgrade", "direction": "inbound",
                 "path": "relay", "transport": "circuit", "security": "/noise", "muxer": "/yamux/1.0.0",
                 "relay_peer_id": relay, "local_address": "/ip4/10.2.0.2/udp/55701/quic-v1",
                 "remote_address": "/ip4/11.0.0.1/udp/42644/quic-v1/p2p/" + relay + "/p2p-circuit"},
                {"kind": "dcutr_frame", "source": "go.native_dcutr.io", "sequence": 7, "mono_ns": 441052626,
                 "connection_id": "12D3KooWHG-2", "stream_id": "12D3KooWHG-2-10", "remote_peer_id": peer,
                 "direction": "read", "message_type": 100, "protocol": "/libp2p/dcutr",
                 "addresses": ["/ip4/11.0.0.2/udp/42060/quic-v1/p2p/" + peer], "receipt": {
                     "framed_hex": "3808641234040b0000029102a44ccd03a503260024080112206ea5c4ee8f7f11f558f7f1403088c3af1312f869ba53e87721a246ecba018c19",
                     "read": {"framed_bytes": 57,
                         "framed_sha256": "21ced4f2120f9fab85be8aa1df6512b02c298c0d18b87a4ea5b0872e204407b4",
                         "frames": 1, "complete_frames": True, "invalid_or_over_limit": False}}},
                {"kind": "dcutr_frame", "source": "go.native_dcutr.io", "sequence": 8, "mono_ns": 441080335,
                 "connection_id": "12D3KooWHG-2", "stream_id": "12D3KooWHG-2-10", "remote_peer_id": peer,
                 "direction": "write", "message_type": 300, "protocol": "/libp2p/dcutr", "addresses": [], "receipt": {
                     "framed_hex": "0308ac02", "write": {"framed_bytes": 4,
                         "framed_sha256": "c1f6fd90cbcd42f1a9e09047328104f92bfbae6df4f736ff13be26f775c02e05",
                         "frames": 1, "complete_frames": True, "invalid_or_over_limit": False}}},
                {"kind": "holepunch_trace", "source": "go.holepunch.tracer", "sequence": 9, "mono_ns": 446126293,
                 "local_peer_id": local, "remote_peer_id": peer, "native_type": "StartHolePunch",
                 "native_unix_ns": 1791239978340351760,
                 "addresses": ["/ip4/11.0.0.2/udp/42060/quic-v1/p2p/" + peer], "rtt_ns": 8955292}]}


def captured_079_go_protocol_error_prefix():
    """Exact first eight raw events from root079 forge_to_go.cancelled, not a PASS receipt."""
    peer = "12D3KooWALFQS1Vc8xcndcvmtLMzeDfH3U6u1ZrzNArfYRYfcdhr"
    local = "12D3KooWE237i59NtztFjjh5gJZEHbEb25PjGPvuzE2UTh7hwU9a"
    relay = "12D3KooWNU1eAam4tLVgz9L2MkCo2ooAMt38geB1ocjzStMBqS59"
    return {"schema_version": 1, "implementation": "go", "case_token": "24057c50c5b95ee8cfd48a87f4f7cdf8",
            "local_peer_id": local, "error": None, "overflow": False, "finalized": False, "joined": False,
            "events": [
                {"kind": "identify_observed_address", "source": "go.event.EvtPeerIdentificationCompleted",
                 "sequence": 1, "mono_ns": 14128960, "connection_id": "12D3KooWNU-1", "observer_peer_id": relay,
                 "connection_local_address": "/ip4/10.2.0.2/udp/36560/quic-v1",
                 "listener_address": "/ip4/10.2.0.2/udp/36560/quic-v1", "observed_address": "/ip4/11.0.0.3/udp/36560/quic-v1"},
                {"kind": "native_dcutr_protocol_ready", "source": "go.host.Mux.Protocols", "sequence": 2,
                 "mono_ns": 264054127, "protocol": "/libp2p/dcutr", "registered": True,
                 "protocols": ["/ipfs/ping/1.0.0", "/libp2p/circuit/relay/0.2.0/stop", "/ipfs/id/1.0.0",
                               "/ipfs/id/push/1.0.0", "/forge/interop/path-echo/1", "/libp2p/dcutr"]},
                {"kind": "baseline", "source": "go.network.connections", "sequence": 3, "mono_ns": 492261252,
                 "remote_peer_id": peer, "direct_connection_ids": []},
                {"kind": "control_completed", "source": "go.path_control.native_call", "sequence": 4,
                 "mono_ns": 492280002, "action": "bind", "control_sequence": 1},
                {"kind": "authenticated_connection", "source": "go.network.Conn.authenticated_output",
                 "sequence": 5, "mono_ns": 541546710, "connection_id": "12D3KooWAL-2", "remote_peer_id": peer,
                 "authenticated": True, "authentication_basis": "native_relay_inner_upgrade", "direction": "inbound",
                 "path": "relay", "transport": "circuit", "security": "/noise", "muxer": "/yamux/1.0.0",
                 "relay_peer_id": relay, "local_address": "/ip4/10.2.0.2/udp/36560/quic-v1",
                 "remote_address": "/ip4/11.0.0.1/udp/56480/quic-v1/p2p/" + relay + "/p2p-circuit"},
                {"kind": "dcutr_frame", "source": "go.native_dcutr.io", "sequence": 6, "mono_ns": 551414502,
                 "connection_id": "12D3KooWAL-2", "stream_id": "12D3KooWAL-2-10", "remote_peer_id": peer,
                 "direction": "write", "message_type": 100, "protocol": "/libp2p/dcutr",
                 "addresses": ["/ip4/11.0.0.3/udp/36560/quic-v1"], "receipt": {
                     "framed_hex": "0f0864120b040b00000391028ed0cd03", "write": {"framed_bytes": 16,
                         "framed_sha256": "6bc507876388714193eb8522e8135a0379049f03016883267a4001512803aa70",
                         "frames": 1, "complete_frames": True, "invalid_or_over_limit": False}}},
                {"kind": "dcutr_stream_terminal", "source": "go.native_dcutr.io", "sequence": 7, "mono_ns": 558921127,
                 "connection_id": "12D3KooWAL-2", "stream_id": "12D3KooWAL-2-10", "remote_peer_id": peer,
                 "direction": "read", "protocol": "/libp2p/dcutr", "error_kind": "reset", "error": "stream reset: stream reset",
                 "completed_frame_count": 0, "io_bytes": 0, "pending_frame_bytes": 0, "invalid_or_over_limit": False},
                {"kind": "holepunch_trace", "source": "go.holepunch.tracer", "sequence": 8, "mono_ns": 558962752,
                 "local_peer_id": local, "remote_peer_id": peer, "native_type": "ProtocolError",
                 "native_unix_ns": 1791272877446472126,
                 "error": "failed to initiateHolePunch: failed to read CONNECT message from remote peer: stream reset: stream reset"}]}


def unit_source_wave(spec):
    """Typed native SOURCE-wave semantic double; never a native aggregate/join."""
    cancelled = next(s for s in case_specs() if s.source == "rust" and s.outcome == "cancelled")
    value = _unit_cancellation(cancelled)
    value["case"] = asdict(spec)
    value["terminal_scope"] = RUST_SOURCE_WAVE_SCOPE
    result = value["raw"]["source"]["result"]
    events = result["events"]
    peer = value["raw"]["destination"]["ready"]["peer_id"]
    relay = next(e for e in events if e["kind"] == "authenticated_connection")
    events[:] = [e for e in events if e["kind"] != "native_dcutr_event"]
    frames = [e for e in events if e["kind"] == "dcutr_frame"]
    for frame_event in frames:
        frame_event["source"] = "rust.native_muxer.dcutr.io"
    common = {"relay_native_connection_id": "2", "relay_connection_id": "circuit", "stream_id": frames[0]["stream_id"],
        "remote_peer_id": peer, "advertised_addresses": frames[0]["addresses"],
        "binding_basis": "native_dcutr_poll_id_pending_callback_and_original_authenticated_relay_wire",
        "proof_scope": "native_wave_only", "aggregate_dcutr_completed": False, "rust_behaviour_joined": False,
        "native_connection_id": "3"}
    request = {"kind": "native_dcutr_dial_requested", "source": "rust.dcutr.Behaviour.poll.ToSwarm.Dial", **common,
        "source_wave_bound": True, "native_origin": "dcutr::Behaviour::poll::ToSwarm::Dial", "candidate_addresses": [],
        "pending_sequence": None, "pending_mono_ns": None}
    options = {"kind": "native_dcutr_dial_options", "source": "rust.dcutr.Behaviour.handle_pending_outbound_connection", **common,
        "candidate_addresses": frames[0]["addresses"], "requested_role": "dialer",
        "candidate_match_basis": "exact_numeric_socket_optional_same_peer_suffix", "native_callback_accepted": True,
        "native_returned_addresses": []}
    error = {"kind": "native_dial_error", "source": "rust.swarm.OutgoingConnectionError", "native_connection_id": "3",
             "remote_peer_id": peer, "error": "native QUIC handshake timed out"}
    failure = {"kind": "native_dcutr_wave_failed", "source": "rust.swarm.OutgoingConnectionError.native_dcutr_wave", **common,
        "native_origin": request["native_origin"], "candidate_addresses": options["candidate_addresses"],
        "dial_error_variant": "Transport", "original_relay_live": True,
        "transport_errors": [{"address": address, "transport_error_variant": "Other", "quic_error_type": "libp2p_quic::Error",
            "quic_error_variant": "HandshakeTimedOut", "classification_basis": "typed_downcast_and_public_native_enum",
            "wrapper_types": ["std::io::Error"], "error": "QUIC handshake timed out"} for address in options["candidate_addresses"]]}
    offset = events.index(frames[-1]) + 1
    events[offset:offset] = [request, options, error, failure]
    for index, event in enumerate(events, 1):
        event.update(sequence=index, mono_ns=index * 1_000_000)
    for event in (request, options, failure):
        for prefix, frame_event in zip(("connect_read", "connect_write", "sync_read"), frames):
            for key in ("sequence", "mono_ns"):
                event[prefix + "_" + key] = frame_event[key]
        for key in ("sequence", "mono_ns"):
            event["requested_" + key] = request[key]
            if event is not request:
                event["pending_" + key] = options[key]
            if event is failure:
                event["raw_error_" + key] = error[key]
    value["native_wave_failure"] = {**result, "finalized": False, "joined": False,
                                    "events": copy.deepcopy(events[:events.index(failure) + 1])}
    if spec.outcome == "cancelled":
        value["cancellation"].pop("natural_terminal")
        value["cancellation"]["wire_before_request"] = {**result, "finalized": False, "joined": False,
            "events": copy.deepcopy(events[:events.index(options) + 1])}
    else:
        value.pop("cancellation")
        forge = value["raw"]["destination"]["result"]["events"]
        forge[:] = [e for e in forge if e["kind"] not in {"cancellation_requested", "native_cancel_completed", "native_service_joined"}]
        forge.insert(len(forge) - 1, {"kind": "native_snapshot", "source": "forge.unit_double", "remote_peer_id": result["local_peer_id"],
                                    "hole_punch_successes": 0, "hole_punch_failures": 1})
        for index, event in enumerate(forge, 1):
            event.update(sequence=index, mono_ns=index * 1_000_000)
    return value


def refresh_source_wave(value, *, restamp=False):
    """Keep structural wave doubles coherent, without repairing mutated facts."""
    result = value["raw"]["source"]["result"]
    events = result["events"]
    if restamp:
        for index, event in enumerate(events, 1):
            event.update(sequence=index, mono_ns=index * 1_000_000)
        frames = [e for e in events if e["kind"] == "dcutr_frame"]
        request = next(e for e in events if e["kind"] == "native_dcutr_dial_requested")
        options = next(e for e in events if e["kind"] == "native_dcutr_dial_options")
        error = next(e for e in events if e["kind"] == "native_dial_error")
        for event in (request, options, next(e for e in events if e["kind"] == "native_dcutr_wave_failed")):
            for prefix, frame_event in zip(("connect_read", "connect_write", "sync_read"), frames):
                for key in ("sequence", "mono_ns"):
                    event[prefix + "_" + key] = frame_event[key]
            for key in ("sequence", "mono_ns"):
                event["requested_" + key] = request[key]
                if event is not request:
                    event["pending_" + key] = options[key]
                if event["kind"] == "native_dcutr_wave_failed":
                    event["raw_error_" + key] = error[key]
    end = next(i for i, e in enumerate(events) if e["kind"] == "echo" and e["phase"] == "relay_after")
    value["native_wave_failure"] = {**result, "finalized": False, "joined": False,
                                    "events": copy.deepcopy(events[:end])}
    return events


def unit_rust_success(spec):
    """Actual-schema retirement semantic double, never a live native receipt."""
    record = unit_record()
    forge_events = copy.deepcopy(record["raw"]["source"]["result"]["events"])
    donor = "destination" if spec.source == "forge" else "source"
    peers = {"source": spec.source + "_peer", "destination": spec.destination + "_peer", "relay": "go_relay"}
    record.update(case=asdict(spec), application_actor=donor)
    for role in ("source", "destination", "relay"):
        actor = record["raw"][role]
        implementation = getattr(spec, role)
        lan = "10.1.0.2" if role == "source" else "10.2.0.2" if role == "destination" else "11.0.0.1"
        actor["ready"].update(implementation=implementation, peer_id=peers[role],
            path_bindings="native_public_diagnostics_v1" if implementation == "forge" else "actual_io_and_native_attempt_v1",
            listen_addrs=[f"/ip4/{lan}/udp/4001/quic-v1"])
        result = actor["result"]
        result.update(implementation=implementation, local_peer_id=peers[role])
        if role == "relay":
            result["events"] = [{"kind": "native_relay_started", "source": "go.unit_double", "sequence": 1, "mono_ns": 1}]
            continue
        peer = peers["destination" if role == "source" else "source"]
        remote = "/ip4/11.0.0." + ("3" if role == "source" else "2") + "/udp/4001/quic-v1"
        route = f"/ip4/11.0.0.1/udp/4001/quic-v1/p2p/{peers['relay']}/p2p-circuit"
        if implementation == "forge":
            result["events"] = copy.deepcopy(forge_events)
            for event in result["events"]:
                if "remote_peer_id" in event:
                    event["remote_peer_id"] = peer
                if event["kind"] == "authenticated_connection":
                    event["local_address"] = f"/ip4/{lan}/udp/4001/quic-v1"
                    if event["path"] == "direct":
                        event["remote_address"] = remote
                    else:
                        event.update(remote_address=route + "/p2p/" + peer, relay_peer_id=peers["relay"],
                            carrier_remote_peer_id=peers["relay"], carrier_local_address=event["local_address"],
                            direction="outbound" if role == "source" else "inbound")
            continue
        events = result["events"] = []

        def add(kind, source, **fields):
            event = {"kind": kind, "source": source, "sequence": len(events) + 1,
                     "mono_ns": (len(events) + 1) * 1_000_000, **fields}
            events.append(event)
            return event

        def owner(path, connection, native_id, remote_peer, endpoint):
            fields = session(path)
            for key in ("kind", "sequence"):
                fields.pop(key)
            fields.update(connection_id=connection, remote_peer_id=remote_peer, endpoint=endpoint,
                direction=endpoint["direction"], local_address=endpoint.get("local_address"),
                remote_address=endpoint["remote_address"], relay_peer_id=peers["relay"] if path == "relay" else None)
            add("authenticated_connection", "rust.native_transport.authenticated_output", **fields)
            return add("swarm_connection", "rust.swarm.ConnectionEstablished", native_connection_id=native_id,
                       remote_peer_id=remote_peer, endpoint=endpoint)

        def echo(phase, connection):
            payload = challenge(TOKEN, phase)
            stream = "application-" + phase
            exact = {}
            if phase == "relay_before":
                requested = add("application_open_requested", "rust.path_application.NotifyHandler.One.request", phase=phase,
                    remote_peer_id=peer, protocol="/forge/interop/path-echo/1", existing_connection_ids=[connection],
                    existing_direct_connection_ids=[], connected_before=True,
                    live_state_basis="native_authenticated_outputs_retained_until_ConnectionClosed",
                    native_connection_id="2", connection_id=connection, path="relay", endpoint=inner["endpoint"],
                    target_binding_basis="exact_live_native_ConnectionId_authenticated_inner_relay",
                    open_api_basis="native_notify_handler_one_no_dial_path",
                    dial_attempts_before=0, dial_counter_basis="native_stream_behaviour_dial_requests")
                exact = {"native_connection_id": "2", "remote_peer_id": peer,
                         "requested_sequence": requested["sequence"], "requested_mono_ns": requested["mono_ns"],
                         "open_api_basis": "native_notify_handler_one_no_dial_path"}
            for direction in ("write", "read"):
                add("application_frame", "rust.native_muxer.application.io", phase=phase,
                    connection_id=connection, stream_id=stream, remote_peer_id=peer, direction=direction,
                    path="direct" if phase == "direct_after" else "relay",
                    payload_bytes=len(payload), payload_sha256=hashlib.sha256(payload).hexdigest())
            add("application_open", "rust.path_application.NotifyHandler.One.actual_owner" if phase == "relay_before"
                else "rust.stream.Control.open_stream.actual_owner", phase=phase,
                connection_id=connection, stream_id=stream, connected_before=True,
                opening_basis="native_existing_connection", dial_counter_basis="native_stream_behaviour_dial_requests",
                dial_attempts_before=0, dial_attempts_after=0, **exact)
            return add("echo", "rust.path_echo.io", phase=phase, connection_id=connection, stream_id=stream,
                remote_peer_id=peer, protocol="/forge/interop/path-echo/1", path="direct" if phase == "direct_after" else "relay",
                fresh_dial=False, server=False, io_basis="retained_native_stream", read_bytes=len(payload), write_bytes=len(payload),
                read_sha256=hashlib.sha256(payload).hexdigest(), write_sha256=hashlib.sha256(payload).hexdigest(),
                dial_attempts_before=1, dial_attempts_after=1)

        owner("direct", "carrier", "1", peers["relay"], {"direction": "outbound", "upgrade_role": "outbound",
              "remote_address": "/ip4/11.0.0.1/udp/4001/quic-v1"})
        add("baseline", "rust.swarm.connections", remote_peer_id=peer, direct_connection_ids=[])
        endpoint = {"direction": "outbound", "upgrade_role": "outbound", "remote_address": route + "/p2p/" + peer}
        if role == "destination":
            endpoint = {"direction": "inbound", "upgrade_role": "inbound", "local_address": route,
                        "remote_address": "/p2p/" + peer}
        inner = owner("relay", "circuit", "2", peer, endpoint)
        before = echo("relay_before", "circuit")
        pattern = (("read", 100), ("write", 100), ("read", 300)) if role == "source" else (
                   ("write", 100), ("read", 100), ("write", 300))
        for direction, kind in pattern:
            value = frame(direction, kind, [remote] if kind == 100 else [])
            for key in ("kind", "sequence", "mono_ns"):
                value.pop(key)
            value["remote_peer_id"] = peer
            add("dcutr_frame", "rust.native_muxer.dcutr.io", **value)
        requested = "dialer" if role == "source" else "listener"
        add("native_coordinated_dial", "rust.native_transport.dial_options", remote_peer_id=peer,
            requested_role=requested, address=remote)
        owner("direct", "direct", "3", peer, {"direction": "outbound", "remote_address": remote,
              "upgrade_role": "outbound" if role == "source" else "inbound"})
        add("native_coordinated_authenticated", "rust.native_quic.output_role", connection_id="direct",
            remote_peer_id=peer, authenticated=True, security_role="client",
            security_role_basis="pinned_quic_authenticated_transport_output", requested_role=requested)
        native = add("native_dcutr_event", "rust.dcutr.behaviour", remote_peer_id=peer,
                     result={"connection_id": "3", "native_success": True})
        receipt = {"native_connection_id": "2", "connection_id": "circuit", "stream_id": before["stream_id"],
            "remote_peer_id": peer, "direct_native_connection_id": "3", "direct_connection_id": "direct",
            "carrier_native_connection_id": "1", "carrier_connection_id": "carrier", "carrier_remote_peer_id": peers["relay"],
            "carrier_binding_basis": "unique_live_authenticated_carrier_for_native_circuit_route",
            "native_success_sequence": native["sequence"], "native_success_mono_ns": native["mono_ns"],
            "relay_before_sequence": before["sequence"], "relay_before_mono_ns": before["mono_ns"],
            "requested_sequence": None, "requested_mono_ns": None, "closed_sequence": None, "closed_mono_ns": None,
            "selection_policy": "success_only_explicit_inner_relay_retirement", "automatic_direct_preference_proven": False}
        request = add("relay_retirement_requested", "rust.swarm.close_connection", **receipt, native_close_accepted=True)
        request.update(requested_sequence=request["sequence"], requested_mono_ns=request["mono_ns"])
        actual = add("native_connection_closed", "rust.swarm.ConnectionClosed", native_connection_id="2",
                     connection_id="circuit", remote_peer_id=peer, endpoint=inner["endpoint"], cause=None, remaining_established=1)
        receipt.update(requested_sequence=request["sequence"], requested_mono_ns=request["mono_ns"],
                       closed_sequence=actual["sequence"], closed_mono_ns=actual["mono_ns"])
        add("relay_retirement_closed", "rust.swarm.ConnectionClosed", **receipt,
            native_closed_sequence=actual["sequence"], native_closed_mono_ns=actual["mono_ns"], cause=None,
            direct_still_live=True, carrier_still_live=True, inner_relay_still_live=False)
        terminal = add("retained_stream_terminal", "rust.path_echo.retained_stream.native_io", **receipt, phase="relay_before",
            terminal_basis="retained_initiator_actual_read_after_native_close", direction="read", io_bytes=0,
            error_kind="eof", error="EOF", expected_retirement=True)
        pre_call_receipt = {key: value for key, value in receipt.items()
                            if key not in {"native_connection_id", "connection_id", "stream_id"}}
        pre_call_receipt.update(retired_relay_native_connection_id="2", retired_relay_connection_id="circuit",
                                retired_relay_stream_id=before["stream_id"])
        add("application_open_requested", "rust.libp2p_stream.Control.open_stream.request", **pre_call_receipt, phase="direct_after",
            protocol="/forge/interop/path-echo/1", existing_connection_ids=["direct"], existing_direct_connection_ids=["direct"],
            connected_before=True, live_state_basis="native_authenticated_outputs_retained_until_ConnectionClosed",
            dial_attempts_before=0, dial_counter_basis="native_stream_behaviour_dial_requests",
            retained_terminal_sequence=terminal["sequence"], retained_terminal_mono_ns=terminal["mono_ns"],
            direct_still_live=True, carrier_still_live=True, inner_relay_still_live=False)
        echo("direct_after", "direct")
    return record


def restamp_retirement(events):
    """Keep unit references coherent when testing real event-order violations."""
    for index, event in enumerate(events, 1):
        event.update(sequence=index, mono_ns=index * 1_000_000)
    relay_request = next(e for e in events if e["kind"] == "application_open_requested" and e.get("phase") == "relay_before")
    relay_open = next(e for e in events if e["kind"] == "application_open" and e.get("phase") == "relay_before")
    for key in ("sequence", "mono_ns"):
        relay_open["requested_" + key] = relay_request[key]
    bound = {"native_success": next(e for e in events if e["kind"] == "native_dcutr_event"),
             "relay_before": next(e for e in events if e["kind"] == "echo" and e["phase"] == "relay_before"),
             "requested": next(e for e in events if e["kind"] == "relay_retirement_requested"),
             "closed": next(e for e in events if e["kind"] == "native_connection_closed" and e["native_connection_id"] == "2")}
    for event in events:
        if (event["kind"] not in {"relay_retirement_requested", "relay_retirement_closed", "retained_stream_terminal"}
                and not (event["kind"] == "application_open_requested" and event.get("phase") == "direct_after")):
            continue
        for prefix, linked in bound.items():
            if event["kind"] == "relay_retirement_requested" and prefix == "closed":
                continue
            for key in ("sequence", "mono_ns"):
                event[prefix + "_" + key] = linked[key]
        if event["kind"] == "relay_retirement_closed":
            for key in ("sequence", "mono_ns"):
                event["native_closed_" + key] = bound["closed"][key]
        if event["kind"] == "application_open_requested":
            terminal = next(e for e in events if e["kind"] == "retained_stream_terminal")
            for key in ("sequence", "mono_ns"):
                event["retained_terminal_" + key] = terminal[key]


class PathEvidenceTests(unittest.TestCase):
    def namespace_record(self, token):
        value = unit_record()
        network = value["network"]
        replacements = {network["outer_namespace"]: "path-o-" + token}
        replacements.update({p["namespace"]: f"path-{p['role']}-{token}" for p in network["participants"]})
        network["outer_namespace"] = replacements[network["outer_namespace"]]
        for participant in network["participants"]:
            participant["namespace"] = replacements[participant["namespace"]]
        for command in network["commands"]:
            command["command"] = [replacements.get(a, a) for a in command["command"]]
        for actor in value["raw"].values():
            actor["process"]["command"] = [replacements.get(a, a) for a in actor["process"]["command"]]
        return value

    def test_nat_namespace_token_matches_real_producer_bounds_and_pid_default(self):
        with patch("isolated_network.os.getpid", return_value=67566), patch("isolated_network.secrets.token_hex", return_value="01234567"):
            generated = PathNetwork().namespaces["outer"].removeprefix("path-o-")
        self.assertEqual(len(generated), 13)
        for token in ("0", "z", "abcxyz", "0" * 12, generated, "z9" * 10):
            value = self.namespace_record(token)
            expected = dict(PathNetwork(namespace_token=token).namespaces)
            namespaces = _nat(value["network"], "success")
            self.assertEqual({"outer": value["network"]["outer_namespace"], **namespaces}, expected)
            self.assertEqual(validate_case(value), [])

    def test_nat_namespace_rejects_invalid_token_bounds_charset_and_prefix(self):
        for token in ("", "a" * 21, "ABCDEF", "a-b", "a_b", "a/b", "a.b", "a b", "a\n", "a\x00", "\u0430"):
            with self.subTest(token=repr(token)):
                self.assertIn("missing owned outer namespace", validate_case(self.namespace_record(token))[0])
        for outer in ("fixture-o-abc123", "path-outer-abc123", "other-o-abc123", None):
            value = self.namespace_record("abc123")
            value["network"]["outer_namespace"] = outer
            self.assertIn("missing owned outer namespace", validate_case(value)[0])

    def test_nat_all_participants_and_execution_use_the_same_outer_owner_token(self):
        for role in ("relay", "source_router", "destination_router", "source", "destination"):
            for wrong in (f"path-{role}-foreign123", "path-other-abc123"):
                value = self.namespace_record("abc123")
                next(p for p in value["network"]["participants"] if p["role"] == role)["namespace"] = wrong
                self.assertIn("not scope-owned", validate_case(value)[0])
        value = self.namespace_record("abc123")
        value["network"]["outer_namespace"] = "path-o-foreign123"
        self.assertIn("not scope-owned", validate_case(value)[0])
        for role in ("source", "destination", "relay"):
            value = self.namespace_record("abc123")
            value["raw"][role]["process"]["command"][3] = f"path-{role}-foreign123"
            self.assertIn("not inside its owned NAT namespace", validate_case(value)[0])
        value = self.namespace_record("abc123")
        for command in value["network"]["commands"]:
            command["command"][3] = command["command"][3].replace("abc123", "foreign123")
        self.assertIn("lacks matching command output", validate_case(value)[0])

    def test_variable_namespace_token_does_not_relax_isolation_cleanup_or_commands(self):
        for key, wrong in (("state", "ready"), ("cleanup_failures", ["delete failed"]),
                           ("cleanup_uncertainty", ["unknown owner"]), ("outer_network", {"external_links": "present"})):
            value = self.namespace_record("z9" * 10)
            value["network"][key] = wrong
            self.assertTrue(validate_case(value))
        value = self.namespace_record("abc123")
        value["network"]["commands"][0]["returncode"] = 1
        self.assertIn("NAT command failed", validate_case(value)[0])

    def rust_relay_specs(self):
        return [s for s in case_specs() if "rust" in (s.source, s.destination)
                and (s.outcome != "failed" or s.source == "rust")]

    def rust_relay_record(self, spec):
        if spec.outcome == "success":
            return unit_rust_success(spec)
        return unit_source_wave(spec) if spec.source == "rust" else unit_cancellation(spec)

    def test_rust_relay_before_is_exact_one_and_direct_after_is_only_control(self):
        for spec in self.rust_relay_specs():
            value = self.rust_relay_record(spec)
            self.assertEqual(validate_case(value), [], spec.identifier)
            events = value["raw"][value["application_actor"]]["result"]["events"]
            before = next(e for e in events if e["kind"] == "application_open_requested" and e["phase"] == "relay_before")
            opened = next(e for e in events if e["kind"] == "application_open" and e["phase"] == "relay_before")
            self.assertEqual(before["source"], "rust.path_application.NotifyHandler.One.request")
            self.assertEqual(opened["source"], "rust.path_application.NotifyHandler.One.actual_owner")
            before["source"] = "rust.libp2p_stream.Control.open_stream.request"
            opened["source"] = "rust.stream.Control.open_stream.actual_owner"
            self.assertTrue(validate_case(value), "generic random Control is not the targeted relay-before producer")
            if spec.outcome == "success":
                value = self.rust_relay_record(spec)
                events = value["raw"][value["application_actor"]]["result"]["events"]
                for kind, source in (("application_open_requested", "rust.path_application.NotifyHandler.One.request"),
                                     ("application_open", "rust.path_application.NotifyHandler.One.actual_owner")):
                    broken = copy.deepcopy(value)
                    next(e for e in broken["raw"][broken["application_actor"]]["result"]["events"]
                         if e["kind"] == kind and e["phase"] == "direct_after")["source"] = source
                    self.assertTrue(validate_case(broken), "One is never a direct-after retirement substitute")

    def test_rust_one_request_and_actual_owner_fields_cannot_be_inferred(self):
        fields = {
            "application_open_requested": ("source", "native_connection_id", "connection_id", "path", "endpoint", "remote_peer_id",
                "protocol", "existing_connection_ids", "existing_direct_connection_ids", "connected_before", "live_state_basis",
                "target_binding_basis", "open_api_basis", "dial_attempts_before", "dial_counter_basis"),
            "application_open": ("source", "native_connection_id", "connection_id", "stream_id", "remote_peer_id", "connected_before",
                "opening_basis", "requested_sequence", "requested_mono_ns", "open_api_basis", "dial_counter_basis",
                "dial_attempts_before", "dial_attempts_after"),
        }
        for spec in self.rust_relay_specs():
            for kind, keys in fields.items():
                for key in keys:
                    for defect in ("missing", "foreign"):
                        value = self.rust_relay_record(spec)
                        event = next(e for e in value["raw"][value["application_actor"]]["result"]["events"]
                                     if e["kind"] == kind and e["phase"] == "relay_before")
                        if defect == "missing":
                            event.pop(key)
                        else:
                            event[key] = "rust.unbound" if key == "source" else "foreign-owner"
                        with self.subTest(case=spec.identifier, kind=kind, key=key, defect=defect):
                            self.assertTrue(validate_case(value))

    def test_rust_one_native_owner_requires_actual_live_swarm_endpoint(self):
        for spec in self.rust_relay_specs():
            for key, wrong in (("source", "rust.configured_endpoint"), ("native_connection_id", 2),
                               ("remote_peer_id", "foreign-peer"), ("endpoint", {"direction": "outbound"})):
                value = self.rust_relay_record(spec)
                events = value["raw"][value["application_actor"]]["result"]["events"]
                next(e for e in events if e["kind"] == "swarm_connection" and e["native_connection_id"] == "2")[key] = wrong
                self.assertTrue(validate_case(value))
            for native_id in (None, True, 2, "unknown", "1", "3"):
                value = self.rust_relay_record(spec)
                events = value["raw"][value["application_actor"]]["result"]["events"]
                for event in events:
                    if event.get("phase") == "relay_before" and event["kind"] in {"application_open_requested", "application_open"}:
                        event["native_connection_id"] = native_id
                self.assertTrue(validate_case(value))
            value = self.rust_relay_record(spec)
            events = value["raw"][value["application_actor"]]["result"]["events"]
            request = next(e for e in events if e["kind"] == "application_open_requested" and e["phase"] == "relay_before")
            events.insert(events.index(request), {"kind": "native_connection_closed", "source": "rust.swarm.ConnectionClosed",
                                                 "native_connection_id": "2", "connection_id": "circuit"})
            for i, event in enumerate(events, 1):
                event.update(sequence=i, mono_ns=i * 1_000_000)
            opening = next(e for e in events if e["kind"] == "application_open" and e["phase"] == "relay_before")
            opening.update(requested_sequence=request["sequence"], requested_mono_ns=request["mono_ns"])
            self.assertIn("closed before completed challenge", validate_case(value)[0])

    def test_rust_one_pre_call_precedes_both_actual_io_and_post_transfer_receipt(self):
        for spec in self.rust_success_specs():
            for kind, anchor in (("application_open_requested", "swarm_connection"),
                                 ("application_frame", "application_open_requested"),
                                 ("application_open", "application_frame")):
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                event = next(e for e in events if e["kind"] == kind and e.get("phase") == "relay_before")
                target = next(e for e in events if e["kind"] == anchor
                              and (e.get("native_connection_id") == "2" if anchor == "swarm_connection"
                                   else e.get("phase") == "relay_before"))
                events.remove(event)
                events.insert(events.index(target), event)
                restamp_retirement(events)
                self.assertTrue(validate_case(value), "coherent references do not excuse an out-of-order native call/I/O")
            for key in ("requested_sequence", "requested_mono_ns"):
                for wrong in (None, True, 0, 99999):
                    value = unit_rust_success(spec)
                    next(e for e in self.rust_success_events(value) if e["kind"] == "application_open"
                         and e["phase"] == "relay_before")[key] = wrong
                    self.assertTrue(validate_case(value))

    def test_rust_one_has_no_ordinary_dial_counter_spoof_or_future_stream(self):
        for spec in self.rust_relay_specs():
            for kind, key, wrong in (("application_open_requested", "dial_attempts_before", True),
                                     ("application_open_requested", "dial_attempts_before", 1),
                                     ("application_open", "dial_attempts_after", 1),
                                     ("application_open_requested", "dial_counter_basis", "configured_zero"),
                                     ("application_open_requested", "stream_id", "future-stream"),
                                     ("application_open_requested", "retired_relay_native_connection_id", "2")):
                value = self.rust_relay_record(spec)
                next(e for e in value["raw"][value["application_actor"]]["result"]["events"]
                     if e["kind"] == kind and e["phase"] == "relay_before")[key] = wrong
                self.assertTrue(validate_case(value))
            value = self.rust_relay_record(spec)
            events = value["raw"][value["application_actor"]]["result"]["events"]
            events.append({"kind": "application_dial_requested", "source": "rust.stream.Behaviour.dial",
                           "sequence": len(events) + 1, "mono_ns": events[-1]["mono_ns"] + 1})
            self.assertTrue(validate_case(value), "zero counters cannot hide an actual ordinary application dial")

    def test_rust_one_snapshot_allows_genuine_auto_direct_but_not_configured_labels(self):
        for spec in self.rust_success_specs():
            value = unit_rust_success(spec)
            events = self.rust_success_events(value)
            request = next(e for e in events if e["kind"] == "application_open_requested" and e["phase"] == "relay_before")
            before = next(e for e in events if e["kind"] == "echo" and e["phase"] == "relay_before")
            # Genuine native AUTO may win before the controller's barrier.
            # Move its entire wire/dial/auth/success prefix, not only a label.
            begin = events.index(before) + 1
            end = next(i for i, e in enumerate(events) if e["kind"] == "native_dcutr_event") + 1
            native = events[begin:end]
            del events[begin:end]
            offset = events.index(request)
            events[offset:offset] = native
            request["existing_direct_connection_ids"] = ["direct"]
            restamp_retirement(events)
            self.assertEqual(validate_case(value), [])
            request["existing_direct_connection_ids"] = []
            self.assertIn("not actual live transport owners", validate_case(value)[0])
            for ids in (["carrier"], ["configured-direct"], ["direct", "direct"]):
                request["existing_direct_connection_ids"] = ids
                self.assertTrue(validate_case(value))

    def source_wave_specs(self):
        return [s for s in case_specs() if rust_source_wave_case(asdict(s))]

    def test_source_wave_scope_proves_no_rust_aggregate_completion_or_join(self):
        for spec in self.source_wave_specs():
            value = unit_source_wave(spec)
            self.assertEqual(validate_case(value), [], spec.identifier)
            snapshot = value["native_wave_failure"]
            peer = value["raw"]["destination"]["ready"]["peer_id"]
            original = copy.deepcopy(value)
            self.assertTrue(rust_source_wave_failure(snapshot, TOKEN, peer))
            self.assertFalse(native_terminal(snapshot, "rust", peer, "failed"))
            self.assertFalse(native_terminal(snapshot, "rust", peer, "success"))
            self.assertEqual(value, original)
            if spec.outcome == "cancelled":
                self.assertTrue(rust_source_wave_pending(value["cancellation"]["wire_before_request"], TOKEN, peer))
                self.assertNotIn("natural_terminal", value["cancellation"])
        for spec in case_specs():
            if rust_source_wave_case(asdict(spec)):
                continue
            value = unit_record() if spec.outcome == "success" else unit_cancellation(
                next(s for s in case_specs() if s.source == spec.source and s.destination == spec.destination and s.outcome == "cancelled"))
            value["case"] = asdict(spec)
            value.update(terminal_scope=RUST_SOURCE_WAVE_SCOPE, native_wave_failure={})
            self.assertIn("exclusive to Rust SOURCE", validate_case(value)[0])

    def test_source_wave_requires_same_actual_native_origin_owner_and_typed_error(self):
        mutations = {
            "native_dcutr_dial_requested": ("source", "native_origin", "native_connection_id", "remote_peer_id", "source_wave_bound"),
            "native_dcutr_dial_options": ("source", "native_connection_id", "remote_peer_id", "requested_role", "native_callback_accepted",
                                           "native_returned_addresses", "candidate_match_basis", "candidate_addresses"),
            "native_dial_error": ("source", "native_connection_id", "remote_peer_id", "error"),
            "native_dcutr_wave_failed": ("source", "native_origin", "native_connection_id", "remote_peer_id", "dial_error_variant",
                                          "original_relay_live", "transport_errors"),
        }
        for spec in self.source_wave_specs():
            for kind, keys in mutations.items():
                for key in keys:
                    for defect in ("missing", "mismatch"):
                        value = unit_source_wave(spec)
                        event = next(e for e in value["raw"]["source"]["result"]["events"] if e["kind"] == kind)
                        if defect == "missing":
                            event.pop(key)
                        else:
                            event[key] = "" if key == "error" else "ordinary-or-unbound"
                        refresh_source_wave(value)
                        with self.subTest(case=spec.identifier, kind=kind, key=key, defect=defect):
                            self.assertTrue(validate_case(value))
            for kind in ("native_dcutr_dial_requested", "native_dcutr_dial_options", "native_dcutr_wave_failed"):
                for key in ("relay_native_connection_id", "relay_connection_id", "stream_id", "advertised_addresses", "binding_basis",
                            "proof_scope", "aggregate_dcutr_completed", "rust_behaviour_joined", "connect_read_sequence",
                            "connect_read_mono_ns", "connect_write_sequence", "connect_write_mono_ns", "sync_read_sequence",
                            "sync_read_mono_ns", "requested_sequence", "requested_mono_ns", "pending_sequence", "pending_mono_ns"):
                    value = unit_source_wave(spec)
                    event = next(e for e in value["raw"]["source"]["result"]["events"] if e["kind"] == kind)
                    event.pop(key)
                    refresh_source_wave(value)
                    with self.subTest(case=spec.identifier, kind=kind, missing=key):
                        self.assertTrue(validate_case(value))

    def test_source_wave_typed_quic_errors_cover_actual_candidates_without_text_parsing(self):
        mutations = {"transport_error_variant": "MultiaddrNotSupported", "quic_error_type": "std::io::Error",
            "quic_error_variant": "Unknown", "classification_basis": "error_string_contains_quic", "error": "",
            "wrapper_types": [], "address": "/ip4/11.0.0.2/udp/5000/quic-v1"}
        for spec in self.source_wave_specs():
            for key, replacement in mutations.items():
                value = unit_source_wave(spec)
                failure = next(e for e in value["raw"]["source"]["result"]["events"] if e["kind"] == "native_dcutr_wave_failed")
                failure["transport_errors"][0][key] = replacement
                refresh_source_wave(value)
                self.assertTrue(validate_case(value))
            value = unit_source_wave(spec)
            events = value["raw"]["source"]["result"]["events"]
            options = next(e for e in events if e["kind"] == "native_dcutr_dial_options")
            failure = next(e for e in events if e["kind"] == "native_dcutr_wave_failed")
            address = options["candidate_addresses"][0] + "/p2p/" + options["remote_peer_id"]
            options["candidate_addresses"] = [address]
            failure["candidate_addresses"] = [address]
            failure["transport_errors"][0]["address"] = address
            refresh_source_wave(value)
            if spec.outcome == "cancelled":
                value["cancellation"]["wire_before_request"]["events"] = copy.deepcopy(events[:events.index(options) + 1])
            self.assertEqual(validate_case(value), [])
            failure["transport_errors"].append(copy.deepcopy(failure["transport_errors"][0]))
            refresh_source_wave(value)
            self.assertTrue(validate_case(value))

    def test_source_wave_wire_origin_order_and_raw_error_references_are_strict(self):
        moves = (("native_dcutr_dial_requested", "dcutr_frame"),
                 ("native_dcutr_dial_options", "native_dcutr_dial_requested"),
                 ("native_dial_error", "native_dcutr_dial_options"),
                 ("native_dcutr_wave_failed", "native_dial_error"))
        for spec in self.source_wave_specs():
            for kind, anchor_kind in moves:
                value = unit_source_wave(spec)
                events = value["raw"]["source"]["result"]["events"]
                event = next(e for e in events if e["kind"] == kind)
                events.remove(event)
                events.insert(next(i for i, e in enumerate(events) if e["kind"] == anchor_kind), event)
                refresh_source_wave(value, restamp=True)
                self.assertTrue(validate_case(value))
            for key in ("raw_error_sequence", "raw_error_mono_ns"):
                for wrong in (None, True, -1, 99999):
                    value = unit_source_wave(spec)
                    next(e for e in value["raw"]["source"]["result"]["events"] if e["kind"] == "native_dcutr_wave_failed")[key] = wrong
                    refresh_source_wave(value)
                    self.assertTrue(validate_case(value))
            value = unit_source_wave(spec)
            next(e for e in value["raw"]["source"]["result"]["events"] if e["kind"] == "dcutr_frame")["source"] = "rust.configured_frame"
            refresh_source_wave(value)
            self.assertTrue(validate_case(value))

    def test_source_wave_scope_never_uses_unindexed_prefix_or_false_aggregate_and_retirement(self):
        for spec in self.source_wave_specs():
            for key in ("terminal_scope", "native_wave_failure"):
                value = unit_source_wave(spec)
                value.pop(key)
                self.assertTrue(validate_case(value))
            for key, wrong in (("implementation", "go"), ("case_token", "f" * 32), ("local_peer_id", "foreign"),
                               ("joined", True), ("finalized", True), ("overflow", True)):
                value = unit_source_wave(spec)
                value["native_wave_failure"][key] = wrong
                self.assertTrue(validate_case(value))
            value = unit_source_wave(spec)
            value["native_wave_failure"]["events"][0]["remote_peer_id"] = "foreign"
            self.assertTrue(validate_case(value))
            for kind in ("native_dcutr_event", "relay_retirement_requested", "native_dcutr_wave_binding_rejected"):
                value = unit_source_wave(spec)
                events = value["raw"]["source"]["result"]["events"]
                failure = next(e for e in events if e["kind"] == "native_dcutr_wave_failed")
                events.insert(events.index(failure), {"kind": kind, "source": "rust.unit_double",
                    "native_connection_id": "3", "remote_peer_id": failure["remote_peer_id"], "result": {"native_success": False}})
                refresh_source_wave(value, restamp=True)
                self.assertTrue(validate_case(value))

    def test_source_wave_cancel_requires_full_unfinished_origin_and_real_forge_join(self):
        spec = next(s for s in self.source_wave_specs() if s.outcome == "cancelled")
        value = unit_source_wave(spec)
        events = value["raw"]["source"]["result"]["events"]
        failure_index = next(i for i, e in enumerate(events) if e["kind"] == "native_dcutr_wave_failed")
        for end in (failure_index, failure_index + 1):
            broken = copy.deepcopy(value)
            broken["cancellation"]["wire_before_request"]["events"] = copy.deepcopy(events[:end])
            self.assertTrue(validate_case(broken), "raw error or bound wave failure cannot be an active pre-cancel phase")
        broken = copy.deepcopy(value)
        wire = broken["cancellation"]["wire_before_request"]
        wire["events"] = wire["events"][:next(i for i, e in enumerate(wire["events"]) if e["kind"] == "native_dcutr_dial_options")]
        self.assertTrue(validate_case(broken), "CONNECT alone cannot prove a pending SOURCE native wave")
        for kind, key, wrong in (("cancellation_requested", "source", "forge.node.stop"),
                                 ("native_cancel_completed", "accepted", False), ("native_service_joined", "accepted", False)):
            broken = copy.deepcopy(value)
            next(e for e in broken["raw"]["destination"]["result"]["events"] if e["kind"] == kind)[key] = wrong
            self.assertTrue(validate_case(broken))
        broken = copy.deepcopy(value)
        broken["cancellation"]["natural_terminal"] = copy.deepcopy(broken["native_wave_failure"])
        self.assertTrue(validate_case(broken), "native wave must not masquerade as a natural aggregate terminal")

    def test_source_wave_failure_still_requires_forge_failure_and_same_retained_relay_echo(self):
        for spec in self.source_wave_specs():
            for role in ("source", "destination"):
                value = unit_source_wave(spec)
                after = next(e for e in value["raw"][role]["result"]["events"] if e["kind"] == "echo" and e["phase"] == "relay_after")
                after["stream_id"] = "replacement-stream"
                self.assertTrue(validate_case(value))
            if spec.outcome == "failed":
                value = unit_source_wave(spec)
                events = value["raw"]["destination"]["result"]["events"]
                snapshot = next(e for e in events if e["kind"] == "native_snapshot" and e["hole_punch_failures"] == 1)
                events.remove(snapshot)
                events.append(snapshot)
                for index, event in enumerate(events, 1):
                    event.update(sequence=index, mono_ns=index * 1_000_000)
                self.assertIn("Forge native owner failure", validate_case(value)[0])

    def rust_success_specs(self):
        return [s for s in case_specs() if s.outcome == "success" and "rust" in (s.source, s.destination)]

    def rust_success_events(self, value):
        return value["raw"][value["application_actor"]]["result"]["events"]

    def test_rust_success_requires_explicit_retirement_in_both_directions(self):
        for spec in self.rust_success_specs():
            value = unit_rust_success(spec)
            self.assertEqual(validate_case(value), [], spec.identifier)
            original = copy.deepcopy(value)
            self.assertEqual(validate_case(value), [])
            self.assertEqual(value, original, "validator must not manufacture native receipts")
            from path_acceptance import validate_record
            self.assertEqual(validate_record(value, verify_provenance=lambda _: ["retirement double is not live provenance"]),
                             ["retirement double is not live provenance"])
            for kind in ("relay_retirement_requested", "relay_retirement_closed", "retained_stream_terminal",
                         "native_connection_closed", "application_open_requested"):
                broken = copy.deepcopy(value)
                events = self.rust_success_events(broken)
                events[:] = [e for e in events if e["kind"] != kind]
                for index, event in enumerate(events, 1):
                    event.update(sequence=index, mono_ns=index * 1_000_000)
                with self.subTest(case=spec.identifier, missing=kind):
                    self.assertIn(kind, validate_case(broken)[0])

    def test_rust_retirement_requires_exact_native_sources_and_acceptance(self):
        for spec in self.rust_success_specs():
            for kind in ("native_dcutr_event", "swarm_connection", "authenticated_connection",
                         "relay_retirement_requested", "native_connection_closed", "relay_retirement_closed",
                         "retained_stream_terminal", "echo", "application_frame", "application_open", "application_open_requested"):
                value = unit_rust_success(spec)
                next(e for e in self.rust_success_events(value) if e["kind"] == kind
                     and (kind != "application_open_requested" or e.get("phase") == "direct_after"))["source"] = "rust.configured_intent"
                with self.subTest(case=spec.identifier, kind=kind):
                    self.assertTrue(validate_case(value))
            value = unit_rust_success(spec)
            next(e for e in self.rust_success_events(value) if e["kind"] == "relay_retirement_requested")["native_close_accepted"] = False
            self.assertIn("accepted native", validate_case(value)[0])

    def test_rust_retirement_requires_actual_successful_direct_connid(self):
        for spec in self.rust_success_specs():
            for result in ({"native_success": False, "error": "native failure"},
                           {"native_success": True, "connection_id": "1"},
                           {"native_success": True, "connection_id": "2"},
                           {"native_success": True, "connection_id": "unestablished"}):
                value = unit_rust_success(spec)
                next(e for e in self.rust_success_events(value) if e["kind"] == "native_dcutr_event")["result"] = result
                self.assertTrue(validate_case(value))

    def test_rust_retirement_preserves_exact_native_transport_stream_and_peer_ids(self):
        for spec in self.rust_success_specs():
            for kind in ("relay_retirement_requested", "relay_retirement_closed", "retained_stream_terminal", "application_open_requested"):
                for key in ("native_connection_id", "connection_id", "stream_id", "remote_peer_id",
                            "direct_native_connection_id", "direct_connection_id", "carrier_native_connection_id",
                            "carrier_connection_id", "carrier_remote_peer_id", "carrier_binding_basis"):
                    value = unit_rust_success(spec)
                    if kind == "application_open_requested":
                        key = {"native_connection_id": "retired_relay_native_connection_id",
                               "connection_id": "retired_relay_connection_id", "stream_id": "retired_relay_stream_id"}.get(key, key)
                    next(e for e in self.rust_success_events(value) if e["kind"] == kind
                         and (kind != "application_open_requested" or e.get("phase") == "direct_after"))[key] = "replacement"
                    with self.subTest(case=spec.identifier, kind=kind, key=key):
                        self.assertTrue(validate_case(value))
            for native_id, key in (("1", "endpoint"), ("2", "remote_peer_id"), ("3", "endpoint")):
                value = unit_rust_success(spec)
                event = next(e for e in self.rust_success_events(value)
                             if e["kind"] == "swarm_connection" and e["native_connection_id"] == native_id)
                event[key] = {} if key == "endpoint" else "another_peer"
                self.assertTrue(validate_case(value))

    def test_rust_retirement_cannot_close_carrier_or_direct_instead_of_inner(self):
        for spec in self.rust_success_specs():
            for native_id, connection in (("1", "carrier"), ("3", "direct")):
                value = unit_rust_success(spec)
                for event in self.rust_success_events(value):
                    if event["kind"] in {"relay_retirement_requested", "native_connection_closed",
                                         "relay_retirement_closed", "retained_stream_terminal"}:
                        event.update(native_connection_id=native_id, connection_id=connection)
                self.assertTrue(validate_case(value))
            for key, replacement in (("connection_id", "carrier"), ("remote_peer_id", "another_peer"),
                                     ("endpoint", {}), ("cause", "transport failed"), ("remaining_established", 0),
                                     ("remaining_established", True)):
                value = unit_rust_success(spec)
                next(e for e in self.rust_success_events(value) if e["kind"] == "native_connection_closed")[key] = replacement
                self.assertIn("actual clean inner", validate_case(value)[0])

    def test_rust_retirement_stamps_reference_actual_events(self):
        for spec in self.rust_success_specs():
            for kind, prefixes in (("relay_retirement_requested", ("native_success", "relay_before", "requested")),
                                   ("relay_retirement_closed", ("native_success", "relay_before", "requested", "closed", "native_closed")),
                                   ("retained_stream_terminal", ("native_success", "relay_before", "requested", "closed")),
                                   ("application_open_requested", ("native_success", "relay_before", "requested", "closed", "retained_terminal"))):
                for prefix in prefixes:
                    for key in ("sequence", "mono_ns"):
                        for replacement in (None, True, -1, 123456789):
                            value = unit_rust_success(spec)
                            next(e for e in self.rust_success_events(value) if e["kind"] == kind
                                 and (kind != "application_open_requested" or e.get("phase") == "direct_after"))[prefix + "_" + key] = replacement
                            with self.subTest(case=spec.identifier, kind=kind, field=prefix + "_" + key, value=replacement):
                                self.assertIn("stamp", validate_case(value)[0])
            for key in ("closed_sequence", "closed_mono_ns"):
                value = unit_rust_success(spec)
                request = next(e for e in self.rust_success_events(value) if e["kind"] == "relay_retirement_requested")
                request[key] = 1
                self.assertIn("precompleted", validate_case(value)[0])
                request.pop(key)
                self.assertIn("precompleted", validate_case(value)[0])

    def test_rust_retirement_order_is_strict_with_coherent_references(self):
        moves = (("relay_retirement_requested", None, "echo", "relay_before"),
                 ("relay_retirement_requested", None, "native_dcutr_event", None),
                 ("native_connection_closed", None, "relay_retirement_requested", None),
                 ("relay_retirement_closed", None, "native_connection_closed", None),
                 ("retained_stream_terminal", None, "relay_retirement_closed", None),
                 ("application_open_requested", "direct_after", "native_connection_closed", None),
                 ("application_open_requested", "direct_after", "retained_stream_terminal", None),
                 ("application_open", "direct_after", "retained_stream_terminal", None),
                 ("application_open", "direct_after", "application_open_requested", "direct_after"),
                 ("application_open", "direct_after", "application_frame", "direct_after"),
                 ("application_frame", "direct_after", "application_open_requested", "direct_after"),
                 ("application_frame", "direct_after", "retained_stream_terminal", None))
        for spec in self.rust_success_specs():
            for kind, phase, anchor_kind, anchor_phase in moves:
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                event = next(e for e in events if e["kind"] == kind and (phase is None or e.get("phase") == phase))
                events.remove(event)
                anchor = next(e for e in events if e["kind"] == anchor_kind
                              and (anchor_phase is None or e.get("phase") == anchor_phase))
                events.insert(events.index(anchor), event)
                restamp_retirement(events)
                with self.subTest(case=spec.identifier, moved=kind, anchor=anchor_kind):
                    self.assertTrue(validate_case(value))

    def test_rust_pre_call_requires_actual_live_owner_protocol_and_dial_snapshots(self):
        mutations = [("phase", "relay_before"), ("phase", "relay_after"), ("remote_peer_id", "another_peer"),
                     ("protocol", "/other/1"), ("connected_before", False), ("connected_before", 1),
                     ("live_state_basis", "configured_direct"), ("direct_still_live", False),
                     ("carrier_still_live", False), ("inner_relay_still_live", True),
                     ("dial_counter_basis", "native_transport_dial_intent")]
        for key in ("existing_connection_ids", "existing_direct_connection_ids"):
            mutations.extend((key, value) for value in (None, "direct", [], ["circuit"], ["carrier"],
                             ["direct", "direct"], ["direct", "circuit"]))
        mutations.extend(("dial_attempts_before", value) for value in (None, True, -1, 1, 2, 0.0))
        for spec in self.rust_success_specs():
            for key, replacement in mutations:
                value = unit_rust_success(spec)
                event = next(e for e in self.rust_success_events(value)
                             if e["kind"] == "application_open_requested" and e["phase"] == "direct_after")
                event[key] = replacement
                with self.subTest(case=spec.identifier, key=key, value=replacement):
                    self.assertTrue(validate_case(value))
            value = unit_rust_success(spec)
            opening = next(e for e in self.rust_success_events(value) if e["kind"] == "application_open" and e["phase"] == "direct_after")
            opening.update(dial_attempts_before=2, dial_attempts_after=2)
            self.assertIn("pre-Control.open_stream", validate_case(value)[0])
            for key in ("source", "protocol", "existing_connection_ids", "existing_direct_connection_ids",
                        "connected_before", "live_state_basis", "dial_attempts_before", "dial_counter_basis",
                        "direct_still_live", "carrier_still_live", "inner_relay_still_live",
                        "retained_terminal_sequence", "retained_terminal_mono_ns"):
                value = unit_rust_success(spec)
                event = next(e for e in self.rust_success_events(value)
                             if e["kind"] == "application_open_requested" and e["phase"] == "direct_after")
                event.pop(key)
                with self.subTest(case=spec.identifier, missing=key):
                    self.assertTrue(validate_case(value))

    def test_rust_pre_call_uses_exact_retired_fields_without_legacy_owner_aliases(self):
        # Schema pinned to rust_fixture/path.rs SHA256 7ce13a6cdafa3ab0...
        fields = (("retired_relay_native_connection_id", "native_connection_id", "2"),
                  ("retired_relay_connection_id", "connection_id", "circuit"),
                  ("retired_relay_stream_id", "stream_id", "application-relay_before"))
        for spec in self.rust_success_specs():
            value = unit_rust_success(spec)
            pre_call = next(e for e in self.rust_success_events(value)
                            if e["kind"] == "application_open_requested" and e["phase"] == "direct_after")
            self.assertEqual(pre_call["dial_attempts_before"], 0)
            for key, alias, expected in fields:
                self.assertEqual(pre_call[key], expected)
                self.assertNotIn(alias, pre_call)
                for defect in ("missing", "mismatch", "alias_only", "alias_with_retired"):
                    broken = copy.deepcopy(value)
                    event = next(e for e in self.rust_success_events(broken)
                                 if e["kind"] == "application_open_requested" and e["phase"] == "direct_after")
                    if defect in {"missing", "alias_only"}:
                        event.pop(key)
                    if defect == "mismatch":
                        event[key] = "unrelated-native-owner"
                    if defect in {"alias_only", "alias_with_retired"}:
                        event[alias] = expected
                    with self.subTest(case=spec.identifier, field=key, defect=defect):
                        self.assertTrue(validate_case(broken))
            self.assertEqual(validate_case(value), [])

    def test_rust_pre_call_missing_or_duplicate_never_uses_another_phase_or_peer(self):
        for spec in self.rust_success_specs():
            value = unit_rust_success(spec)
            events = self.rust_success_events(value)
            direct_request = next(e for e in events if e["kind"] == "application_open_requested" and e["phase"] == "direct_after")
            events.remove(direct_request)
            restamp_retirement(events)
            self.assertIn("application_open_requested", validate_case(value)[0])
            for replacement in ({}, {"remote_peer_id": "another_peer"}, {"source": "rust.configured_intent"}):
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                event = copy.deepcopy(next(e for e in events
                    if e["kind"] == "application_open_requested" and e["phase"] == "direct_after"))
                event.update(replacement)
                events.insert(len(events) - 1, event)
                restamp_retirement(events)
                self.assertIn("ambiguous application_open_requested", validate_case(value)[0])

    def test_rust_pre_call_checks_transient_extra_native_owner_even_if_closed_before_echo(self):
        for spec in self.rust_success_specs():
            value = unit_rust_success(spec)
            events = self.rust_success_events(value)
            established = copy.deepcopy(next(e for e in events if e["kind"] == "swarm_connection" and e["native_connection_id"] == "3"))
            established.update(native_connection_id="4", endpoint={"direction": "outbound", "upgrade_role": "outbound",
                               "remote_address": "/ip4/11.0.0.2/udp/5000/quic-v1"})
            pre_call = next(e for e in events if e["kind"] == "application_open_requested" and e["phase"] == "direct_after")
            events.insert(events.index(pre_call), established)
            events.insert(events.index(pre_call) + 1, {"kind": "native_connection_closed", "source": "rust.swarm.ConnectionClosed",
                "native_connection_id": "4", "connection_id": "transient", "remote_peer_id": established["remote_peer_id"],
                "endpoint": established["endpoint"], "cause": None, "remaining_established": 1})
            restamp_retirement(events)
            self.assertIn("ambiguous live native owners", validate_case(value)[0])

    def test_rust_retirement_survival_flags_cannot_hide_actual_native_closure(self):
        for spec in self.rust_success_specs():
            for native_id, connection, position in (("1", "carrier", "relay_retirement_requested"),
                    ("3", "direct", "relay_retirement_requested"), ("1", "carrier", "application_frame"),
                    ("3", "direct", "application_frame"), ("1", "carrier", "application_open_requested"),
                    ("3", "direct", "application_open_requested")):
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                established = next(e for e in events if e["kind"] == "swarm_connection" and e["native_connection_id"] == native_id)
                closure = {"kind": "native_connection_closed", "source": "rust.swarm.ConnectionClosed",
                           "native_connection_id": native_id, "connection_id": connection,
                           "remote_peer_id": established["remote_peer_id"], "endpoint": established["endpoint"],
                           "cause": None, "remaining_established": 0}
                anchor = next(e for e in events if e["kind"] == position
                              and (position not in {"application_frame", "application_open_requested"} or e["phase"] == "direct_after"))
                events.insert(events.index(anchor), closure)
                restamp_retirement(events)
                self.assertIn("closed before direct echo", validate_case(value)[0])
            for key, replacement in (("direct_still_live", False), ("carrier_still_live", False), ("inner_relay_still_live", True)):
                value = unit_rust_success(spec)
                next(e for e in self.rust_success_events(value) if e["kind"] == "relay_retirement_closed")[key] = replacement
                self.assertIn("preserve direct", validate_case(value)[0])

    def test_rust_retirement_survival_does_not_claim_post_echo_host_shutdown_survival(self):
        for spec in self.rust_success_specs():
            value = unit_rust_success(spec)
            events = self.rust_success_events(value)
            for native_id, connection in (("3", "direct"), ("1", "carrier")):
                established = next(e for e in events if e["kind"] == "swarm_connection" and e["native_connection_id"] == native_id)
                events.append({"kind": "native_connection_closed", "source": "rust.swarm.ConnectionClosed",
                               "native_connection_id": native_id, "connection_id": connection,
                               "remote_peer_id": established["remote_peer_id"], "endpoint": established["endpoint"],
                               "cause": None, "remaining_established": 0})
            restamp_retirement(events)
            self.assertEqual(validate_case(value), [], "survival proof ends at direct I/O, not after stopping the host")

    def test_rust_retirement_requires_unique_original_live_owners_and_carrier_route(self):
        for spec in self.rust_success_specs():
            for kind, native_id in (("swarm_connection", "2"), ("swarm_connection", "3"), ("authenticated_connection", "carrier")):
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                field = "native_connection_id" if kind == "swarm_connection" else "connection_id"
                event = copy.deepcopy(next(e for e in events if e["kind"] == kind and e[field] == native_id))
                events.insert(0, event)
                restamp_retirement(events)
                self.assertTrue(validate_case(value))
            for native_id in ("2", "3"):
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                extra = copy.deepcopy(next(e for e in events if e["kind"] == "swarm_connection" and e["native_connection_id"] == native_id))
                extra["native_connection_id"] = "4"
                extra["endpoint"] = {"direction": "outbound", "upgrade_role": "outbound",
                                     "remote_address": "/ip4/11.0.0.2/udp/5000/quic-v1"}
                anchor = next(e for e in events if e["kind"] == "relay_retirement_requested")
                events.insert(events.index(anchor), extra)
                restamp_retirement(events)
                self.assertIn("ambiguous live", validate_case(value)[0])
            value = unit_rust_success(spec)
            events = self.rust_success_events(value)
            carrier = next(e for e in events if e["kind"] == "authenticated_connection" and e["connection_id"] == "carrier")
            carrier["remote_address"] = "/ip4/11.0.0.1/udp/5000/quic-v1"
            carrier["endpoint"]["remote_address"] = carrier["remote_address"]
            self.assertIn("carrier socket", validate_case(value)[0])

    def test_rust_retirement_requires_actual_terminal_read_not_timeout_or_flags(self):
        for spec in self.rust_success_specs():
            for replacement in ({"error_kind": "observation_timeout", "io_bytes": None},
                    {"error_kind": "io_error", "io_bytes": None}, {"error_kind": "unexpected_data", "io_bytes": 1},
                    {"io_bytes": True}, {"io_bytes": -1}, {"io_bytes": 1}, {"error_kind": "reset", "io_bytes": 0},
                    {"error": ""}, {"direction": "write"}, {"expected_retirement": False},
                    {"terminal_basis": "retained_handler_actual_read_error"}, {"phase": "relay_after"}):
                value = unit_rust_success(spec)
                terminal = next(e for e in self.rust_success_events(value) if e["kind"] == "retained_stream_terminal")
                terminal.update(replacement)
                with self.subTest(case=spec.identifier, fields=replacement):
                    self.assertIn("actual retained-stream", validate_case(value)[0])
            for kind in ("eof", "reset"):
                value = unit_rust_success(spec)
                terminal = next(e for e in self.rust_success_events(value) if e["kind"] == "retained_stream_terminal")
                terminal.update(error_kind=kind, io_bytes=None, error="native stream read failed")
                self.assertEqual(validate_case(value), [])
                terminal.pop("io_bytes")
                self.assertTrue(validate_case(value))

    def test_rust_retirement_requires_both_actual_muxer_io_and_exact_echo(self):
        for spec in self.rust_success_specs():
            for phase in ("relay_before", "direct_after"):
                for key, replacement in (("connection_id", "carrier"), ("stream_id", "old-stream"),
                        ("remote_peer_id", "another_peer"), ("path", "wrong"), ("direction", "write"),
                        ("payload_bytes", 0), ("payload_bytes", True), ("payload_sha256", "0" * 64)):
                    value = unit_rust_success(spec)
                    event = next(e for e in self.rust_success_events(value)
                                 if e["kind"] == "application_frame" and e["phase"] == phase and e["direction"] == "read")
                    event[key] = replacement
                    with self.subTest(case=spec.identifier, phase=phase, key=key):
                        self.assertTrue(validate_case(value))
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                events.remove(next(e for e in events if e["kind"] == "application_frame" and e["phase"] == phase))
                restamp_retirement(events)
                self.assertIn("both actual native", validate_case(value)[0])
                value = unit_rust_success(spec)
                next(e for e in self.rust_success_events(value) if e["kind"] == "echo" and e["phase"] == phase)["read_sha256"] = "0" * 64
                self.assertIn("actual challenge", validate_case(value)[0])
                value = unit_rust_success(spec)
                events = self.rust_success_events(value)
                frames = [e for e in events if e["kind"] == "application_frame" and e["phase"] == phase]
                left, right = events.index(frames[0]), events.index(frames[1])
                events[left], events[right] = events[right], events[left]
                restamp_retirement(events)
                self.assertIn("I/O is out of order", validate_case(value)[0])
            value = unit_rust_success(spec)
            events = self.rust_success_events(value)
            events.append({"kind": "capture_error", "source": "rust.native_muxer.io", "stream_id": "application-direct_after"})
            restamp_retirement(events)
            self.assertIn("capture failed", validate_case(value)[0])

    def test_rust_retirement_never_claims_automatic_preference_or_relabeled_stream(self):
        for spec in self.rust_success_specs():
            for kind in ("relay_retirement_requested", "relay_retirement_closed", "retained_stream_terminal", "application_open_requested"):
                for key, replacement in (("automatic_direct_preference_proven", True), ("selection_policy", "automatic_direct_preference")):
                    value = unit_rust_success(spec)
                    next(e for e in self.rust_success_events(value) if e["kind"] == kind
                         and (kind != "application_open_requested" or e.get("phase") == "direct_after"))[key] = replacement
                    self.assertIn("automatic direct preference", validate_case(value)[0])
            value = unit_rust_success(spec)
            for event in self.rust_success_events(value):
                if event.get("phase") == "direct_after":
                    event["stream_id"] = "application-relay_before"
            self.assertTrue(validate_case(value))

    def test_retirement_is_forbidden_in_all_failed_cancelled_and_nonrust_actors(self):
        for spec in case_specs():
            if spec.outcome == "success":
                continue
            # The retirement scope gate runs before terminal/fault semantics;
            # preserve a valid cancelled record so rejection is not incidental.
            cancelled = next(s for s in case_specs() if s.source == spec.source and s.destination == spec.destination
                             and s.outcome == "cancelled")
            for role in ("source", "destination", "relay"):
                for kind in ("relay_retirement_requested", "relay_retirement_closed", "retained_stream_terminal", "application_open_requested"):
                    value = unit_cancellation(cancelled)
                    value["case"] = asdict(spec)
                    events = value["raw"][role]["result"]["events"]
                    events.append({"kind": kind, "source": getattr(spec, role) + ".unit_double",
                                   "sequence": len(events) + 1, "mono_ns": events[-1]["mono_ns"] + 1,
                                   **({"phase": "direct_after"} if kind == "application_open_requested" else {})})
                    with self.subTest(case=spec.identifier, role=role, kind=kind):
                        self.assertIn("only on the successful Rust", validate_case(value)[0])
            value = unit_cancellation(cancelled)
            value["case"] = asdict(spec)
            donor = value["application_actor"]
            if getattr(spec, donor) == "rust":
                before = next(e for e in value["raw"][donor]["result"]["events"] if e["kind"] == "application_open_requested")
                before["closed_sequence"] = 1
                self.assertIn("only on the successful Rust", validate_case(value)[0])
        for role in ("source", "destination", "relay"):
            value = unit_record()
            events = value["raw"][role]["result"]["events"]
            events.append({"kind": "relay_retirement_requested", "source": value["case"][role] + ".unit_double",
                           "sequence": len(events) + 1, "mono_ns": events[-1]["mono_ns"] + 1})
            self.assertIn("only on the successful Rust", validate_case(value)[0])

    def test_handler_failure_after_echo_is_not_success(self):
        value = unit_record()
        events = value["raw"]["source"]["result"]["events"]
        events.append({"kind": "application_error", "source": "forge.path_echo.native_failure",
                       "sequence": len(events) + 1, "mono_ns": events[-1]["mono_ns"] + 1,
                       "error": "native close failed"})
        self.assertIn("handler failed", validate_case(value)[0])

    def test_other_forge_source_cannot_bypass_carrier_contract(self):
        value = unit_record()
        event = next(e for e in value["raw"]["source"]["result"]["events"]
                     if e["kind"] == "authenticated_connection" and e["path"] == "relay")
        event["source"] = "forge.other_capture"
        event.pop("carrier_connection_id")
        self.assertIn("unknown Forge", validate_case(value)[0])

    def test_all_cancellation_directions_keep_go_and_forge_owner_contracts(self):
        for spec in case_specs():
            if spec.outcome != "cancelled":
                continue
            value = unit_cancellation(spec)
            self.assertEqual(validate_case(value), [], spec.identifier)
            owner = value["cancellation"]["actor"]
            events = value["raw"][owner]["result"]["events"]
            if getattr(spec, owner) == "go":
                next(e for e in events if e["kind"] == "native_handlers_drained")["active"] = 1
                self.assertIn("actually drain", validate_case(value)[0])
            else:
                next(e for e in events if e["kind"] == "native_cancel_completed")["accepted"] = False
                self.assertIn("accept and join", validate_case(value)[0])

    def test_captured_079_protocol_error_is_terminal_for_cancel_not_failed_acceptance(self):
        prefix = captured_079_go_protocol_error_prefix()
        peer = prefix["events"][-1]["remote_peer_id"]
        validate_frame(prefix["events"][5])
        self.assertEqual(go_cancel_state(prefix, prefix["case_token"], peer, "destination"),
                         {"state": "terminal", "start_sequence": None, "terminal_sequence": 8,
                          "terminal_type": "ProtocolError"})
        self.assertFalse(native_terminal(prefix, "go", peer, "failed"))
        with self.assertRaisesRegex(ValueError, "missing complete native DCUtR handshake"):
            _handshakes(prefix["events"], peer, prefix["events"][4], "destination", "failed")

    def test_captured_go_connect_before_auth_snapshot_uses_peer_baseline_without_reordering(self):
        prefix = captured_023_go_connect_before_auth_prefix()
        original = copy.deepcopy(prefix)
        peer = prefix["events"][-1]["remote_peer_id"]
        state = go_cancel_state(prefix, prefix["case_token"], peer, "destination")
        self.assertEqual(state, {"state": "active", "start_sequence": 9, "terminal_sequence": None,
            "terminal_type": None, "connection_id": "12D3KooWHG-2", "stream_id": "12D3KooWHG-2-10",
            "start_mono_ns": 446126293})
        self.assertEqual(prefix, original, "validation must not reorder native capture events")

    def test_go_first_wave_still_requires_unique_baseline_and_exact_authenticated_wire(self):
        for mutation, error in (
                ("absent_auth", "authenticated_connection"), ("unverified", "authenticated connection"),
                ("foreign_auth_peer", "authenticated_connection"), ("foreign_connection", "circuit peer/stream"),
                ("foreign_wire_peer", "circuit peer/stream"), ("split_stream", "roles/order"),
                ("absent_baseline", "baseline"), ("foreign_baseline", "baseline"),
                ("duplicate_baseline", "baseline"), ("preexisting_direct", "baseline already became direct"),
                ("wire_before_baseline", "unique current complete"), ("two_current_streams", "unique current complete"),
                ("candidate_mismatch", "candidates differ")):
            with self.subTest(mutation=mutation):
                prefix = captured_023_go_connect_before_auth_prefix()
                events = prefix["events"]
                baseline, write, auth, read, sync, start = events[2], *events[4:9]
                peer = start["remote_peer_id"]
                if mutation == "absent_auth":
                    events.remove(auth)
                elif mutation == "unverified":
                    auth["authenticated"] = False
                elif mutation == "foreign_auth_peer":
                    auth["remote_peer_id"] = "foreign"
                elif mutation == "foreign_connection":
                    read["connection_id"] = "foreign"
                elif mutation == "foreign_wire_peer":
                    read["remote_peer_id"] = "foreign"
                elif mutation == "split_stream":
                    read["stream_id"] = "foreign"
                elif mutation == "absent_baseline":
                    events.remove(baseline)
                elif mutation == "foreign_baseline":
                    baseline["remote_peer_id"] = "foreign"
                elif mutation == "duplicate_baseline":
                    events.insert(3, copy.deepcopy(baseline))
                elif mutation == "preexisting_direct":
                    baseline["direct_connection_ids"] = ["old-direct"]
                elif mutation == "wire_before_baseline":
                    events.remove(baseline)
                    events.insert(events.index(auth) + 1, baseline)
                elif mutation == "two_current_streams":
                    extra = copy.deepcopy([write, read, sync])
                    for event in extra:
                        event["stream_id"] = "another-current-stream"
                    events[events.index(start):events.index(start)] = extra
                else:
                    start["addresses"] = ["/ip4/11.0.0.9/udp/42060/quic-v1"]
                # Only the negative structural doubles get rebased after mutation.
                for index, event in enumerate(events, 1):
                    event.update(sequence=index, mono_ns=index * 1_000_000)
                with self.assertRaisesRegex(ValueError, error):
                    go_cancel_state(prefix, prefix["case_token"], peer, "destination")

    def test_protocol_error_before_cancel_cannot_pass_with_empty_handler_drain(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        value = unit_cancellation(spec)
        events = value["raw"]["destination"]["result"]["events"]
        frames = [e for e in events if e["kind"] == "dcutr_frame"]
        events[:] = [e for e in events if e not in frames[1:] and e["kind"] != "holepunch_trace"]
        captured = captured_079_go_protocol_error_prefix()["events"]
        terminal, error = copy.deepcopy(captured[6:8])
        for event in (terminal, error):
            event.update(remote_peer_id="forge_peer", connection_id="circuit", stream_id="dcutr-1")
        error.pop("connection_id"); error.pop("stream_id")
        error["local_peer_id"] = "go_peer"
        events[events.index(frames[0]) + 1:events.index(frames[0]) + 1] = [terminal, error]
        next(e for e in events if e["kind"] == "native_handlers_drained").update(entered=0, completed=0, active=0)
        refresh_go_cancel_prefix(value)
        self.assertIn("unfinished donor Go native method claim", validate_case(value)[0])

    def test_failed_native_attempt_then_new_start_can_be_actively_cancelled(self):
        for spec in case_specs():
            if "go" not in (spec.source, spec.destination) or spec.outcome != "cancelled":
                continue
            with self.subTest(direction=spec.identifier):
                value = unit_go_retry_cancellation(spec)
                role = value["cancellation"]["actor"]
                peer = "forge_peer"
                prefix = value["cancellation"]["before_request"]
                state = go_cancel_state(prefix, TOKEN, peer, role)
                self.assertEqual(state["state"], "active")
                self.assertEqual(state["stream_id"], "dcutr-2")
                self.assertGreater(state["start_sequence"], state["terminal_sequence"])
                self.assertTrue(native_terminal(prefix, "go", peer, "failed"))
                self.assertEqual(validate_case(value), [])

    def test_protocol_error_only_reopens_after_fresh_full_exchange_and_native_start(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        value = unit_go_retry_cancellation(spec)
        events = value["raw"]["destination"]["result"]["events"]
        ended = next(e for e in events if e.get("native_type") == "EndHolePunch")
        ended.update(native_type="ProtocolError", error="native protocol failure")
        ended.pop("success")
        refresh_go_cancel_prefix(value)
        self.assertEqual(validate_case(value), [])
        prefix = value["cancellation"]["before_request"]
        prefix["events"] = prefix["events"][:-1]
        self.assertEqual(go_cancel_state(prefix, TOKEN, "forge_peer", "destination")["state"], "terminal")

    def test_go_cancel_requires_native_start_not_labels_or_full_wire_alone(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        for mutation in ("missing", "foreign_peer", "foreign_local", "fake_source", "missing_timestamp", "wrong_candidates"):
            with self.subTest(mutation=mutation):
                value = unit_cancellation(spec)
                events = value["raw"]["destination"]["result"]["events"]
                start = next(e for e in events if e.get("native_type") == "StartHolePunch")
                if mutation == "missing":
                    events.remove(start)
                elif mutation == "foreign_peer":
                    start["remote_peer_id"] = "someone_else"
                elif mutation == "foreign_local":
                    start["local_peer_id"] = "someone_else"
                elif mutation == "fake_source":
                    start["source"] = "go.path_control.intent"
                elif mutation == "missing_timestamp":
                    start.pop("native_unix_ns")
                else:
                    start["addresses"] = ["/ip4/11.0.0.9/udp/4001/quic-v1"]
                refresh_go_cancel_prefix(value)
                self.assertTrue(validate_case(value))

    def test_go_cancel_current_start_requires_full_unique_current_wire_exchange(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        for mutation in ("missing_sync", "historical_wire", "ambiguous_wire", "non_native_wire", "reused_retry_stream"):
            with self.subTest(mutation=mutation):
                value = unit_go_retry_cancellation(spec)
                events = value["raw"]["destination"]["result"]["events"]
                frames = [e for e in events if e["kind"] == "dcutr_frame" and e["stream_id"] == "dcutr-2"]
                if mutation == "missing_sync":
                    events.remove(frames[-1])
                elif mutation == "historical_wire":
                    events[:] = [e for e in events if e not in frames]
                elif mutation == "non_native_wire":
                    frames[0]["source"] = "go.path_control.intent"
                elif mutation == "reused_retry_stream":
                    for event in frames:
                        event["stream_id"] = "dcutr-1"
                else:
                    extra = copy.deepcopy(frames)
                    for event in extra:
                        event["stream_id"] = "dcutr-3"
                    at = events.index(frames[-1]) + 1
                    events[at:at] = extra
                refresh_go_cancel_prefix(value)
                self.assertTrue(validate_case(value))

    def test_go_cancel_revalidates_actual_events_between_observation_and_request(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        for retry in (False, True):
            with self.subTest(retry=retry):
                value = unit_cancellation(spec)
                observed = copy.deepcopy(value["cancellation"])
                events = value["raw"]["destination"]["result"]["events"]
                if retry:
                    value = unit_go_retry_cancellation(spec)
                else:
                    end = copy.deepcopy(next(e for e in events if e.get("native_type") == "EndHolePunch"))
                    at = next(i for i, e in enumerate(events) if e["kind"] == "cancellation_requested")
                    events.insert(at, end)
                    refresh_go_cancel_prefix(value)
                value["cancellation"] = observed
                self.assertIn("changed or completed before request", validate_case(value)[0])

    def test_go_cancel_requires_selected_method_terminal_before_native_drain_and_join(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        for mutation in ("missing_end", "late_end", "successful_end", "late_start"):
            with self.subTest(mutation=mutation):
                value = unit_cancellation(spec)
                events = value["raw"]["destination"]["result"]["events"]
                end = next(e for e in events if e.get("native_type") == "EndHolePunch")
                if mutation == "missing_end":
                    events.remove(end)
                elif mutation == "successful_end":
                    end["success"] = True
                elif mutation == "late_end":
                    events.remove(end)
                    at = next(i for i, e in enumerate(events) if e["kind"] == "native_service_joined")
                    events.insert(at, end)
                else:
                    start = copy.deepcopy(next(e for e in events if e.get("native_type") == "StartHolePunch"))
                    at = next(i for i, e in enumerate(events) if e["kind"] == "native_service_joined") + 1
                    events.insert(at, start)
                refresh_go_cancel_prefix(value)
                self.assertTrue(validate_case(value))

    def test_go_destination_native_method_claim_does_not_invent_inbound_handler_workers(self):
        spec = next(s for s in case_specs() if s.source == "forge" and s.destination == "go" and s.outcome == "cancelled")
        value = unit_cancellation(spec)
        events = value["raw"]["destination"]["result"]["events"]
        next(e for e in events if e["kind"] == "native_handlers_drained").update(entered=0, completed=0, active=0)
        self.assertEqual(validate_case(value), [])

    def test_host_stop_completed_attempt_and_replaced_relay_cannot_prove_cancel(self):
        spec = next(s for s in case_specs() if s.outcome == "cancelled")
        value = unit_cancellation(spec)
        owner = value["cancellation"]["actor"]
        events = value["raw"][owner]["result"]["events"]
        next(e for e in events if e["kind"] == "cancellation_requested")["source"] = "go.host.Close.request"
        self.assertTrue(validate_case(value))
        value = unit_cancellation(spec)
        value["cancellation"]["wire_before_request"] = copy.deepcopy(value["cancellation"]["wire_before_request"])
        value["cancellation"]["wire_before_request"]["events"] = value["raw"][owner]["result"]["events"]
        self.assertIn("unfinished donor", validate_case(value)[0])
        value = unit_cancellation(spec)
        next(e for e in value["raw"][owner]["result"]["events"] if e["kind"] == "echo" and e["phase"] == "relay_after")["stream_id"] = "replacement"
        self.assertIn("original", validate_case(value)[0])
    def test_accessible_native_shapes_compose_without_forge_operation_or_frame_invention(self):
        value = unit_record()
        self.assertEqual(validate_case(value), [])
        from path_acceptance import validate_record
        self.assertEqual(validate_record(value, verify_provenance=lambda _: ["unit double is not live provenance"]),
                         ["unit double is not live provenance"])

    def test_preexisting_direct_and_missing_donor_write_cannot_pass(self):
        value = unit_record()
        value["raw"]["source"]["result"]["events"][0]["direct_connection_ids"] = ["old-direct"]
        self.assertIn("preexisting", validate_case(value)[0])
        value = unit_record()
        frames = [e for e in value["raw"]["destination"]["result"]["events"] if e["kind"] == "dcutr_frame"]
        frames[0]["receipt"].pop("write")
        self.assertIn("actual I/O", validate_case(value)[0])

    def test_actual_read_and_write_use_same_numeric_wire_validator(self):
        for address in ("/ip4/127.0.0.1/udp/4001/quic-v1", "/ip4/11.0.0.2/udp/4001/quic-v1", "/ip6/2001:db8::1/udp/4001/quic-v1"):
            for direction in ("read", "write"):
                value = frame(direction, 100, [address])
                original = copy.deepcopy(value)
                validate_frame(value)
                self.assertEqual(value, original, "validation must not manufacture logged read receipts")
                value["receipt"][direction]["framed_sha256"] = "0" * 64
                with self.assertRaises(ValueError):
                    validate_frame(value)

    def test_one_donor_proves_both_roles_all_three_real_frames(self):
        addresses = ["/ip4/11.0.0.2/udp/4001/quic-v1"]
        for role, pattern in (("source", [("read", 100), ("write", 100), ("read", 300)]),
                              ("destination", [("write", 100), ("read", 100), ("write", 300)])):
            values = [frame(direction, kind, addresses if kind == 100 else [], index)
                      for index, (direction, kind) in enumerate(pattern, 1)]
            self.assertEqual(len(_handshakes(values, "expected", {"connection_id": "circuit"}, role, "success")), 1)
            with self.assertRaises(ValueError):
                _handshakes(values[:2], "expected", {"connection_id": "circuit"}, role, "success")
            values[-1]["stream_id"] = "another-stream"
            with self.assertRaises(ValueError):
                _handshakes(values, "expected", {"connection_id": "circuit"}, role, "success")

    def test_failed_retry_prefix_requires_actual_bound_terminal_io(self):
        for role, pattern in (("source", [("read", 100), ("write", 100), ("read", 300)]),
                              ("destination", [("write", 100), ("read", 100), ("write", 300)])):
            completed = [frame(direction, kind, ["/ip4/11.0.0.2/udp/4001/quic-v1"] if kind == 100 else [], index)
                         for index, (direction, kind) in enumerate(pattern, 1)]
            for value in completed:
                value["source"] = "go.unit_double"
            retry = copy.deepcopy(completed[0])
            retry.update(stream_id="retry", sequence=4, mono_ns=4_000_000)
            terminal = {"kind": "dcutr_stream_terminal", "source": "go.native_dcutr.io", "sequence": 5,
                        "mono_ns": 5_000_000, "connection_id": "circuit", "stream_id": "retry",
                        "remote_peer_id": "expected", "protocol": "/libp2p/dcutr", "direction": pattern[1][0],
                        "error": "native stream reset", "error_kind": "reset", "io_bytes": 0,
                        "completed_frame_count": 0, "pending_frame_bytes": 0, "invalid_or_over_limit": False}
            values = completed + [retry, terminal]
            self.assertEqual(len(_handshakes(values, "expected", {"connection_id": "circuit"}, role, "failed")), 1)
            for key, replacement in (("source", "go.unit_inferred"), ("stream_id", "foreign"),
                                      ("connection_id", "foreign"), ("remote_peer_id", "foreign"),
                                      ("direction", pattern[0][0]), ("protocol", "/foreign"),
                                      ("error_kind", "success"), ("error", ""), ("sequence", 3),
                                      ("mono_ns", 1), ("completed_frame_count", 1), ("io_bytes", -1),
                                      ("pending_frame_bytes", 4097), ("invalid_or_over_limit", True)):
                with self.subTest(role=role, field=key):
                    bad = copy.deepcopy(values)
                    bad[-1][key] = replacement
                    with self.assertRaises(ValueError):
                        _handshakes(bad, "expected", {"connection_id": "circuit"}, role, "failed")
            for bad in (completed + [retry], values + [copy.deepcopy(terminal)], [retry, terminal]):
                with self.assertRaises(ValueError):
                    _handshakes(bad, "expected", {"connection_id": "circuit"}, role, "failed")

    def test_native_secured_receipt_binds_exact_owner_peer_role_and_sockets(self):
        for key, replacement in (("source", "go.unit_inferred"), ("native_connection_basis", "swarm.Stat.Direction"),
                                  ("native_connection_id", ""), ("local_peer_id", "foreign"),
                                  ("remote_peer_id", "foreign"), ("direction", "outbound"),
                                  ("security_role", "client"), ("observed_mono_ns", 2**63),
                                  ("local_address", "/ip4/10.2.0.2/udp/5001/quic-v1"),
                                  ("remote_address", "/ip4/11.0.0.3/udp/4001/quic-v1")):
            with self.subTest(field=key):
                value = unit_record()
                coordinated = next(e for e in value["raw"]["destination"]["result"]["events"]
                                   if e["kind"] == "native_coordinated_authenticated")
                coordinated["secured_receipt"][key] = replacement
                self.assertTrue(validate_case(value))

    def test_relay_inner_security_is_not_outer_quic(self):
        value = session("relay")
        _session([value], "expected", "relay")
        value["security"] = "/tls/1.0.0"
        _session([value], "expected", "relay")
        value["transport"] = "quic"
        with self.assertRaises(ValueError):
            _session([value], "expected", "relay")

    def test_direct_needs_actual_quic_tls_not_noise(self):
        value = session("direct")
        _session([value], "expected", "direct")
        value["security"] = "/noise"
        with self.assertRaises(ValueError):
            _session([value], "expected", "direct")

    def test_forge_logical_circuit_requires_distinct_native_carrier(self):
        value = session("relay")
        value.update(source="forge.authenticated_stream.diagnostics", relay_peer_id="relay-peer",
                     remote_address="/ip4/11.0.0.1/udp/4001/quic-v1/p2p/relay-peer/p2p-circuit/p2p/expected",
                     local_address=None, endpoint_basis="logical_authenticated_circuit_route",
                     carrier_basis="forge.node.diagnostics.native_carrier", carrier_connection_id="outer",
                     carrier_remote_peer_id="relay-peer", carrier_local_address="/ip4/10.1.0.2/udp/4001/quic-v1",
                     carrier_remote_address="/ip4/11.0.0.1/udp/4001/quic-v1")
        _session([value], "expected", "relay")
        for field, replacement in (("endpoint_basis", "getsockname"), ("carrier_basis", "configured"),
                                   ("carrier_connection_id", "relay"), ("carrier_remote_peer_id", "foreign"),
                                   ("carrier_local_address", None), ("carrier_remote_address", "/ip4/11.0.0.1/tcp/4001"),
                                   ("carrier_remote_address", "/ip4/10.1.0.2/udp/4001/quic-v1"),
                                   ("remote_address", value["remote_address"] + "-foreign")):
            with self.subTest(field=field):
                bad = copy.deepcopy(value)
                bad[field] = replacement
                with self.assertRaises((ValueError, TypeError)):
                    _session([bad], "expected", "relay")

    def test_fresh_dial_during_stream_open_is_not_existing_direct_echo(self):
        owner = session("direct")
        payload = challenge(TOKEN, "direct_after")
        opening = {"kind": "application_open", "sequence": 2, "stream_id": "application", "connection_id": "direct",
                   "connected_before": True, "dial_attempts_before": 9, "dial_attempts_after": 9,
                   "opening_basis": "native_existing_connection", "phase": "direct_after",
                   "source": "rust.stream.Control.open_stream.actual_owner", "dial_counter_basis": "native_stream_behaviour_dial_requests"}
        echo = {"kind": "echo", "sequence": 3, "phase": "direct_after", "protocol": "/forge/interop/path-echo/1",
                "connection_id": "direct", "remote_peer_id": "expected", "stream_id": "application", "path": "direct",
                "fresh_dial": False, "server": False, "io_basis": "retained_native_stream",
                "dial_attempts_before": 9, "dial_attempts_after": 11,
                "read_bytes": len(payload), "write_bytes": len(payload), "read_sha256": hashlib.sha256(payload).hexdigest(),
                "write_sha256": hashlib.sha256(payload).hexdigest()}
        _echo([owner, opening, echo], TOKEN, "direct_after", owner)
        opening["dial_attempts_after"] = 10
        with self.assertRaises(ValueError):
            _echo([owner, opening, echo], TOKEN, "direct_after", owner)

    def test_native_retries_during_relay_io_do_not_implicate_application_open(self):
        value = unit_record()
        for role in ("source", "destination"):
            for event in value["raw"][role]["result"]["events"]:
                if event["kind"] == "echo":
                    event["dial_attempts_after"] += 3
                if event["kind"] == "application_open":
                    event["dial_attempts_after"] += 1
        self.assertEqual(validate_case(value), [])

    def test_quic_reuse_needs_actual_listener_socket_or_conntrack_tuple(self):
        value = unit_record()
        direct = next(e for e in value["raw"]["destination"]["result"]["events"]
                      if e["kind"] == "authenticated_connection" and e["path"] == "direct")
        direct["local_address"] = "/ip4/10.2.0.2/udp/5001/quic-v1"
        self.assertIn("local socket", validate_case(value)[0])
        direct["local_address"] = None  # Rust's inaccessible outbound socket.
        snapshot = next(s for s in value["network"]["snapshots"] if s["phase"] == "after_upgrade")
        flows = snapshot["routers"]["destination_router"]["conntrack"].replace("sport=4001", "sport=5001", 1)
        original = snapshot["routers"]["destination_router"]["conntrack"]
        snapshot["routers"]["destination_router"]["conntrack"] = flows
        for other_snapshot in value["network"]["snapshots"]:
            router = other_snapshot["routers"]["destination_router"]
            if router["conntrack"] == original:
                router["conntrack"] = flows
        for command in value["network"]["commands"]:
            if command["stdout"] == original:
                command["stdout"] = flows
        self.assertIn("conntrack UDP local IP/port", validate_case(value)[0])

    def test_standard_conntrack_whitespace_preserves_exact_ports(self):
        value = unit_record()
        for snapshot in value["network"]["snapshots"]:
            for router in snapshot["routers"].values():
                router["conntrack"] = router["conntrack"].replace("udp 17", "udp      17")
        for command in value["network"]["commands"]:
            command["stdout"] = command["stdout"].replace("udp 17", "udp      17")
        self.assertEqual(validate_case(value), [])
        for replacement in ("sport=5001", "sport=0", "sport=65536"):
            wrong = copy.deepcopy(value)
            for snapshot in wrong["network"]["snapshots"]:
                for router in snapshot["routers"].values():
                    router["conntrack"] = router["conntrack"].replace("sport=4001", replacement, 1)
            for command in wrong["network"]["commands"]:
                command["stdout"] = command["stdout"].replace("sport=4001", replacement, 1)
            self.assertIn("conntrack UDP local IP/port", validate_case(wrong)[0])

    def test_security_role_must_not_be_only_requested_role(self):
        value = unit_record()
        coordinated = next(e for e in value["raw"]["destination"]["result"]["events"]
                           if e["kind"] == "native_coordinated_authenticated")
        coordinated.pop("security_role_basis")
        self.assertIn("actual QUIC handshake", validate_case(value)[0])

    def test_metrics_and_status_alone_cannot_pass(self):
        samples = [{"kind": "native_snapshot", "remote_peer_id": "expected", "hole_punch_successes": n} for n in (0, 1)]
        self.assertTrue(native_terminal({"events": samples}, "forge", "expected", "success"))
        self.assertTrue(validate_case({"status": "passed", "case": {"profile": "native", "transport": "quic", "outcome": "success"},
                                      "result": {"events": samples, "relay_echo": True}}))

    def test_nat_requires_exact_rule_output_not_topology_labels(self):
        network = {"kind": "linux_dcutr_conntrack_nat", "state": "closed", "outer_network": {"external_links": "absent"},
                   "commands": [{"returncode": 0}], "snapshots": []}
        with self.assertRaises(ValueError):
            _nat(network, "success")


if __name__ == "__main__":
    unittest.main()
