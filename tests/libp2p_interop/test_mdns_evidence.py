"""Synthetic receipt validation only, never a claim of live discovery."""

import copy
import hashlib
import json
import unittest

from mdns_evidence import BINDINGS, PROTOCOL, SCHEMA, decode_receipt, validate_mdns_evidence


def pair(client_impl="forge", server_impl="go", transport="tcp", family=4):
    payload = b"unique-mdns-test-challenge"
    fingerprint = "45fc986bbc9388a11d939df26f730f0c" if transport == "tcp-pnet" else None
    service = f"_p2p-{fingerprint}._udp.local" if fingerprint else "_p2p._udp.local"
    ips = ["10.231.77.1", "10.231.77.2"] if family == 4 else ["fd00:666f:7267::1", "fd00:666f:7267::2"]
    peers = ["clientPeer", "serverPeer"]
    def addr(index, port):
        return f"/ip{family}/{ips[index]}/" + (
            f"udp/{port}/quic-v1" if transport == "quic" else f"tcp/{port}")
    ready, results = [], []
    for index, impl in enumerate((client_impl, server_impl)):
        role = "dialer" if index == 0 else "listener"
        other = 1 - index
        local = addr(index, 45000 if index == 0 else 4001)
        remote = addr(other, 4001 if index == 0 else 45000)
        security = "/tls/1.0.0" if transport in ("tcp-tls", "tcp-pnet") else "/noise"
        muxer = None if impl == "forge" else "/yamux/1.0.0"
        if transport == "quic":
            security = {"forge": "quic-tls", "go": "", "rust": None}[impl]
            muxer = "" if impl == "go" else None
            if impl == "rust" and index == 0:
                local = None
        discovery = {"source": "mdns", "peer_id": peers[other], "addresses": [addr(other, 4001)]}
        if impl == "forge":
            discovery = {"source": "mdns", "basis": "runtime_observation_count",
                         "runtime_observation_count": 1, "observations_before_outbound_dial": 1 - index}
        identifier = f"connection-{index}"
        result = {
            "schema": SCHEMA, "implementation": impl, "role": role, "status": "ok",
            "local_peer_id": peers[index], "service_name": service, "discovery": discovery,
            "connection": {"id": identifier, "local_peer_id": peers[index], "remote_peer_id": peers[other],
                           "direction": "outbound" if index == 0 else "inbound", "local_address": local,
                           "remote_address": remote, "transport": "/quic-v1" if transport == "quic" else "tcp",
                           "security": security, "muxer": muxer, "binding": BINDINGS[impl]},
            "echo": {"protocol": PROTOCOL, "bytes": len(payload), "sha256": hashlib.sha256(payload).hexdigest(),
                     "connection_id": identifier, "remote_peer_id": peers[other]},
        }
        if impl == "forge":
            result["echo"]["stream_id"] = 4
        if impl == "go":
            result["echo"]["stream_id"] = "stream-1"
            result["echo_handlers_joined"] = True
        if impl == "rust":
            result["fixture_task_lifecycle"] = {
                "scope": "public_swarm_executor_and_fixture_echo_handler",
                "shutdown_mode": "close_admission_abort_join_after_swarm_drop",
                "fixture_owned_tasks_joined": True, "overflow": False, "errors": [], "tasks": [],
            }
            result["upgrade_observation"] = {
                "finalized_after_swarm_drop": True, "fixture_owned_tasks_joined": True, "overflow": False,
            }
        results.append(result)
        ready.append({"schema": SCHEMA, "role": role, "status": "ready",
                      "local_peer_id": peers[index], "service_name": service})
    network = {
        "schema": "forge.mdns.network.v1", "kind": "linux_isolated_mdns_netns", "family": family,
        "state": "closed", "outer_namespace": "mdns-o-unit", "cleanup_failures": [], "cleanup_uncertainty": [],
        "outer_network": {"bridge": "br0", "default_route": "absent", "external_links": "absent"},
        "participants": [{"role": role, "namespace": f"mdns-{role}-unit", "addresses": [ips[i]]}
                         for i, role in enumerate(("client", "server"))],
        "interfaces": {role: {"name": "eth0", "index": 2, "up": True, "multicast": True, "address": ips[i]}
                       for i, role in enumerate(("client", "server"))},
        "multicast": {"group": "224.0.0.251" if family == 4 else "ff02::fb", "port": 5353,
                      "bridge_snooping": "disabled_by_checked_command"},
    }
    return {"client": results[0], "server": results[1], "client_impl": client_impl, "server_impl": server_impl,
            "transport": transport, "family": family, "client_ready": ready[0], "server_ready": ready[1],
            "client_terminal_status": {"exit_code": 0, "termination": "graceful"},
            "server_terminal_status": {"exit_code": 0, "termination": "graceful"},
            "network": network, "payload": payload, "private_fingerprint": fingerprint}


class MdnsEvidenceTests(unittest.TestCase):
    def test_all_28_positive_contract_combinations(self):
        count = 0
        for donor in ("go", "rust"):
            for client, server in (("forge", donor), (donor, "forge")):
                for transport in ("tcp", "tcp-tls", "quic") + (("tcp-pnet",) if donor == "go" else ()):
                    for family in (4, 6):
                        with self.subTest(client=client, server=server, transport=transport, family=family):
                            self.assertEqual(validate_mdns_evidence(**pair(client, server, transport, family)), [])
                            count += 1
        self.assertEqual(count, 28)

    def test_private_tcp_requires_actual_tls_in_both_directions(self):
        for client, server in (("forge", "go"), ("go", "forge")):
            with self.subTest(client=client, server=server):
                data = pair(client, server, "tcp-pnet")
                self.assertEqual(data["client"]["connection"]["security"], "/tls/1.0.0")
                self.assertEqual(data["server"]["connection"]["security"], "/tls/1.0.0")
                self.assertEqual(validate_mdns_evidence(**data), [])
                for roles in (("client",), ("server",), ("client", "server")):
                    changed = copy.deepcopy(data)
                    for role in roles:
                        changed[role]["connection"]["security"] = "/noise"
                    self.assertIn("actual TCP security mismatch", validate_mdns_evidence(**changed))

    def test_donor_dialer_must_discover_actual_endpoint_not_just_peer_and_ip(self):
        for donor in ("go", "rust"):
            for transport in ("tcp", "quic"):
                for family in (4, 6):
                    with self.subTest(donor=donor, transport=transport, family=family):
                        data = pair(donor, "forge", transport, family)
                        self.assertEqual(validate_mdns_evidence(**data), [])
                        addresses = data["client"]["discovery"]["addresses"]
                        original = addresses[0]
                        addresses[0] = original.replace("/4001", "/4999")
                        self.assertIn("donor dial endpoint is absent from its mDNS discovery addresses",
                                      validate_mdns_evidence(**data))
                        # It is a set-membership requirement, not first-address equality.
                        addresses.append(original)
                        self.assertEqual(validate_mdns_evidence(**data), [])

    def test_donor_listener_discovery_need_not_match_inbound_ephemeral_port(self):
        for donor in ("go", "rust"):
            for transport in ("tcp", "quic"):
                with self.subTest(donor=donor, transport=transport):
                    data = pair("forge", donor, transport)
                    self.assertIn("/4001", data["server"]["discovery"]["addresses"][0])
                    self.assertIn("/45000", data["server"]["connection"]["remote_address"])
                    self.assertEqual(validate_mdns_evidence(**data), [])

    def test_isolated_forge_counts_are_exactly_one_with_role_specific_predial(self):
        for client, server, role in (("forge", "go", "client"), ("go", "forge", "server")):
            with self.subTest(role=role):
                data = pair(client, server)
                self.assertEqual(validate_mdns_evidence(**data), [])
                for field in ("runtime_observation_count", "observations_before_outbound_dial"):
                    changed = copy.deepcopy(data)
                    changed[role]["discovery"][field] = 2
                    self.assertIn("invalid " + ("mDNS observation count" if field == "runtime_observation_count"
                                               else "pre-dial observation count"),
                                  validate_mdns_evidence(**changed))

    def test_status_ok_rejects_any_nonnull_failure_including_empty_object(self):
        data = pair()
        self.assertEqual(validate_mdns_evidence(**data), [])
        data["client"]["failure"] = None
        self.assertEqual(validate_mdns_evidence(**data), [])
        for role in ("client", "server"):
            for failure in ({}, {"code": "timeout"}, [], "", False, 0):
                with self.subTest(role=role, failure=failure):
                    changed = copy.deepcopy(data)
                    changed[role]["failure"] = failure
                    self.assertIn("result reports failure", validate_mdns_evidence(**changed))

    def test_cross_receipt_mutations_fail_closed(self):
        changes = [
            (("client", "schema"), "wrong"), (("server", "status"), "ready"),
            (("client", "connection", "id"), "other-connection"),
            (("client", "connection", "remote_peer_id"), "stranger"),
            (("server", "connection", "direction"), "outbound"),
            (("client", "connection", "transport"), "/quic-v1"),
            (("client", "connection", "security"), "/tls/1.0.0"),
            (("client", "connection", "local_address"), "/ip4/10.231.77.1/tcp/45001"),
            (("client", "connection", "binding"), "requested_transport"),
            (("server", "connection", "remote_address"), "/ip4/10.231.77.99/tcp/45000"),
            (("client", "echo", "remote_peer_id"), "stranger"),
            (("client", "echo", "bytes"), True), (("server", "echo", "sha256"), "0" * 64),
            (("server", "echo", "protocol"), "/wrong"), (("server", "echo_handlers_joined"), False),
            (("client", "discovery", "runtime_observation_count"), 0),
            (("client", "discovery", "runtime_observation_count"), True),
            (("client", "discovery", "observations_before_outbound_dial"), 0),
            (("client", "discovery", "peer_id"), "serverPeer"),
            (("server", "discovery", "peer_id"), "otherPeer"),
            (("server", "discovery", "addresses"), []),
            (("server", "discovery", "addresses"), ["/dns4/example/tcp/4001"]),
            (("server", "discovery", "addresses"), ["/ip6zone/2/ip4/10.231.77.1/tcp/4001"]),
            (("server", "discovery", "addresses"), ["/ip4/10.231.77.1/tcp/4001/p2p-circuit"]),
            (("client_terminal_status", "exit_code"), False),
            (("server_terminal_status", "termination"), "terminated"),
            (("server_ready", "local_peer_id"), "otherPeer"),
            (("client", "service_name"), "_wrong._udp.local"),
            (("network", "state"), "ready"), (("network", "cleanup_failures"), ["busy"]),
            (("network", "outer_network", "default_route"), "present"),
            (("network", "interfaces", "client", "multicast"), False),
        ]
        for path, replacement in changes:
            with self.subTest(path=path, replacement=replacement):
                data = pair()
                target = data
                for component in path[:-1]:
                    target = target[component]
                target[path[-1]] = replacement
                self.assertTrue(validate_mdns_evidence(**data))

    def test_rust_requires_finalized_joins_but_does_not_invent_quic_source(self):
        data = pair("rust", "forge", "quic", 6)
        self.assertIsNone(data["client"]["connection"]["local_address"])
        self.assertEqual(validate_mdns_evidence(**data), [])
        for owner, field in (("fixture_task_lifecycle", "fixture_owned_tasks_joined"),
                             ("upgrade_observation", "finalized_after_swarm_drop")):
            changed = copy.deepcopy(data)
            changed["client"][owner][field] = False
            self.assertTrue(validate_mdns_evidence(**changed))
        data["server"]["connection"]["local_address"] = None
        self.assertTrue(validate_mdns_evidence(**data))

    def test_quic_wildcard_requires_counterpart_same_port(self):
        data = pair(transport="quic")
        data["client"]["connection"]["local_address"] = "/ip4/0.0.0.0/udp/45000/quic-v1"
        self.assertEqual(validate_mdns_evidence(**data), [])
        data["client"]["connection"]["local_address"] = "/ip4/0.0.0.0/udp/45001/quic-v1"
        self.assertTrue(validate_mdns_evidence(**data))

    def test_private_namespace_never_uses_operational_hash_or_rust(self):
        for fingerprint in (None, "0" * 64, "A" * 32):
            data = pair(transport="tcp-pnet")
            data["private_fingerprint"] = fingerprint
            self.assertTrue(validate_mdns_evidence(**data))
        self.assertTrue(validate_mdns_evidence(**pair("rust", "forge", "tcp-pnet")))

    def test_third_participant_and_duplicate_namespace_rejected(self):
        data = pair()
        data["network"]["participants"].append(copy.deepcopy(data["network"]["participants"][0]))
        self.assertTrue(validate_mdns_evidence(**data))
        data = pair()
        data["network"]["participants"][1]["namespace"] = data["network"]["participants"][0]["namespace"]
        self.assertTrue(validate_mdns_evidence(**data))

    def test_json_is_bounded_and_rejects_duplicate_keys_and_nonfinite_numbers(self):
        self.assertEqual(decode_receipt(json.dumps(pair()["client"]))["status"], "ok")
        for raw in ('{"status":"ok","status":"error"}', '{"n":NaN}', '[]', '{', ' ' * (1024 * 1024 + 1)):
            with self.subTest(raw=raw[:40]), self.assertRaises(ValueError):
                decode_receipt(raw)


if __name__ == "__main__":
    unittest.main()
