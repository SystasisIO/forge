"""Synthetic kernel-command ownership regressions, not live wire evidence."""

import unittest

from coordinated_network import CoordinatedNetwork
from isolated_network import CommandResult, NetworkError
from test_path_network import Commands


class TCPCommands(Commands):
    def __init__(self):
        super().__init__()
        self.sockets = {}

    def __call__(self, args):
        if "ss" in args:
            self.calls.append(args)
            return CommandResult(0, self.sockets.get(args[3], ""))
        return super().__call__(args)


class CoordinatedNetworkTests(unittest.TestCase):
    def make(self):
        commands = TCPCommands()
        network = CoordinatedNetwork(command_runner=commands, system=lambda: "Linux", ip_lookup=lambda n: n,
                                     outer_namespace_isolated=lambda: True, namespace_token="unit")
        network.setup()
        return network, commands

    def test_only_exact_initial_syn_tuples_are_held(self):
        network, commands = self.make()
        network.arm({"client": 40010, "server": 40020})
        rules = [c for c in commands.calls if "-A" in c]
        self.assertEqual(len(rules), 2)
        self.assertTrue(all(c[c.index("--tcp-flags") + 1:c.index("--tcp-flags") + 3] == ["SYN,ACK", "SYN"] for c in rules))
        self.assertEqual({c[c.index("--sport") + 1] for c in rules}, {"40010", "40020"})
        self.assertTrue(all(c[-1] == "DROP" and c[c.index("-A") + 1] == "OUTPUT" for c in rules))
        self.assertEqual(network.close(), [])
        self.assertFalse(commands.names)

    def test_release_requires_both_actual_native_socket_states(self):
        network, commands = self.make()
        network.arm({"client": 40010, "server": 40020})
        with self.assertRaises(NetworkError):
            network.release()
        commands.sockets[network.namespaces["client"]] = "0 1 11.0.0.1:40010 11.0.0.2:40020\n"
        self.assertFalse(network.observe_syn_sent())
        with self.assertRaises(NetworkError):
            network.release()
        commands.sockets[network.namespaces["server"]] = "0 1 11.0.0.2:40020 11.0.0.1:40010\n"
        self.assertTrue(network.observe_syn_sent())
        network.release()
        removals = [c for c in commands.calls if "-D" in c]
        self.assertEqual(len(removals), 2)
        self.assertTrue(network.evidence()["coordination"]["released"])
        with self.assertRaises(NetworkError):
            network.release()
        network.close()

    def test_ephemeral_or_unrelated_sockets_do_not_satisfy_barrier(self):
        network, commands = self.make()
        network.arm({"client": 40010, "server": 40020})
        commands.sockets[network.namespaces["client"]] = "0 1 11.0.0.1:45000 11.0.0.2:40020\n"
        commands.sockets[network.namespaces["server"]] = "0 1 11.0.0.2:40020 11.0.0.3:40010\n"
        self.assertFalse(network.observe_syn_sent())
        self.assertEqual(network.evidence()["coordination"]["syn_sent"], {})
        network.close()

    def test_disjoint_observations_never_accumulate_into_bilateral_state(self):
        network, commands = self.make()
        network.arm({"client": 40010, "server": 40020})
        commands.sockets[network.namespaces["client"]] = "0 1 11.0.0.1:40010 11.0.0.2:40020\n"
        self.assertFalse(network.observe_syn_sent())
        commands.sockets[network.namespaces["client"]] = ""
        commands.sockets[network.namespaces["server"]] = "0 1 11.0.0.2:40020 11.0.0.1:40010\n"
        self.assertFalse(network.observe_syn_sent())
        with self.assertRaises(NetworkError):
            network.release()
        self.assertFalse(any("-D" in c for c in commands.calls))
        network.close()

    def test_release_rechecks_sockets_after_successful_observation(self):
        network, commands = self.make()
        network.arm({"client": 40010, "server": 40020})
        commands.sockets[network.namespaces["client"]] = "0 1 11.0.0.1:40010 11.0.0.2:40020\n"
        commands.sockets[network.namespaces["server"]] = "0 1 11.0.0.2:40020 11.0.0.1:40010\n"
        self.assertTrue(network.observe_syn_sent())
        commands.sockets[network.namespaces["server"]] = ""
        with self.assertRaises(NetworkError):
            network.release()
        self.assertFalse(any("-D" in c for c in commands.calls))
        network.close()

    def test_second_rule_install_or_removal_failure_preserves_namespace_cleanup(self):
        for action in ("-A", "-D"):
            commands = TCPCommands()
            count = 0
            def run(args):
                nonlocal count
                if "iptables" in args and action in args:
                    count += 1
                    if count == 2:
                        return CommandResult(1, "", "injected second rule failure")
                return commands(args)
            network = CoordinatedNetwork(command_runner=run, system=lambda: "Linux", ip_lookup=lambda n: n,
                                         outer_namespace_isolated=lambda: True, namespace_token="failure")
            network.setup()
            if action == "-A":
                with self.assertRaises(NetworkError):
                    network.arm({"client": 40010, "server": 40020})
            else:
                network.arm({"client": 40010, "server": 40020})
                commands.sockets[network.namespaces["client"]] = "0 1 11.0.0.1:40010 11.0.0.2:40020\n"
                commands.sockets[network.namespaces["server"]] = "0 1 11.0.0.2:40020 11.0.0.1:40010\n"
                with self.assertRaises(NetworkError):
                    network.release()
                self.assertFalse(network.evidence()["coordination"]["released"])
                with self.assertRaises(NetworkError):
                    network.observe_syn_sent()
                with self.assertRaises(NetworkError):
                    network.release()
            self.assertEqual(network.close(), [])
            self.assertFalse(commands.names)

    def test_duplicate_and_oversized_kernel_capture_fail_closed(self):
        for value in ("0 1 11.0.0.1:40010 11.0.0.2:40020\n" * 2, "x" * 8193):
            network, commands = self.make()
            network.arm({"client": 40010, "server": 40020})
            commands.sockets[network.namespaces["client"]] = value
            with self.assertRaises(NetworkError):
                network.observe_syn_sent()
            network.close()

    def test_invalid_ports_reject_before_rule_creation(self):
        for ports in ({"client": 1}, {"client": True, "server": 40020}, {"client": 0, "server": 40020},
                      {"client": 40010, "server": 65536}):
            network, commands = self.make()
            with self.assertRaises(ValueError):
                network.arm(ports)
            self.assertFalse(any("iptables" in c for c in commands.calls))
            network.close()

    def test_failed_rule_install_keeps_cleanup_owned(self):
        network, commands = self.make()
        commands.fail = "iptables"
        with self.assertRaises(NetworkError):
            network.arm({"client": 40010, "server": 40020})
        commands.fail = None
        self.assertEqual(network.close(), [])
        self.assertFalse(commands.names)


if __name__ == "__main__":
    unittest.main()
