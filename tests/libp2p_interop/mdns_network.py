"""Two isolated multicast participants; setup evidence is not live discovery proof."""

import ipaddress

from isolated_network import IsolatedNetwork, NetworkError


class IsolatedMdnsNetwork(IsolatedNetwork):
    _PREFIX = "mdns"
    _LABEL = "mDNS"
    _KIND = "linux_isolated_mdns_netns"

    def __init__(self, family: int = 4, **dependencies):
        if type(family) is not int or family not in (4, 6):
            raise ValueError("mDNS family must be 4 or 6")
        self.family = family
        self._ADDRESSES = (
            {"client": "10.231.77.1", "server": "10.231.77.2"} if family == 4 else
            {"client": "fd00:666f:7267::1", "server": "fd00:666f:7267::2"}
        )
        self._verified_interfaces = {}
        super().__init__(False, **dependencies)

    def _configure_bridge(self) -> None:
        # Flood multicast within this isolated two-port bridge. No dependency on
        # a host querier or multicast membership timing during fixture startup.
        self._inside("outer", "link", "set", "dev", "br0", "type", "bridge", "mcast_snooping", "0")

    def _configure_role_addresses(self, role: str) -> None:
        self._inside(role, "link", "set", "dev", "eth0", "multicast", "on")
        if self.family == 4:
            self._inside(role, "addr", "add", f"{self._ADDRESSES[role]}/24", "dev", "eth0")
        else:
            # A fresh namespace may inherit disabled IPv6 from the test host.
            # Change only the owned interface; never sysctl the caller/host.
            sysctl = self._ip_lookup("sysctl")
            if not sysctl:
                raise NetworkError("IPv6 mDNS fixture requires sysctl")
            self._run_checked([self._ip, "netns", "exec", self._namespaces[role], sysctl,
                               "-w", "net.ipv6.conf.eth0.disable_ipv6=0"])
            self._inside(role, "-6", "addr", "add", f"{self._ADDRESSES[role]}/64", "dev", "eth0", "nodad")

    def _verify_isolated_namespace(self, role: str, expected_links: set[str]) -> None:
        super()._verify_isolated_namespace(role, expected_links)
        if role == "outer":
            return
        records = self._json_output(
            self._inside_command(self._namespaces[role], "-j", "addr", "show", "dev", "eth0"),
            "mDNS interface inspection",
        )
        if not isinstance(records, list) or len(records) != 1 or not isinstance(records[0], dict):
            raise NetworkError("mDNS interface inspection requires one interface")
        record = records[0]
        flags = record.get("flags")
        addresses = record.get("addr_info")
        if (record.get("ifname") != "eth0" or type(record.get("ifindex")) is not int
                or record["ifindex"] <= 0 or not isinstance(flags, list)
                or "UP" not in flags or "MULTICAST" not in flags or not isinstance(addresses, list)):
            raise NetworkError("mDNS interface is not verifiably up and multicast-capable")
        expected = self._ADDRESSES[role]
        matching = [a for a in addresses if isinstance(a, dict) and a.get("local") == expected]
        if (len(matching) != 1 or matching[0].get("family") != ("inet" if self.family == 4 else "inet6")
                or matching[0].get("prefixlen") != (24 if self.family == 4 else 64)
                or matching[0].get("tentative", False) is not False
                or matching[0].get("dadfailed", False) is not False
                or any(flag in matching[0].get("flags", []) for flag in ("tentative", "dadfailed"))):
            raise NetworkError("mDNS interface lacks its usable configured address")
        self._verified_interfaces[role] = {
            "name": "eth0", "index": record["ifindex"], "up": True, "multicast": True,
            "address": str(ipaddress.ip_address(expected)),
        }

    def evidence(self) -> dict[str, object]:
        value = super().evidence()
        value.pop("closed_tcp_ports")
        value.update({
            "schema": "forge.mdns.network.v1", "family": self.family,
            "multicast": {"group": "224.0.0.251" if self.family == 4 else "ff02::fb",
                          "port": 5353, "bridge_snooping": "disabled_by_checked_command"},
            "interfaces": dict(self._verified_interfaces),
        })
        return value

    def set_interface_up(self, role: str, up: bool) -> None:
        """Checked mutation of an owned participant, never the caller's interface."""
        if role not in ("client", "server") or type(up) is not bool or self.evidence()["state"] != "ready":
            raise NetworkError("interface transition requires a ready owned participant")
        self._inside(role, "link", "set", "dev", "eth0", "up" if up else "down")
        records = self._json_output(
            self._inside_command(self._namespaces[role], "-j", "link", "show", "dev", "eth0"),
            "mDNS interface transition",
        )
        if (not isinstance(records, list) or len(records) != 1
                or not isinstance(records[0], dict) or records[0].get("ifname") != "eth0"
                or not isinstance(records[0].get("flags"), list)
                or ("UP" in records[0]["flags"]) != up
                or records[0].get("ifindex") != self._verified_interfaces[role]["index"]):
            raise NetworkError("interface transition was not corroborated by the kernel")
        self._verified_interfaces[role]["up"] = up
