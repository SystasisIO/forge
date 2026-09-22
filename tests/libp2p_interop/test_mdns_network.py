"""Fake-command checks only; these tests do not claim multicast interoperability."""

import json
import ipaddress
import unittest

from isolated_network import CommandResult, NetworkError
from mdns_network import IsolatedMdnsNetwork
from test_autonat_network import FakeCommandRunner


class MdnsCommands(FakeCommandRunner):
    def __init__(self):
        super().__init__()
        self.multicast = True
        self.tentative = False

    def __call__(self, command):
        if command[-5:] == ["-j", "addr", "show", "dev", "eth0"]:
            self.calls.append(list(command))
            network = self.network
            role = next(r for r in ("client", "server") if network.namespaces[r] == command[3])
            return CommandResult(0, json.dumps([{
                "ifname": "eth0", "ifindex": 2,
                "flags": ["UP", "MULTICAST"] if self.multicast else ["UP"],
                "addr_info": [{"local": network.addresses[role][0],
                               "family": "inet" if network.family == 4 else "inet6",
                               "prefixlen": 24 if network.family == 4 else 64,
                               "tentative": self.tentative}],
            }]))
        return super().__call__(command)


class MdnsNetworkTests(unittest.TestCase):
    def test_ipv4_addresses_are_rfc1918_not_documentation_or_reserved(self):
        network, _ = self.network()
        self.assertEqual(network.addresses, {"client": ("10.231.77.1",), "server": ("10.231.77.2",)})
        documentation = tuple(ipaddress.ip_network(value) for value in (
            "192.0.2.0/24", "198.51.100.0/24", "203.0.113.0/24"))
        for values in network.addresses.values():
            address = ipaddress.ip_address(values[0])
            self.assertIn(address, ipaddress.ip_network("10.0.0.0/8"))
            self.assertFalse(address.is_reserved or address.is_loopback or address.is_multicast)
            self.assertFalse(any(address in subnet for subnet in documentation))

    def network(self, family=4, **kwargs):
        commands = MdnsCommands()
        network = IsolatedMdnsNetwork(family, command_runner=commands, system=lambda: "Linux",
                                      ip_lookup=lambda name: name, outer_namespace_isolated=lambda: True,
                                      namespace_token="unit", **kwargs)
        commands.network = network
        return network, commands

    def test_both_families_have_exactly_two_isolated_multicast_participants(self):
        for family in (4, 6):
            with self.subTest(family=family):
                network, commands = self.network(family)
                network.setup()
                evidence = network.evidence()
                self.assertEqual(evidence["state"], "ready")
                self.assertEqual(evidence["schema"], "forge.mdns.network.v1")
                self.assertEqual(len(evidence["participants"]), 2)
                self.assertEqual(set(evidence["interfaces"]), {"client", "server"})
                self.assertEqual(evidence["outer_network"], {
                    "bridge": "br0", "default_route": "absent", "external_links": "absent"})
                self.assertIn(["ip", "netns", "exec", "mdns-o-unit", "ip", "link", "set",
                               "dev", "br0", "type", "bridge", "mcast_snooping", "0"], commands.calls)
                self.assertFalse(any("default" in c and "add" in c for c in commands.calls))
                if family == 6:
                    self.assertEqual(sum("nodad" in c for c in commands.calls), 2)
                    self.assertEqual(sum("net.ipv6.conf.eth0.disable_ipv6=0" in c for c in commands.calls), 2)
                self.assertEqual(network.close(), [])
                self.assertEqual(network.evidence()["state"], "closed")

    def test_unusable_interface_fails_closed_and_cleans_children(self):
        for field in ("multicast", "tentative"):
            network, commands = self.network(6)
            setattr(commands, field, field == "tentative")
            with self.subTest(field=field), self.assertRaises(NetworkError):
                network.setup()
            self.assertFalse(commands.existing_namespaces)

    def test_no_host_fallback_or_third_participant(self):
        network, commands = self.network()
        commands.extra_links[network.namespaces["outer"]] = {"external0"}
        with self.assertRaises(NetworkError):
            network.setup()
        self.assertFalse(commands.existing_namespaces)

    def test_invalid_family_is_rejected_without_commands(self):
        for family in (True, "6", 0, 5):
            with self.subTest(family=family), self.assertRaises(ValueError):
                self.network(family)


if __name__ == "__main__":
    unittest.main()
