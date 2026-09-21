"""Fake-command tests for the AutoNAT network namespace harness, not live evidence."""

import json
import unittest

from autonat_network import AutonatNetworkError, CommandResult, IsolatedAutonatNetwork


DEFAULT_KERNEL_TUNNELS = (
    "tunl0", "gre0", "gretap0", "erspan0", "ip_vti0", "ip6_vti0", "sit0", "ip6tnl0", "ip6gre0"
)


class FakeCommandRunner:
    def __init__(self):
        self.calls: list[list[str]] = []
        self.failures: dict[tuple[str, ...], list[CommandResult]] = {}
        self.exceptions: dict[tuple[str, ...], list[BaseException]] = {}
        self.competitor_create_then_exceptions: dict[tuple[str, ...], list[BaseException]] = {}
        self.existing_namespaces: set[str] = set()
        self.caller_routes: object = []
        self.caller_ipv6_routes: object = []
        self.namespace_routes: dict[str, object] = {}
        self.namespace_ipv6_routes: dict[str, object] = {}
        self.caller_links: list[dict[str, object]] = [{"ifname": "lo"}] + [
            {"ifname": name, "flags": ["NOARP"], "operstate": "DOWN"}
            for name in DEFAULT_KERNEL_TUNNELS
        ]
        self.extra_links: dict[str, set[str]] = {}
        self.fresh_namespace_kernel_tunnels = False
        self.tunnel_states: dict[tuple[str, str], tuple[list[str], str]] = {}
        self.network: IsolatedAutonatNetwork | None = None
        self.bridge_created = False

    def fail_once(self, command: list[str], result: CommandResult) -> None:
        self.failures.setdefault(tuple(command), []).append(result)

    def raise_once(self, command: list[str], error: BaseException) -> None:
        self.exceptions.setdefault(tuple(command), []).append(error)

    def competitor_creates_then_raises_once(self, command: list[str], error: BaseException) -> None:
        self.competitor_create_then_exceptions.setdefault(tuple(command), []).append(error)

    def __call__(self, command: list[str]) -> CommandResult:
        self.calls.append(list(command))
        if command == ["ip", "netns", "list"]:
            return CommandResult(0, "\n".join(sorted(self.existing_namespaces)))
        create_then_error = self.competitor_create_then_exceptions.get(tuple(command), [])
        if create_then_error:
            self.existing_namespaces.add(command[-1])
            raise create_then_error.pop(0)
        raised = self.exceptions.get(tuple(command), [])
        if raised:
            raise raised.pop(0)
        planned = self.failures.get(tuple(command), [])
        if planned:
            return planned.pop(0)
        if command[:3] == ["ip", "netns", "add"]:
            if command[-1] in self.existing_namespaces:
                return CommandResult(1, stderr="File exists")
            self.existing_namespaces.add(command[-1])
        if command[:3] == ["ip", "netns", "del"]:
            self.existing_namespaces.discard(command[-1])
        if command == ["ip", "-j", "route", "show", "default"]:
            return CommandResult(0, json.dumps(self.caller_routes))
        if command == ["ip", "-j", "-6", "route", "show", "default"]:
            return CommandResult(0, json.dumps(self.caller_ipv6_routes))
        if command == ["ip", "-j", "link", "show"]:
            return CommandResult(0, json.dumps(self.caller_links))
        if command[-5:] == ["-j", "-6", "route", "show", "default"]:
            return CommandResult(0, json.dumps(self.namespace_ipv6_routes.get(command[3], [])))
        if command[-4:] == ["-j", "route", "show", "default"]:
            return CommandResult(0, json.dumps(self.namespace_routes.get(command[3], [])))
        if command[-3:] == ["-j", "link", "show"]:
            return CommandResult(0, json.dumps(self._link_records(command[3])))
        if command[-5:] == ["link", "add", "br0", "type", "bridge"]:
            self.bridge_created = True
        return CommandResult(0)

    def _links(self, namespace: str) -> set[str]:
        assert self.network is not None
        names = self.network.namespaces
        if namespace == names["outer"]:
            links = {"lo"}
            if self.bridge_created:
                links.update({"br0", "c0", "s0"})
        else:
            links = {"lo", "eth0"}
        if self.fresh_namespace_kernel_tunnels:
            links.update(DEFAULT_KERNEL_TUNNELS)
        return links | self.extra_links.get(namespace, set())

    def _link_records(self, namespace: str) -> list[dict[str, object]]:
        records = []
        for name in self._links(namespace):
            record: dict[str, object] = {"ifname": name}
            if name in DEFAULT_KERNEL_TUNNELS:
                flags, state = self.tunnel_states.get((namespace, name), (["NOARP"], "DOWN"))
                record.update({"flags": flags, "operstate": state})
            records.append(record)
        return records


class IsolatedAutonatNetworkTests(unittest.TestCase):
    def make_network(self, *, client_secondary_address: bool = False,
                     system=lambda: "Linux", ip_lookup=lambda _: "ip",
                     outer_isolated=lambda: True) -> tuple[IsolatedAutonatNetwork, FakeCommandRunner]:
        commands = FakeCommandRunner()
        network = IsolatedAutonatNetwork(
            client_secondary_address,
            command_runner=commands,
            system=system,
            ip_lookup=ip_lookup,
            outer_namespace_isolated=outer_isolated,
            namespace_token="unit",
        )
        commands.network = network
        return network, commands

    def test_closed_tcp_port_is_checked_inside_owned_namespace_and_recorded(self):
        network, commands = self.make_network(ip_lookup=lambda name: name)
        network.setup()
        network.assert_closed_tcp_port("client", 9)
        command = ["ip", "netns", "exec", network.namespaces["client"], "ss", "-H", "-lnt", "sport", "=", ":9"]
        self.assertIn(command, commands.calls)
        self.assertEqual(network.evidence()["closed_tcp_ports"], [
            {"role": "client", "port": 9, "transport": "tcp", "listening": False},
        ])
        self.assertEqual(network.close(), [])

    def test_closed_tcp_port_rejects_listener_tool_failure_and_missing_tool(self):
        for answer in (CommandResult(0, "LISTEN 0 4096 0.0.0.0:9 *:*\n"),
                       CommandResult(0, "LISTEN 0 4096 [::]:9 [::]:*\n"),
                       CommandResult(1, stderr="permission denied")):
            with self.subTest(answer=answer):
                network, commands = self.make_network(ip_lookup=lambda name: name)
                network.setup()
                command = network.namespace_command("client", ["ss", "-H", "-lnt", "sport", "=", ":9"])
                commands.fail_once(command, answer)
                with self.assertRaises(AutonatNetworkError):
                    network.assert_closed_tcp_port("client", 9)
                self.assertEqual(network.evidence()["closed_tcp_ports"], [])
                network.close()
        network, commands = self.make_network(ip_lookup=lambda name: "ip" if name == "ip" else None)
        network.setup()
        with self.assertRaisesRegex(AutonatNetworkError, "requires iproute2"):
            network.assert_closed_tcp_port("client", 9)
        network.close()

    def test_closed_tcp_port_has_no_pre_setup_or_invalid_port_fallback(self):
        network, commands = self.make_network(ip_lookup=lambda name: name)
        with self.assertRaisesRegex(AutonatNetworkError, "not ready"):
            network.assert_closed_tcp_port("client", 9)
        for port in (0, 65536, True, "9"):
            with self.subTest(port=port), self.assertRaises(ValueError):
                network.assert_closed_tcp_port("client", port)
        self.assertEqual(commands.calls, [])

    def test_setup_uses_only_fresh_namespaces_and_public_test_addresses(self) -> None:
        network, commands = self.make_network()

        network.setup()

        self.assertEqual(network.addresses, {"client": ("11.0.0.1",), "server": ("11.0.0.2",)})
        self.assertEqual(
            network.namespace_command("client", ["fixture", "listen"]),
            ["ip", "netns", "exec", "autonat-c-unit", "fixture", "listen"],
        )
        self.assertEqual(network.evidence()["outer_network"], {
            "bridge": "br0", "default_route": "absent", "external_links": "absent"
        })
        self.assertEqual(commands.calls[:2], [
            ["ip", "-j", "route", "show", "default"], ["ip", "-j", "-6", "route", "show", "default"]
        ])
        self.assertIn(["ip", "netns", "add", "autonat-o-unit"], commands.calls)
        self.assertTrue(all(
            command in (
                ["ip", "-j", "route", "show", "default"],
                ["ip", "-j", "-6", "route", "show", "default"],
                ["ip", "-j", "link", "show"],
                ["ip", "netns", "list"],
            )
            or command[:3] in (["ip", "netns", "add"], ["ip", "netns", "del"])
            or command[:3] == ["ip", "netns", "exec"]
            for command in commands.calls
        ))
        self.assertFalse(any("default" in command and "add" in command for command in commands.calls))

    def test_client_secondary_address_is_on_the_same_host_for_v2_probe_control(self) -> None:
        network, commands = self.make_network(client_secondary_address=True)

        network.setup()

        self.assertEqual(network.addresses["client"], ("11.0.0.1", "11.0.0.3"))
        self.assertIn(
            ["ip", "netns", "exec", "autonat-c-unit", "ip", "addr", "add", "11.0.0.3/24", "dev", "eth0"],
            commands.calls,
        )
        self.assertEqual(
            [command[-1] for command in commands.calls if command[:3] == ["ip", "netns", "add"]],
            [network.namespaces["outer"], network.namespaces["client"], network.namespaces["server"]],
        )

    def test_fresh_namespaces_allow_only_default_down_kernel_tunnels(self) -> None:
        network, commands = self.make_network()
        commands.fresh_namespace_kernel_tunnels = True

        network.setup()

        self.assertEqual(network.evidence()["outer_network"]["external_links"], "absent")

    def test_fresh_child_rejects_an_up_kernel_tunnel(self) -> None:
        network, commands = self.make_network()
        commands.fresh_namespace_kernel_tunnels = True
        commands.tunnel_states[(network.namespaces["client"], "gre0")] = (["UP"], "UP")

        with self.assertRaisesRegex(AutonatNetworkError, "tunnel gre0 is not verifiably DOWN"):
            network.setup()

    def test_non_linux_refuses_before_a_network_command(self) -> None:
        network, commands = self.make_network(system=lambda: "Darwin")

        with self.assertRaisesRegex(AutonatNetworkError, "requires Linux"):
            network.setup()

        self.assertEqual(commands.calls, [])

    def test_missing_iproute2_refuses_host_network_fallback(self) -> None:
        network, commands = self.make_network(ip_lookup=lambda _: None)

        with self.assertRaisesRegex(AutonatNetworkError, "iproute2"):
            network.setup()

        self.assertEqual(commands.calls, [])

    def test_shared_caller_namespace_refuses_before_network_inspection(self) -> None:
        network, commands = self.make_network(outer_isolated=lambda: False)

        with self.assertRaisesRegex(AutonatNetworkError, "unshare --net --mount"):
            network.setup()

        self.assertEqual(commands.calls, [])

    def test_caller_default_route_refuses_before_namespace_creation(self) -> None:
        network, commands = self.make_network()
        commands.caller_routes = [{"dst": "default", "gateway": "192.0.2.1"}]

        with self.assertRaisesRegex(AutonatNetworkError, "caller outer network: default route"):
            network.setup()

        self.assertEqual(commands.calls, [["ip", "-j", "route", "show", "default"]])

    def test_caller_ipv6_default_route_refuses_before_namespace_creation(self) -> None:
        network, commands = self.make_network()
        commands.caller_ipv6_routes = [{"dst": "default", "gateway": "2001:db8::1"}]

        with self.assertRaisesRegex(AutonatNetworkError, "IPv6 default route"):
            network.setup()

        self.assertNotIn(["ip", "netns", "add", network.namespaces["outer"]], commands.calls)

    def test_caller_external_or_up_tunnel_refuses_before_namespace_creation(self) -> None:
        for changed_link, expected in (
            ({"ifname": "eth0", "flags": ["UP"], "operstate": "UP"}, r"unexpected=\['eth0'\]"),
            ({"ifname": "gre0", "flags": ["UP"], "operstate": "UP"}, "tunnel gre0 is not verifiably DOWN"),
        ):
            with self.subTest(link=changed_link["ifname"]):
                network, commands = self.make_network()
                commands.caller_links = [{"ifname": "lo"}, changed_link]

                with self.assertRaisesRegex(AutonatNetworkError, expected):
                    network.setup()

                self.assertNotIn(["ip", "netns", "add", network.namespaces["outer"]], commands.calls)

    def test_capability_error_is_clear_and_leaves_no_namespace(self) -> None:
        network, commands = self.make_network()
        outer = network.namespaces["outer"]
        commands.fail_once(
            ["ip", "netns", "add", outer], CommandResult(1, stderr="Operation not permitted")
        )

        with self.assertRaisesRegex(AutonatNetworkError, "CAP_SYS_ADMIN and CAP_NET_ADMIN"):
            network.setup()

        self.assertEqual(commands.calls[-1], ["ip", "netns", "add", outer])

    def test_competitor_creation_between_precheck_and_timeout_remains_uncertain(self) -> None:
        network, commands = self.make_network()
        outer = network.namespaces["outer"]
        add = ["ip", "netns", "add", outer]
        commands.competitor_creates_then_raises_once(add, AutonatNetworkError("network command timed out"))

        with self.assertRaisesRegex(AutonatNetworkError, "timed out"):
            network.setup()

        self.assertIn(outer, commands.existing_namespaces)
        self.assertNotIn(["ip", "netns", "del", outer], commands.calls)
        self.assertEqual(network.evidence()["state"], "failed")
        self.assertEqual(network.evidence()["cleanup_uncertainty"], [{
            "role": "outer",
            "namespace": outer,
            "reason": "netns add raised before ownership could be established; refusing existence-based ownership inference",
        }])
        self.assertEqual(
            [command for command in commands.calls if command == ["ip", "netns", "list"]],
            [["ip", "netns", "list"]],
        )
        add_evidence = next(item for item in network.evidence()["commands"] if item["command"] == add)
        self.assertEqual(add_evidence["exception"], {
            "type": "AutonatNetworkError", "message": "network command timed out"
        })

    def test_preexisting_namespace_collision_is_preserved(self) -> None:
        network, commands = self.make_network()
        outer = network.namespaces["outer"]
        commands.existing_namespaces.add(outer)

        with self.assertRaisesRegex(AutonatNetworkError, "namespace collision"):
            network.setup()

        self.assertIn(outer, commands.existing_namespaces)
        self.assertNotIn(["ip", "netns", "add", outer], commands.calls)
        self.assertNotIn(["ip", "netns", "del", outer], commands.calls)

    def test_keyboard_interrupt_after_child_creation_cleans_owned_namespaces(self) -> None:
        network, commands = self.make_network()
        outer = network.namespaces["outer"]
        commands.raise_once(
            ["ip", "netns", "exec", outer, "ip", "link", "add", "br0", "type", "bridge"],
            KeyboardInterrupt(),
        )

        with self.assertRaises(KeyboardInterrupt):
            network.setup()

        self.assertFalse(commands.existing_namespaces)
        deletes = [command[-1] for command in commands.calls if command[:3] == ["ip", "netns", "del"]]
        self.assertEqual(deletes, [network.namespaces["server"], network.namespaces["client"], outer])

    def test_command_exception_is_retained_in_network_evidence(self) -> None:
        network, commands = self.make_network()
        outer = network.namespaces["outer"]
        failing_command = ["ip", "netns", "exec", outer, "ip", "link", "set", "lo", "up"]
        commands.raise_once(failing_command, RuntimeError("controlled command failure"))

        with self.assertRaisesRegex(RuntimeError, "controlled command failure"):
            network.setup()

        evidence = next(item for item in network.evidence()["commands"] if item["command"] == failing_command)
        self.assertEqual(evidence["exception"], {
            "type": "RuntimeError", "message": "controlled command failure"
        })
        self.assertNotIn(outer, commands.existing_namespaces)

    def test_default_route_in_outer_namespace_fails_closed_and_cleans_it(self) -> None:
        network, commands = self.make_network()
        outer = network.namespaces["outer"]
        commands.namespace_routes[outer] = [{"dst": "default", "gateway": "192.0.2.1"}]

        with self.assertRaisesRegex(AutonatNetworkError, "default route"):
            network.setup()

        self.assertIn(["ip", "netns", "del", outer], commands.calls)
        self.assertNotIn(["ip", "netns", "add", network.namespaces["client"]], commands.calls)

    def test_external_link_in_outer_namespace_fails_closed_and_cleans_it(self) -> None:
        network, commands = self.make_network()
        outer = network.namespaces["outer"]
        commands.extra_links[outer] = {"eth9"}

        with self.assertRaisesRegex(AutonatNetworkError, r"unexpected=\['eth9'\]"):
            network.setup()

        self.assertIn(["ip", "netns", "del", outer], commands.calls)

    def test_partial_setup_deletes_created_children_before_outer_namespace(self) -> None:
        network, commands = self.make_network()
        client = network.namespaces["client"]
        commands.fail_once(
            ["ip", "netns", "add", client], CommandResult(1, stderr="simulated namespace failure")
        )

        with self.assertRaisesRegex(AutonatNetworkError, "simulated namespace failure"):
            network.setup()

        deletes = [command[-1] for command in commands.calls if command[:3] == ["ip", "netns", "del"]]
        self.assertEqual(deletes, [network.namespaces["outer"]])

    def test_cleanup_retries_only_remaining_child_before_deleting_outer(self) -> None:
        network, commands = self.make_network()
        network.setup()
        server = network.namespaces["server"]
        commands.fail_once(["ip", "netns", "del", server], CommandResult(1, stderr="busy"))

        first_errors = network.close()
        second_errors = network.close()

        self.assertEqual(len(first_errors), 1)
        self.assertEqual(second_errors, [])
        deletes = [command[-1] for command in commands.calls if command[:3] == ["ip", "netns", "del"]]
        self.assertEqual(deletes, [server, network.namespaces["client"], server, network.namespaces["outer"]])
        self.assertEqual(network.evidence()["state"], "closed")

    def test_cleanup_refuses_to_delete_a_namespace_with_a_remaining_process(self) -> None:
        network, commands = self.make_network()
        network.setup()
        server = network.namespaces["server"]
        commands.fail_once(["ip", "netns", "pids", server], CommandResult(0, stdout="451\n"))

        errors = network.close()

        self.assertEqual(len(errors), 1)
        self.assertIn("owned processes remain", errors[0])
        deletes = [command[-1] for command in commands.calls if command[:3] == ["ip", "netns", "del"]]
        self.assertEqual(deletes, [network.namespaces["client"]])
        self.assertNotIn(network.namespaces["outer"], deletes)


if __name__ == "__main__":
    unittest.main()
