#!/usr/bin/env python3
"""Synthetic checker inputs, not live AutoNAT evidence or a donor PASS claim."""

import copy
import json
import unittest

from autonat_evidence import validate_autonat_evidence


CLIENT = "client-peer"
SERVICE = "service-peer"
PROBE = "independent-go-probe-peer"
PROTOCOLS = {1: "/libp2p/autonat/1.0.0", 2: "/libp2p/autonat/2/dial-request"}


def address(ip, port, transport):
    suffix = f"udp/{port}/quic-v1" if transport == "quic" else f"tcp/{port}"
    return f"/ip4/{ip}/{suffix}"


def fixture_result(implementation, role, version, transport):
    peer = CLIENT if role == "dialer" else SERVICE
    result = {
        "implementation": implementation,
        "role": role,
        "scenario": f"autonat_v{version}",
        "version": version,
        "protocol": PROTOCOLS[version],
        "transport": transport,
        "status": "ok",
        "internet_egress": "allow" if transport == "tcp-pnet" else "",
        "policy_denied": False,
        "service_enabled": role == "listener",
        "reached": role == "dialer",
        "requested_addr": None,
        "response_addr": None,
        "actual_connections": [],
    }
    if implementation == "forge":
        result.update(
            local_peer_id=peer,
            hosts_closed=True,
            handlers_joined=True,
            # Forge exposes complete gater evidence, not a generic trace claim.
            trace_complete=False,
            connection_gater_trace_complete=True,
            connection_gater_trace_failure=None,
            nonce_proof="not_exposed_by_public_API",
            response_addr_proof="not_exposed_by_public_API",
            autonat_probe_api_calls=1 if role == "dialer" else 0,
            v2_fixture_candidate_count=1,
            probe_state="public" if role == "dialer" else "unknown",
            effective_state="unknown",
            autonat_v1_state="unknown",
            v1_vote="public" if version == 1 and role == "dialer" else None,
            autonat_v2_address_state=None,
            current_v2_address_state=None,
        )
        before = {
            "reachability": {
                "internet_egress_allowed": True,
                f"{'client' if role == 'dialer' else 'service'}_v{version}_enabled": True,
            },
        }
        after = {
            "lifecycle_phase": "stopped",
            "active_sessions": 0,
            "resources": dict.fromkeys((
                "system_memory", "inbound_connections", "outbound_connections",
                "active_dials", "active_service_scopes",
            ), 0),
            "reachability": {"pending_probes": 0, "active_handlers": 0},
        }
        result["diagnostics_before_stop"] = json.dumps(before)
        result["diagnostics_after_stop"] = json.dumps(after)
    elif implementation == "go":
        result.update(
            local_peer_id=peer,
            donor_revision="9cfe2cc00be5b20a0be737f002c99f81b92255c5",
            hosts_closed=True,
            handlers_joined=True,
            hosts_created=2,
            trace_complete=True,
            nonce_proof="not_exposed_by_donor_api",
            autonat_probe_api_calls=1 if role == "dialer" else 0,
            response_index=0 if version == 2 else None,
            dial_data_bytes=None,
            donor_api_basis=(
                "NewAutoNATClient.DialBack + bounded_copy_of_actual_v1_response"
                if version == 1 else
                "autonatv2.New/Start/GetReachability: donor_verified_result"
            ),
            probe_peer_id=PROBE,
            probe_connections=[],
            service_completed_requests=[],
        )
    else:
        result.update(
            peer_id=peer,
            donor_revision="22fb4c784fc55ad8b15d05fdc9f98d663107d4cb",
            swarm_closed=True,
            connection_drain=True,
            joined_swarm_executor=True,
            trace_complete=True,
            trace_overflow_error=None,
            donor_events=[],
            nonce_proof="not_applicable_v1" if version == 1 else "not_exposed_by_donor_api",
            autonat_probe_api_calls=None,
            response_addr_basis=(
                "matched_response_event" if version == 1 else
                "tested_addr_from_verified_event_not_raw_wire_response"
            ),
            donor_api_basis=(
                "v1::Behaviour::OutboundProbe::Response: donor_verified_result"
                if version == 1 else
                "v2::client::Behaviour::Event: donor_verified_result"
            ),
            v1_request_events=1 if version == 1 and role == "dialer" else 0,
            v2_result_events=1 if version == 2 and role == "dialer" else 0,
            dial_data_bytes=30000 if version == 2 and role == "dialer" else None,
            # Only v2 reports a candidate through the reporter behaviour.
            candidate_reported=version == 2,
        )
    return result


def connection(result, identifier, direction, peer, local, remote, fresh=False):
    record = {
        "direction": direction,
        "authenticated": True,
        "authenticated_peer": peer,
        "local_addr": local,
        "remote_addr": remote,
    }
    if result["implementation"] == "forge":
        return [dict(record, stage="secured"), dict(record, stage="upgraded")]
    record["connection_id"] = identifier
    if result["implementation"] == "rust":
        record.update(fresh_requested_inbound=fresh, closed=True)
        if direction == "outbound":
            record["local_addr"] = None  # Swarm API does not expose this socket.
    return [record]


def valid_pair(client_impl="forge", server_impl="go", version=1,
               transport="tcp", negative=False, reuse_client_port=False):
    client = fixture_result(client_impl, "dialer", version, transport)
    server = fixture_result(server_impl, "listener", version, transport)
    control_local = address("11.0.0.1", 41000, transport)
    control_remote = address("11.0.0.2", 42000, transport)
    requested = address("11.0.0.3" if version == 2 else "11.0.0.1",
                        9 if negative else 43000, transport)
    if reuse_client_port:
        control_local = requested
    probe_local = address("11.0.0.2", 44000, transport)
    client["requested_addr"] = requested
    client["actual_connections"] = connection(
        client, "control-1", "outbound", SERVICE, control_local, control_remote,
    )
    server["actual_connections"] = connection(
        server, "control-1", "inbound", CLIENT, control_remote, control_local,
    )
    if client_impl == "rust":
        client.update(authenticated_peer=SERVICE, control_connection_id="control-1",
                      observer_reported_control_addr=control_local)
    else:
        client.update(control_authenticated_peer=SERVICE, observer_peer_id=SERVICE,
                      control_identify_completed=True)
        if client_impl == "go":
            client.update(control_connection_id="control-1",
                          control_local_addr=control_local, control_remote_addr=control_remote)
    if client_impl == "forge" and version == 2:
        state = json.dumps({"address": requested, "state": "public"})
        client.update(autonat_v2_address_state=state, current_v2_address_state=state)
    elif client_impl != "forge":
        client["response_addr"] = requested
    if negative:
        client.update(expected_outcome="unreachable",
                      candidate_basis="explicit_unreachable_negative_control",
                      reached=False, v1_vote="private", probe_state="private",
                      probe_elapsed_ms=15005, v2_fixture_candidate_count=0)
    else:
        probe_peer = PROBE if server_impl == "go" else SERVICE
        if client_impl == "go":
            client["authenticated_peer"] = probe_peer
        incoming = connection(client, "dialback-2", "inbound", probe_peer,
                              requested, probe_local, fresh=True)
        client["actual_connections"].extend(incoming)
        if client_impl != "forge":
            client["fresh_inbound_connections"] = copy.deepcopy(incoming)
            client["fresh_inbound_count"] = 1
        outgoing = connection(server, "probe-1", "outbound", CLIENT,
                              probe_local, requested)
        if server_impl == "go":
            server["probe_connections"] = outgoing
            if version == 2:
                server["service_completed_requests"] = [{
                    "dial_data_required": not reuse_client_port,
                    "dial_status": "OK", "response_status": "OK",
                    "dialed_addr": requested,
                }]
        else:
            server["actual_connections"].extend(outgoing)
    for result in (client, server):
        if result["implementation"] != "rust":
            continue
        dialing = result["role"] == "dialer"
        if version == 1:
            prefix = "v1_outbound" if dialing else "v1_inbound"
            request = {
                "protocol": PROTOCOLS[1], "kind": prefix + "_request",
                "probe_id": "ProbeId(7)", "peer": SERVICE if dialing else CLIENT,
                "event": "diagnostic only",
            }
            if not dialing:
                request["addresses"] = [requested]
            result["donor_events"] = [request, {
                "protocol": PROTOCOLS[1], "kind": prefix + "_response",
                "probe_id": "ProbeId(7)", "peer": SERVICE if dialing else CLIENT,
                "address": requested, "success": True, "event": "diagnostic only",
            }]
        else:
            result["donor_events"] = [{
                "protocol": PROTOCOLS[2],
                "kind": "v2_client_result" if dialing else "v2_server_result",
                "server" if dialing else "client": SERVICE if dialing else CLIENT,
                "tested_addr": requested, "success": True,
                "bytes_sent" if dialing else "data_amount": 30000,
                "result": "diagnostic only",
            }]
    return {
        "client": client, "server": server, "version": version, "transport": transport,
        "expected_service_peer": SERVICE, "expected_requested_address": requested,
        "expected_outcome": "unreachable" if negative else "reachable",
        "client_terminal_status": {"exit_code": 0, "termination": "graceful"},
        "server_terminal_status": {"exit_code": 0, "termination": "graceful"},
    }


class AutoNATEvidenceTests(unittest.TestCase):
    def assert_invalid(self, pair, message=None):
        errors = validate_autonat_evidence(**pair)
        self.assertTrue(errors)
        if message is not None:
            self.assertTrue(any(message in error for error in errors), errors)

    def test_positive_paired_versions_transports_and_donors(self):
        for client, server in (("forge", "go"), ("go", "forge"),
                               ("forge", "rust"), ("rust", "forge")):
            for version in (1, 2):
                for transport in ("tcp", "tcp-tls", "tcp-pnet", "quic"):
                    with self.subTest(client=client, server=server, version=version,
                                      transport=transport):
                        pair = valid_pair(client, server, version, transport)
                        original = copy.deepcopy(pair)
                        self.assertEqual(validate_autonat_evidence(**pair), [])
                        self.assertEqual(pair, original)

    def test_quic_wildcard_outbound_binds_require_concrete_counterparts(self):
        for client, server, version in (("forge", "go", 1), ("forge", "go", 2),
                                        ("go", "forge", 1)):
            with self.subTest(client=client, version=version):
                pair = valid_pair(client, server, version, "quic")
                for side in ("client", "server"):
                    result = pair[side]
                    for field in ("actual_connections", "probe_connections"):
                        for entry in result.get(field, []):
                            if entry["direction"] == "outbound":
                                parts = entry["local_addr"].split("/")
                                parts[2] = "0.0.0.0"
                                entry["local_addr"] = "/".join(parts)
                    if client == "go" and side == "client":
                        result["control_local_addr"] = result["actual_connections"][0]["local_addr"]
                original = copy.deepcopy(pair)
                self.assertEqual(validate_autonat_evidence(**pair), [])
                self.assertEqual(pair, original)  # Never rewrite the native wildcard IP.

    def test_quic_ipv6_wildcard_is_family_and_port_bound(self):
        pair = valid_pair(transport="quic")
        replacements = {"/ip4/11.0.0.1/": "/ip6/2606:4700::1111/",
                        "/ip4/11.0.0.2/": "/ip6/2606:4700::2222/"}
        for side in ("client", "server"):
            for field in ("actual_connections", "probe_connections"):
                for entry in pair[side].get(field, []):
                    for key in ("local_addr", "remote_addr"):
                        for old, new in replacements.items():
                            entry[key] = entry[key].replace(old, new)
                    if entry["direction"] == "outbound":
                        parts = entry["local_addr"].split("/")
                        parts[2] = "::"
                        entry["local_addr"] = "/".join(parts)
        requested = "/ip6/2606:4700::1111/udp/43000/quic-v1"
        pair["client"]["requested_addr"] = pair["expected_requested_address"] = requested
        self.assertEqual(validate_autonat_evidence(**pair), [])
        for wrong in ("/ip4/0.0.0.0/udp/44000/quic-v1", "/ip6/::/udp/44001/quic-v1"):
            with self.subTest(wrong=wrong):
                changed = copy.deepcopy(pair)
                changed["server"]["probe_connections"][0]["local_addr"] = wrong
                self.assert_invalid(changed, "concrete counterpart")

    def test_quic_wildcard_missing_or_wrong_counterpart_is_not_proof(self):
        pair = valid_pair(transport="quic")
        pair["server"]["probe_connections"][0]["local_addr"] = "/ip4/0.0.0.0/udp/44000/quic-v1"
        for mutation in ("missing", "peer", "target", "port", "family"):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(pair)
                if mutation == "missing":
                    changed["client"]["actual_connections"] = changed["client"]["actual_connections"][:2]
                else:
                    for entry in changed["client"]["actual_connections"][2:]:
                        if mutation == "peer":
                            entry["authenticated_peer"] = "wrong-peer"
                        elif mutation == "target":
                            entry["local_addr"] = "/ip4/11.0.0.1/udp/43001/quic-v1"
                        elif mutation == "port":
                            entry["remote_addr"] = "/ip4/11.0.0.2/udp/44001/quic-v1"
                        else:
                            entry["remote_addr"] = "/ip6/2606:4700::2222/udp/44000/quic-v1"
                self.assert_invalid(changed, "concrete counterpart")
        pair = valid_pair(transport="quic")
        for entry in pair["client"]["actual_connections"][:2]:
            entry["local_addr"] = "/ip4/0.0.0.0/udp/41000/quic-v1"
        pair["server"]["actual_connections"] = []
        self.assert_invalid(pair, "concrete counterpart")

    def test_wildcard_is_not_allowed_for_tcp_requested_remote_or_inbound(self):
        for field, inbound in (("remote_addr", False), ("local_addr", True)):
            with self.subTest(field=field, inbound=inbound):
                pair = valid_pair(transport="quic")
                entries = pair["client"]["actual_connections"][2:] if inbound else pair["client"]["actual_connections"][:2]
                for entry in entries:
                    entry[field] = "/ip4/0.0.0.0/udp/43000/quic-v1"
                self.assert_invalid(pair, "public numeric")
        pair = valid_pair(transport="quic")
        pair["expected_requested_address"] = "/ip4/0.0.0.0/udp/43000/quic-v1"
        self.assert_invalid(pair, "public numeric")
        pair = valid_pair()
        for entry in pair["client"]["actual_connections"][:2]:
            entry["local_addr"] = "/ip4/0.0.0.0/tcp/41000"
        self.assert_invalid(pair, "public numeric")

    def test_quic_authenticated_connection_cannot_override_failed_v2_response(self):
        pair = valid_pair("go", "forge", version=2, transport="quic")
        for entry in pair["server"]["actual_connections"][2:]:
            entry["local_addr"] = "/ip4/0.0.0.0/udp/44000/quic-v1"
        pair["client"].update(status="error", error="dial response read failed: EOF",
                              reached=False, response_addr=None, authenticated_peer=None,
                              fresh_inbound_count=0)
        del pair["client"]["fresh_inbound_connections"]
        pair["client_terminal_status"]["exit_code"] = 2
        self.assert_invalid(pair, "independent exit 0")

    def test_go_client_port_reuse_still_requires_fresh_connection(self):
        pair = valid_pair("go", "forge", reuse_client_port=True)
        self.assertEqual(validate_autonat_evidence(**pair), [])
        fresh = pair["client"]["fresh_inbound_connections"][0]
        fresh["connection_id"] = pair["client"]["control_connection_id"]
        self.assert_invalid(pair, "distinct traced")

    def test_independent_go_hosts_can_have_equal_connection_ids(self):
        pair = valid_pair()
        pair["server"]["probe_connections"][0]["connection_id"] = "control-1"
        self.assertEqual(validate_autonat_evidence(**pair), [])

    def test_wrong_service_or_control_identify_peer(self):
        for owner, field in (("server", "local_peer_id"),
                             ("client", "control_authenticated_peer"),
                             ("client", "observer_peer_id")):
            with self.subTest(owner=owner, field=field):
                pair = valid_pair()
                pair[owner][field] = "wrong-peer"
                self.assert_invalid(pair)
        pair = valid_pair("rust", "forge")
        pair["client"]["authenticated_peer"] = "wrong-peer"
        self.assert_invalid(pair, "Identify")

    def test_wrong_dialback_peer_or_address(self):
        for field, value in (("authenticated_peer", "wrong-peer"),
                             ("local_addr", "/ip4/11.0.0.1/tcp/43001"),
                             ("remote_addr", "/ip4/11.0.0.2/tcp/44001")):
            with self.subTest(field=field):
                pair = valid_pair()
                for entry in pair["client"]["actual_connections"][2:]:
                    entry[field] = value
                self.assert_invalid(pair, "paired fresh")
        pair = valid_pair()
        pair["expected_requested_address"] = "/ip4/11.0.0.1/tcp/43001"
        self.assert_invalid(pair, "runner's exact target")

    def test_ordinary_control_connection_is_not_dialback(self):
        for client, server in (("forge", "rust"), ("go", "forge"), ("rust", "forge")):
            with self.subTest(client=client):
                pair = valid_pair(client, server)
                pair["client"]["actual_connections"] = [
                    c for c in pair["client"]["actual_connections"]
                    if c["direction"] == "outbound"
                ]
                if client != "forge":
                    pair["client"]["fresh_inbound_count"] = 0
                    pair["client"]["fresh_inbound_connections"] = []
                self.assert_invalid(pair, "paired fresh")

    def test_forge_go_v1_tls_without_inbound_uses_independent_probe_and_vote(self):
        pair = valid_pair(transport="tcp-tls")
        pair["client"]["actual_connections"] = pair["client"]["actual_connections"][:2]
        self.assertEqual(validate_autonat_evidence(**pair), [])
        for mutation in ("missing_probe", "wrong_peer", "wrong_address", "control_as_probe",
                         "shared_identity", "missing_vote", "cleanup", "incomplete_trace"):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(pair)
                server = changed["server"]
                probe = server["probe_connections"][0]
                if mutation == "missing_probe":
                    server["probe_connections"] = []
                elif mutation == "wrong_peer":
                    probe["authenticated_peer"] = "other-peer"
                elif mutation == "wrong_address":
                    probe["remote_addr"] = "/ip4/11.0.0.1/tcp/1"
                elif mutation == "control_as_probe":
                    server["probe_connections"] = copy.deepcopy(server["actual_connections"])
                elif mutation == "shared_identity":
                    server["probe_peer_id"] = SERVICE
                elif mutation == "missing_vote":
                    changed["client"]["v1_vote"] = None
                elif mutation == "cleanup":
                    changed["server_terminal_status"]["termination"] = "kill"
                else:
                    server["trace_complete"] = False
                self.assert_invalid(changed)
        pair = valid_pair(version=2, transport="tcp-tls")
        pair["client"]["actual_connections"] = pair["client"]["actual_connections"][:2]
        self.assert_invalid(pair, "paired fresh")

    def test_rust_v1_accepts_secured_dialback_with_independent_donor_proof(self):
        for transport in ("tcp", "tcp-tls", "tcp-pnet", "quic"):
            with self.subTest(transport=transport):
                pair = valid_pair("forge", "rust", transport=transport)
                pair["client"]["actual_connections"] = [
                    c for c in pair["client"]["actual_connections"]
                    if not (c["direction"] == "inbound" and c["stage"] == "upgraded")
                ]
                self.assertEqual(validate_autonat_evidence(**pair), [])
                for mutation in ("missing", "peer", "address", "probe", "control", "cleanup"):
                    changed = copy.deepcopy(pair)
                    inbound = changed["client"]["actual_connections"][-1]
                    if mutation == "missing":
                        changed["client"]["actual_connections"].pop()
                    elif mutation == "peer":
                        inbound["authenticated_peer"] = "wrong-peer"
                    elif mutation == "address":
                        inbound["local_addr"] = address("11.0.0.1", 44001, transport)
                    elif mutation == "probe":
                        changed["server"]["donor_events"][-1]["probe_id"] = "wrong-probe"
                    elif mutation == "control":
                        changed["server"]["actual_connections"][-1]["connection_id"] = "control-1"
                    else:
                        changed["client"]["hosts_closed"] = False
                    self.assertTrue(validate_autonat_evidence(**changed), mutation)

    def test_rust_v2_secured_only_cannot_replace_upgrade(self):
        pair = valid_pair("forge", "rust", version=2)
        pair["client"]["actual_connections"] = [
            c for c in pair["client"]["actual_connections"]
            if not (c["direction"] == "inbound" and c["stage"] == "upgraded")
        ]
        self.assertTrue(validate_autonat_evidence(**pair))

    def test_rust_requires_actual_typed_events_not_debug_or_counters(self):
        for version in (1, 2):
            for side in ("client", "server"):
                for events in ([], [{"protocol": PROTOCOLS[version], "event": "Response OK"}]):
                    with self.subTest(version=version, side=side, events=events):
                        pair = valid_pair("rust" if side == "client" else "forge",
                                          "rust" if side == "server" else "forge", version)
                        pair[side]["donor_events"] = events
                        self.assert_invalid(pair)

    def test_rust_v1_typed_correlation_rejects_peer_probe_address_order_and_failure(self):
        for side in ("client", "server"):
            for field, value in (("peer", "wrong-peer"), ("probe_id", "ProbeId(8)"),
                                 ("address", "/ip4/11.0.0.1/tcp/1"), ("success", False)):
                with self.subTest(side=side, field=field):
                    pair = valid_pair("rust" if side == "client" else "forge",
                                      "rust" if side == "server" else "forge")
                    pair[side]["donor_events"][1][field] = value
                    self.assert_invalid(pair)
        pair = valid_pair("rust", "forge")
        pair["client"]["donor_events"].reverse()
        self.assert_invalid(pair, "correlation")
        pair = valid_pair("rust", "forge")
        pair["client"]["v1_request_events"] = 2
        self.assert_invalid(pair, "counters")
        pair = valid_pair("forge", "rust")
        pair["server"]["donor_events"][0]["addresses"] = []
        self.assert_invalid(pair, "Request lacks")

    def test_rust_v1_only_uncorrelated_preliminary_typed_no_server_is_neutral(self):
        pair = valid_pair("rust", "forge")
        preliminary = {
            "protocol": PROTOCOLS[1], "kind": "v1_outbound_error",
            "probe_id": "ProbeId(6)", "peer": None, "success": False,
            "error_kind": "NoServer", "event": "diagnostic only",
        }
        pair["client"]["donor_events"].insert(0, preliminary)
        self.assertEqual(validate_autonat_evidence(**pair), [])
        for field, value in (("probe_id", "ProbeId(7)"), ("peer", SERVICE),
                             ("error_kind", "NoAddresses"), ("error_kind", None),
                             ("success", True)):
            with self.subTest(field=field):
                changed = copy.deepcopy(pair)
                changed["client"]["donor_events"][0][field] = value
                self.assert_invalid(changed)
        for position in (1, 2):
            with self.subTest(position=position):
                changed = valid_pair("rust", "forge")
                changed["client"]["donor_events"].insert(position, copy.deepcopy(preliminary))
                self.assert_invalid(changed)
        pair["client"]["donor_events"] = [preliminary]
        self.assert_invalid(pair, "Request/Response pair")

    def test_rust_v2_server_prior_success_may_test_another_address(self):
        pair = valid_pair("forge", "rust", version=2)
        history = copy.deepcopy(pair["server"]["donor_events"][0])
        history["tested_addr"] = "/ip4/11.0.0.1/tcp/41000"
        history["data_amount"] = 0
        pair["server"]["donor_events"].insert(0, history)
        self.assertEqual(validate_autonat_evidence(**pair), [])
        for field, value in (("success", False), ("client", "wrong-peer"),
                             ("tested_addr", "/dns4/localhost/tcp/1"),
                             ("data_amount", 10000)):
            with self.subTest(field=field):
                changed = copy.deepcopy(pair)
                changed["server"]["donor_events"][0][field] = value
                self.assert_invalid(changed)
        changed = copy.deepcopy(pair)
        changed["server"]["donor_events"].reverse()
        self.assert_invalid(changed, "correlation")
        pair["server"]["v2_result_events"] = 2  # This counter counts client events only.
        self.assert_invalid(pair, "counters")

    def test_rust_v2_requires_matching_successful_typed_event_and_count(self):
        for side in ("client", "server"):
            for field, value in (("tested_addr", "/ip4/11.0.0.3/tcp/1"),
                                 ("server" if side == "client" else "client", "wrong-peer"),
                                 ("success", False), ("protocol", PROTOCOLS[1])):
                with self.subTest(side=side, field=field):
                    pair = valid_pair("rust" if side == "client" else "forge",
                                      "rust" if side == "server" else "forge", version=2)
                    pair[side]["donor_events"][0][field] = value
                    self.assert_invalid(pair)
        pair = valid_pair("rust", "forge", version=2)
        pair["client"]["donor_events"].append(copy.deepcopy(pair["client"]["donor_events"][0]))
        self.assert_invalid(pair, "counters")

    def test_rust_v2_dial_data_is_zero_or_protocol_range_and_matches_event(self):
        for amount in (30000, 100000):
            with self.subTest(valid=amount):
                pair = valid_pair("rust", "forge", version=2)
                pair["client"]["dial_data_bytes"] = amount
                pair["client"]["donor_events"][0]["bytes_sent"] = amount
                self.assertEqual(validate_autonat_evidence(**pair), [])
        for amount in (-1, True, 1, 10000, 29999, 100001, None):
            with self.subTest(invalid=amount):
                pair = valid_pair("rust", "forge", version=2)
                pair["client"]["dial_data_bytes"] = amount
                pair["client"]["donor_events"][0]["bytes_sent"] = amount
                self.assert_invalid(pair, "dial-data")
        pair = valid_pair("rust", "forge", version=2)
        pair["client"]["donor_events"][0]["bytes_sent"] = 100000
        self.assert_invalid(pair, "dial-data correlation")

    def test_rust_v2_cross_ip_requires_payment_for_both_roles(self):
        for client, server in (("rust", "forge"), ("forge", "rust")):
            with self.subTest(client=client, server=server):
                pair = valid_pair(client, server, version=2)
                result = pair["client" if client == "rust" else "server"]
                result["donor_events"][0]["bytes_sent" if client == "rust" else "data_amount"] = 0
                if client == "rust":
                    result["dial_data_bytes"] = 0
                self.assert_invalid(pair, "payment")

    def test_rust_v2_same_control_address_may_omit_payment(self):
        for client, server in (("rust", "forge"), ("forge", "rust")):
            with self.subTest(client=client, server=server):
                pair = valid_pair(client, server, version=2, reuse_client_port=True)
                result = pair["client" if client == "rust" else "server"]
                result["donor_events"][0]["bytes_sent" if client == "rust" else "data_amount"] = 0
                if client == "rust":
                    result["dial_data_bytes"] = 0
                self.assertEqual(validate_autonat_evidence(**pair), [])

    def test_rust_v2_service_checks_prior_same_ip_different_port_payment(self):
        pair = valid_pair("forge", "rust", version=2)
        history = copy.deepcopy(pair["server"]["donor_events"][0])
        history.update(tested_addr="/ip4/11.0.0.1/tcp/43001", data_amount=0)
        pair["server"]["donor_events"].insert(0, history)
        self.assert_invalid(pair, "payment")
        history["data_amount"] = 30000
        self.assertEqual(validate_autonat_evidence(**pair), [])

    def test_wrong_direction_or_missing_authentication(self):
        for owner, field in (("client", "actual_connections"),
                             ("server", "probe_connections")):
            for key, value in (("direction", "inbound" if owner == "server" else "outbound"),
                               ("authenticated", False)):
                with self.subTest(owner=owner, key=key):
                    pair = valid_pair()
                    for entry in pair[owner][field]:
                        if owner == "server" or entry["direction"] == "inbound":
                            entry[key] = value
                    self.assert_invalid(pair)

    def test_relabeling_reverse_control_socket_does_not_prove_freshness(self):
        pair = valid_pair("forge", "forge", reuse_client_port=True)
        control_remote = pair["client"]["actual_connections"][0]["remote_addr"]
        for entry in pair["client"]["actual_connections"][2:]:
            entry["remote_addr"] = control_remote
        for entry in pair["server"]["actual_connections"][2:]:
            entry["local_addr"] = control_remote
        self.assert_invalid(pair, "paired fresh")

    def test_forge_upgrade_requires_matching_secured_evidence(self):
        pair = valid_pair()
        del pair["client"]["actual_connections"][2]
        self.assert_invalid(pair, "matching authentication")

    def test_missing_counterpart_and_required_data(self):
        pair = valid_pair()
        pair["server"] = None
        self.assert_invalid(pair, "missing")
        for owner, field in (("client", "control_identify_completed"),
                             ("client", "actual_connections"),
                             ("server", "probe_connections"),
                             ("client", "diagnostics_after_stop")):
            with self.subTest(owner=owner, field=field):
                pair = valid_pair()
                del pair[owner][field]
                self.assert_invalid(pair)

    def test_v2_cross_ip_go_service_requires_completed_dial_data(self):
        for change in ("missing", "unpaid", "wrong_address", "failed"):
            with self.subTest(change=change):
                pair = valid_pair(version=2)
                entry = pair["server"]["service_completed_requests"][0]
                if change == "missing":
                    pair["server"]["service_completed_requests"] = []
                elif change == "unpaid":
                    entry["dial_data_required"] = False
                elif change == "wrong_address":
                    entry["dialed_addr"] = "/ip4/11.0.0.3/tcp/1"
                else:
                    entry["dial_status"] = "E_DIAL_ERROR"
                self.assert_invalid(pair, "completed request")

    def test_trace_overflow_failure_and_unjoined_owners(self):
        mutations = (
            ("forge", "connection_gater_trace_complete", False),
            ("forge", "connection_gater_trace_failure", "overflow"),
            ("go", "trace_complete", False),
            ("go", "handler_join_error", "failed"),
            ("rust", "trace_overflow_error", "overflow"),
            ("rust", "joined_swarm_executor", False),
            ("rust", "connection_drain", False),
            ("rust", "donor_events", [{}] * 65),
            ("rust", "donor_events", [{"event": "x" * 8193}]),
        )
        for impl, key, value in mutations:
            with self.subTest(impl=impl, key=key):
                pair = valid_pair(impl, "go" if impl == "forge" else "forge")
                pair["client"][key] = value
                self.assert_invalid(pair)
        for client in ("forge", "go", "rust"):
            with self.subTest(client=client):
                pair = valid_pair(client, "go" if client == "forge" else "forge")
                trace = pair["client"]["actual_connections"]
                pair["client"]["actual_connections"] = trace * 129
                self.assert_invalid(pair, "trace bound")

    def test_cleanup_failure_even_when_exit_is_zero(self):
        for owner in ("client", "server"):
            for terminal in (None, {"exit_code": 0, "termination": "terminate"},
                             {"exit_code": 0, "termination": "kill"},
                             {"exit_code": 1, "termination": "graceful"}):
                with self.subTest(owner=owner, terminal=terminal):
                    pair = valid_pair()
                    pair[f"{owner}_terminal_status"] = terminal
                    self.assert_invalid(pair, "independent exit 0")
            pair = valid_pair()
            pair[owner]["cleanup_errors"] = ["close failed"]
            self.assert_invalid(pair, "cleanup errors")
        pair = valid_pair()
        after = json.loads(pair["client"]["diagnostics_after_stop"])
        after["resources"]["active_service_scopes"] = 1
        pair["client"]["diagnostics_after_stop"] = json.dumps(after)
        self.assert_invalid(pair, "resource cleanup")

    def test_version_specific_results_cannot_substitute_for_each_other(self):
        pair = valid_pair()
        pair["client"].update(v1_vote=None, autonat_v2_address_state={
            "address": pair["expected_requested_address"], "state": "public",
        })
        self.assert_invalid(pair, "conflated")
        pair = valid_pair(version=2)
        pair["client"].update(v1_vote="public", autonat_v2_address_state=None)
        self.assert_invalid(pair, "exact verified address")
        for impl in ("go", "rust"):
            with self.subTest(impl=impl):
                pair = valid_pair(impl, "forge", version=2)
                pair["client"]["donor_api_basis"] = valid_pair(impl, "forge")["client"]["donor_api_basis"]
                self.assert_invalid(pair, "version-specific")
        pair = valid_pair("rust", "forge", version=2)
        pair["client"].update(v1_request_events=1, v2_result_events=0)
        self.assert_invalid(pair, "per-version events")

    def test_rust_hidden_fields_are_not_fabricated_proof(self):
        pair = valid_pair("rust", "forge", version=2)
        self.assertIsNone(pair["client"]["actual_connections"][0]["local_addr"])
        self.assertIsNone(pair["client"]["autonat_probe_api_calls"])
        self.assertEqual(validate_autonat_evidence(**pair), [])
        for field, value in (("nonce_proof", "raw_nonce_verified"),
                             ("autonat_probe_api_calls", 1),
                             ("dial_data_bytes", None),
                             ("candidate_reported", False),
                             ("observer_reported_control_addr", None)):
            with self.subTest(field=field):
                changed = copy.deepcopy(pair)
                changed["client"][field] = value
                self.assert_invalid(changed)
        pair["client"]["fresh_inbound_connections"][0]["closed"] = False
        self.assert_invalid(pair, "terminal Rust")

    def test_negative_v1_typed_vote_and_real_elapsed(self):
        pair = valid_pair(negative=True)
        self.assertEqual(validate_autonat_evidence(**pair), [])
        for elapsed in (0, 13999, True, "15005"):
            with self.subTest(elapsed=elapsed):
                changed = copy.deepcopy(pair)
                changed["client"]["probe_elapsed_ms"] = elapsed
                self.assert_invalid(changed, "elapsed >= 14")
        pair["client"]["probe_elapsed_ms"] = 14000
        self.assertEqual(validate_autonat_evidence(**pair), [])

    def test_timeout_and_policy_denial_are_not_negative_votes(self):
        for changes in ({"probe_state": "unknown", "v1_vote": None},
                        {"status": "error", "error": "probe_timeout"},
                        {"error": "connect_timeout"}, {"policy_denied": True},
                        {"internet_egress": "deny"}):
            with self.subTest(changes=changes):
                pair = valid_pair(negative=True)
                pair["client"].update(changes)
                self.assert_invalid(pair)
        pair = valid_pair(negative=True)
        pair["expected_outcome"] = "reachable"
        self.assert_invalid(pair, "positive result")

    def test_negative_rejects_any_positive_or_wrong_source(self):
        for field, value in (("reached", True), ("v1_vote", "public"),
                             ("effective_state", "public"),
                             ("response_addr", "/ip4/11.0.0.1/tcp/9")):
            with self.subTest(field=field):
                pair = valid_pair(negative=True)
                pair["client"][field] = value
                self.assert_invalid(pair)
        pair = valid_pair(negative=True)
        pair["server"]["probe_connections"] = valid_pair()["server"]["probe_connections"]
        self.assert_invalid(pair, "contradicts")
        pair = valid_pair(negative=True)
        pair["client"]["requested_addr"] = "/ip4/11.0.0.3/tcp/9"
        pair["expected_requested_address"] = pair["client"]["requested_addr"]
        self.assert_invalid(pair, "actual control source IP")
        pair = valid_pair(version=2, negative=True)
        self.assert_invalid(pair, "only for Forge v1")

    def test_private_deny_and_loopback_are_not_reachability_proof(self):
        for owner in ("client", "server"):
            pair = valid_pair(transport="tcp-pnet")
            pair[owner]["internet_egress"] = "deny"
            self.assert_invalid(pair, "policy denial")
        for target in ("/ip4/127.0.0.1/tcp/9", "/ip4/10.0.0.1/tcp/9",
                       "/ip6/::1/tcp/9", "/dns4/localhost/tcp/9",
                       "/ip4/11.0.0.1/tcp/0"):
            with self.subTest(target=target):
                pair = valid_pair()
                pair["expected_requested_address"] = target
                self.assert_invalid(pair, "public numeric")

    def test_malformed_inputs_return_errors_not_exceptions(self):
        for bad in (None, [], "ok", 0, {}):
            with self.subTest(bad=bad):
                pair = valid_pair()
                pair["client"] = bad
                self.assert_invalid(pair)
        for bad in (None, {}, [None], ["ok"]):
            with self.subTest(trace=bad):
                pair = valid_pair()
                pair["client"]["actual_connections"] = bad
                self.assert_invalid(pair)
        pair = valid_pair(version=2)
        pair["client"]["autonat_v2_address_state"] = '{"state":"private","state":"public"}'
        self.assert_invalid(pair, "exact verified address")
        pair = valid_pair()
        pair["version"] = True
        self.assert_invalid(pair, "unsupported")


if __name__ == "__main__":
    unittest.main()
