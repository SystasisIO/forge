import json
import unittest

from isolated_network import CommandResult, NetworkError
from path_network import PathNetwork


class Commands:
    def __init__(self):
        self.calls, self.names, self.links, self.routes = [], set(), {}, {}
        self.fail = None

    def __call__(self, args):
        self.calls.append(args)
        if self.fail and self.fail in args:
            return CommandResult(1, stderr="Operation not permitted")
        if args[:3] == ["ip", "netns", "list"]:
            return CommandResult(0, "\n".join(self.names))
        if args[:3] == ["ip", "netns", "add"]:
            self.names.add(args[-1]); self.links[args[-1]] = {"lo"}
        if args[:3] == ["ip", "netns", "del"]:
            self.names.remove(args[-1])
        if args[:3] == ["ip", "netns", "pids"]:
            return CommandResult(0)
        ns, command = (args[3], args[4:]) if args[:3] == ["ip", "netns", "exec"] else ("caller", args)
        if command[-3:] == ["-j", "link", "show"]:
            return CommandResult(0, json.dumps([{"ifname": n} for n in self.links.get(ns, {"lo"})]))
        if command[-4:] == ["route", "show", "default"] or command[-3:] == ["route", "show", "default"]:
            return CommandResult(0, json.dumps([] if "-6" in command else self.routes.get(ns, [])))
        if "link" in command and "add" in command:
            name = command[command.index("add") + 1]
            self.links[ns].add(name)
            if "netns" in command:
                self.links[command[-1]].add("eth0")
        if "route" in command and "add" in command and "default" in command:
            self.routes[ns] = [{"gateway": command[command.index("via") + 1], "dev": "eth0"}]
        if "iptables-save" in command:
            return CommandResult(0, ":FORWARD DROP [0:0]\n")
        return CommandResult(0)


class PathNetworkTests(unittest.TestCase):
    def make(self, **options):
        commands = Commands()
        network = PathNetwork(command_runner=commands, system=lambda: "Linux", ip_lookup=lambda name: name,
                              outer_namespace_isolated=lambda: True, namespace_token="unit", **options)
        return network, commands

    def test_owned_snat_rules_never_open_unilateral_inbound(self):
        network, commands = self.make()
        network.setup()
        nat = [c for c in commands.calls if "SNAT" in c]
        self.assertEqual(len(nat), 2)
        self.assertEqual({c[-1] for c in nat}, {"11.0.0.2", "11.0.0.3"})
        for c in commands.calls:
            if "FORWARD" in c and "ACCEPT" in c and "-i" in c and c[c.index("-i") + 1] == "eth0":
                self.assertIn("ESTABLISHED,RELATED", c)
        self.assertFalse(any("DNAT" in c for c in commands.calls))
        self.assertEqual(network.close(), [])
        self.assertEqual(network.evidence()["state"], "closed")
        self.assertFalse(commands.names)

    def test_partial_setup_cleanup_reuses_namespace_owner(self):
        network, commands = self.make()
        commands.fail = "iptables"
        with self.assertRaises(NetworkError):
            network.setup()
        self.assertFalse(commands.names)
        self.assertEqual(network.evidence()["state"], "closed")

    def test_failure_blocks_peer_udp_not_relay_and_never_releases_rule(self):
        network, commands = self.make()
        network.setup()
        network.block_peer_udp()
        faults = [c for c in commands.calls if "-I" in c]
        self.assertEqual({c[c.index("-s") + 1] for c in faults}, {"11.0.0.2", "11.0.0.3"})
        self.assertTrue(all(c[-1] == "DROP" for c in faults))
        self.assertFalse(any("-D" in c for c in commands.calls))
        network.close()

    def test_no_host_or_emulated_fallback(self):
        calls = Commands()
        network = PathNetwork(command_runner=calls, system=lambda: "Darwin")
        with self.assertRaisesRegex(NetworkError, "Linux"):
            network.setup()
        self.assertEqual(calls.calls, [])
        network = PathNetwork(command_runner=calls, system=lambda: "Linux", ip_lookup=lambda n: "ip" if n == "ip" else None,
                              outer_namespace_isolated=lambda: True)
        with self.assertRaisesRegex(NetworkError, "iptables"):
            network.setup()
        self.assertFalse(calls.names)

    def test_rule_capture_commands_have_raw_provenance(self):
        network, commands = self.make()
        network.setup()
        before = network.evidence()["snapshots"][0]
        self.assertEqual(before["phase"], "before_connect")
        self.assertEqual(set(before["routers"]), {"source_router", "destination_router"})
        self.assertEqual(sum(len(c) > 4 and c[4] == "conntrack" for c in commands.calls), 2)
        network.close()


if __name__ == "__main__":
    unittest.main()
