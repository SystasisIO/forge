"""Pure mDNS receipt checks. Runtime counts are not peer-specific discovery proof.

The caller must supply immutable final results, independent process statuses,
launcher-bound readiness and the closed two-participant network evidence.
No timeout is accepted as a successful negative isolation outcome.
"""

import hashlib
import ipaddress
import json
import re

from provenance import reject_duplicate_json_keys


SCHEMA = "forge.mdns.interop.v1"
PROTOCOL = "/forge/interop/relay-echo/1"
PUBLIC_SERVICE = "_p2p._udp.local"
RESULT_LIMIT = 1024 * 1024
BINDINGS = {
    "forge": "single_lifetime_session_and_stream_peer",
    "go": "echo_stream_conn",
    "rust": "single_lifetime_swarm_connection_and_stream_peer",
}


def decode_receipt(text):
    if not isinstance(text, str) or len(text.encode("utf-8")) > RESULT_LIMIT:
        raise ValueError("oversized or non-text mDNS receipt")
    try:
        value = json.loads(text, object_pairs_hook=reject_duplicate_json_keys,
                           parse_constant=lambda value: (_ for _ in ()).throw(ValueError(value)))
    except (ValueError, RecursionError) as error:
        raise ValueError("malformed mDNS receipt") from error
    return _object(value, "receipt")


def _object(value, label):
    if not isinstance(value, dict):
        raise ValueError(f"{label} must be an object")
    return value


def _token(value, label, maximum=256):
    if (not isinstance(value, str) or not 0 < len(value) <= maximum
            or any(ord(c) < 33 or ord(c) > 126 for c in value)):
        raise ValueError(f"invalid {label}")
    return value


def _integer(value, label, minimum=0, maximum=2**63 - 1):
    if type(value) is not int or not minimum <= value <= maximum:
        raise ValueError(f"invalid {label}")
    return value


def _terminal(value):
    value = _object(value, "terminal status")
    if (set(value) != {"exit_code", "termination"} or type(value["exit_code"]) is not int
            or value["exit_code"] != 0 or value["termination"] != "graceful"):
        raise ValueError("mDNS requires independent graceful exit 0")


def _endpoint(value, *, peer, ip, transport, wildcard=False):
    _token(value, "endpoint", 512)
    parts = value.split("/")
    if len(parts) >= 3 and parts[-2] == "p2p":
        if parts[-1] != peer:
            raise ValueError("endpoint peer suffix mismatch")
        parts = parts[:-2]
    quic = transport == "quic"
    if (len(parts) != (6 if quic else 5) or parts[0] or parts[1] not in ("ip4", "ip6")
            or parts[3] != ("udp" if quic else "tcp") or (quic and parts[5] != "quic-v1")):
        raise ValueError("endpoint must be a direct numeric address of the actual transport")
    if "%" in parts[2]:
        raise ValueError("remote endpoint must not supply a zone")
    address = ipaddress.ip_address(parts[2])
    expected = ipaddress.ip_address(ip)
    if (parts[1] != f"ip{address.version}" or address.version != expected.version
            or not parts[4].isascii() or not parts[4].isdecimal()):
        raise ValueError("endpoint family/port mismatch")
    port = int(parts[4])
    if not 0 < port < 65536 or str(port) != parts[4]:
        raise ValueError("invalid endpoint port")
    if address != expected and not (quic and wildcard and address.is_unspecified):
        raise ValueError("endpoint is outside its isolated participant")
    return address, port


def _network(value, family):
    value = _object(value, "network")
    if (value.get("schema") != "forge.mdns.network.v1" or value.get("kind") != "linux_isolated_mdns_netns"
            or value.get("state") != "closed" or type(value.get("family")) is not int
            or value["family"] != family or value.get("cleanup_failures") != []
            or value.get("cleanup_uncertainty") != [] or value.get("outer_network") != {
                "bridge": "br0", "default_route": "absent", "external_links": "absent"}):
        raise ValueError("missing closed isolated mDNS network evidence")
    if value.get("multicast") != {
        "group": "224.0.0.251" if family == 4 else "ff02::fb", "port": 5353,
        "bridge_snooping": "disabled_by_checked_command",
    }:
        raise ValueError("mDNS multicast setup mismatch")
    participants = value.get("participants")
    if not isinstance(participants, list) or len(participants) != 2:
        raise ValueError("mDNS requires exactly two network participants")
    interfaces = _object(value.get("interfaces"), "interfaces")
    if set(interfaces) != {"client", "server"}:
        raise ValueError("mDNS interface participant mismatch")
    addresses, namespaces = {}, set()
    outer = _token(value.get("outer_namespace"), "outer namespace")
    for participant in participants:
        participant = _object(participant, "participant")
        role = participant.get("role")
        if role not in ("client", "server") or role in addresses:
            raise ValueError("duplicate/unknown network participant")
        namespace = _token(participant.get("namespace"), "namespace")
        if namespace in namespaces or namespace == outer:
            raise ValueError("network namespaces are not distinct")
        namespaces.add(namespace)
        values = participant.get("addresses")
        if not isinstance(values, list) or len(values) != 1 or not isinstance(values[0], str) or "%" in values[0]:
            raise ValueError("participant must have one unscoped address")
        address = ipaddress.ip_address(values[0])
        if (address.version != family or address.is_unspecified or address.is_multicast
                or address.is_loopback or address.is_link_local):
            raise ValueError("unusable participant address")
        interface = _object(interfaces[role], "interface")
        if (interface.get("name") != "eth0" or interface.get("address") != str(address)
                or interface.get("up") is not True or interface.get("multicast") is not True):
            raise ValueError("interface does not corroborate participant address")
        _integer(interface.get("index"), "interface index", 1, 2**32 - 1)
        addresses[role] = str(address)
    if addresses["client"] == addresses["server"]:
        raise ValueError("participants share an address")
    return addresses


def _ready(value, implementation, role, service):
    value = _object(value, "readiness")
    if (value.get("schema") != SCHEMA or value.get("status") != "ready"
            or value.get("role") != role or value.get("service_name") != service
            or ("implementation" in value and value["implementation"] != implementation)):
        raise ValueError("readiness schema/role/service mismatch")
    peer = _token(value.get("local_peer_id"), "ready peer")
    if "/" in peer:
        raise ValueError("invalid ready peer")
    return peer


def _result(value, implementation, role, peer, other_peer, own_ip, other_ip, transport, service, payload):
    value = _object(value, "result")
    if (value.get("schema") != SCHEMA or value.get("implementation") != implementation
            or value.get("role") != role or value.get("status") != "ok"
            or value.get("local_peer_id") != peer or value.get("service_name") != service):
        raise ValueError("result identity/schema/role/service mismatch")
    for key in ("failure", "error", "cleanup_error", "handler_error", "trace_error"):
        if value.get(key) is not None:
            raise ValueError(f"result reports {key}")
    if value.get("cleanup_errors") not in (None, []):
        raise ValueError("result reports cleanup errors")
    if implementation == "go" and value.get("echo_handlers_joined") is not True:
        raise ValueError("Go receipt is not finalized after handler joins")
    if implementation == "rust":
        tasks = _object(value.get("fixture_task_lifecycle"), "Rust task lifecycle")
        upgrade = _object(value.get("upgrade_observation"), "Rust upgrade observation")
        if (tasks.get("scope") != "public_swarm_executor_and_fixture_echo_handler"
                or tasks.get("shutdown_mode") != "close_admission_abort_join_after_swarm_drop"
                or tasks.get("fixture_owned_tasks_joined") is not True
                or tasks.get("overflow") is not False or tasks.get("errors") != []
                or upgrade.get("finalized_after_swarm_drop") is not True
                or upgrade.get("fixture_owned_tasks_joined") is not True or upgrade.get("overflow") is not False):
            raise ValueError("Rust receipt lacks clean finalized task/upgrade evidence")
    discovery = _object(value.get("discovery"), "discovery")
    if discovery.get("source") != "mdns":
        raise ValueError("discovery is not mDNS")
    if implementation == "forge":
        if (set(discovery) != {"source", "basis", "runtime_observation_count", "observations_before_outbound_dial"}
                or discovery["basis"] != "runtime_observation_count"):
            raise ValueError("Forge must not claim peer-specific mDNS discovery")
        _integer(discovery["runtime_observation_count"], "mDNS observation count", 1, 1)
        _integer(discovery["observations_before_outbound_dial"], "pre-dial observation count",
                 1 if role == "dialer" else 0, 1 if role == "dialer" else 0)
    else:
        if discovery.get("peer_id") != other_peer:
            raise ValueError("donor discovery peer differs from authenticated counterpart")
        discovered = discovery.get("addresses")
        if not isinstance(discovered, list) or not 0 < len(discovered) <= 16:
            raise ValueError("missing/beyond-bound donor discovery addresses")
        normalized = [_endpoint(a, peer=other_peer, ip=other_ip, transport=transport) for a in discovered]
        if len(set(normalized)) != len(normalized):
            raise ValueError("duplicate donor discovery addresses")
    connection = _object(value.get("connection"), "connection")
    identifier = _token(connection.get("id"), "connection id")
    if (connection.get("local_peer_id") != peer or connection.get("remote_peer_id") != other_peer
            or connection.get("direction") != ("outbound" if role == "dialer" else "inbound")
            or connection.get("binding") != BINDINGS[implementation]
            or connection.get("transport") != ("/quic-v1" if transport == "quic" else "tcp")):
        raise ValueError("actual connection binding/transport/direction mismatch")
    security, muxer = connection.get("security"), connection.get("muxer")
    if transport != "quic":
        if security != ("/tls/1.0.0" if transport in ("tcp-tls", "tcp-pnet") else "/noise"):
            raise ValueError("actual TCP security mismatch")
        if muxer != (None if implementation == "forge" else "/yamux/1.0.0"):
            raise ValueError("TCP muxer differs from available implementation evidence")
    elif (security != {"forge": "quic-tls", "go": "", "rust": None}[implementation]
          or muxer != ("" if implementation == "go" else None)):
        raise ValueError("unexpected QUIC security/muxer evidence")
    remote = _endpoint(connection.get("remote_address"), peer=other_peer, ip=other_ip, transport=transport)
    if implementation != "forge" and role == "dialer" and remote not in normalized:
        raise ValueError("donor dial endpoint is absent from its mDNS discovery addresses")
    local_value = connection.get("local_address")
    local = None
    if local_value is None:
        if implementation != "rust" or role != "dialer" or transport != "quic":
            raise ValueError("missing observed local socket endpoint")
    else:
        local = _endpoint(local_value, peer=peer, ip=own_ip, transport=transport,
                          wildcard=role == "dialer" and transport == "quic")
    echo = _object(value.get("echo"), "echo")
    if (echo.get("protocol") != PROTOCOL or type(echo.get("bytes")) is not int
            or echo["bytes"] != len(payload) or echo.get("sha256") != hashlib.sha256(payload).hexdigest()
            or echo.get("connection_id") != identifier or echo.get("remote_peer_id") != other_peer):
        raise ValueError("echo is not bound to the challenge and authenticated connection")
    if implementation == "go":
        _token(echo.get("stream_id"), "Go stream id")
    elif implementation == "forge":
        _integer(echo.get("stream_id"), "Forge stream id")
    return local, remote


def validate_mdns_evidence(client, server, *, client_impl, server_impl, transport, family,
                           client_ready, server_ready, client_terminal_status, server_terminal_status,
                           network, payload, private_fingerprint=None):
    """Return errors; empty means this paired artifact satisfies the declared basis."""
    try:
        if ({client_impl, server_impl} not in ({"forge", "go"}, {"forge", "rust"})
                or transport not in ("tcp", "tcp-tls", "quic", "tcp-pnet")
                or type(family) is not int or family not in (4, 6)):
            raise ValueError("unregistered mDNS implementation/transport/family combination")
        if not isinstance(payload, bytes) or not 0 < len(payload) <= 4096:
            raise ValueError("invalid challenge bytes")
        service = PUBLIC_SERVICE
        if transport == "tcp-pnet":
            if "rust" in (client_impl, server_impl) or not isinstance(private_fingerprint, str) or not re.fullmatch(
                    r"[0-9a-f]{32}", private_fingerprint):
                raise ValueError("private mDNS requires Forge/Go and the donor-compatible fingerprint")
            service = f"_p2p-{private_fingerprint}._udp.local"
        elif private_fingerprint is not None:
            raise ValueError("public mDNS must not declare a private fingerprint")
        _terminal(client_terminal_status)
        _terminal(server_terminal_status)
        addresses = _network(network, family)
        cp = _ready(client_ready, client_impl, "dialer", service)
        sp = _ready(server_ready, server_impl, "listener", service)
        if cp == sp:
            raise ValueError("mDNS peers must have distinct identities")
        cl, cr = _result(client, client_impl, "dialer", cp, sp, addresses["client"], addresses["server"],
                         transport, service, payload)
        sl, sr = _result(server, server_impl, "listener", sp, cp, addresses["server"], addresses["client"],
                         transport, service, payload)
        if cr != sl or (cl is not None and (cl[1] != sr[1] or (not cl[0].is_unspecified and cl[0] != sr[0]))):
            raise ValueError("independently observed socket endpoints do not pair")
        # Null Rust outbound QUIC is deliberately not fabricated. The server
        # independently observes its concrete source, already bound above.
        return []
    except (ValueError, TypeError, KeyError, RecursionError, UnicodeError) as error:
        return [str(error)]
