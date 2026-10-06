"""Owned two-peer TCP coordination barrier, never a socket/upgrade substitute.

Hold only the exact initial SYN tuples until both real outgoing sockets are in
SYN-SENT. Releasing the rules lets the kernel perform simultaneous TCP connect;
authentication and application traffic must still use each native libp2p stack.
"""

import time

from isolated_network import IsolatedNetwork, NetworkError


class CoordinatedNetwork(IsolatedNetwork):
    _PREFIX = "reuse"
    _LABEL = "coordinated TCP"
    _KIND = "linux_coordinated_tcp_netns"

    def __init__(self, **support):
        super().__init__(**support)
        self._rules = {}
        self._ports = {}
        self._syn_sent = {}
        self._released = False
        self._iptables = None
        self._ss = None

    def arm(self, ports):
        if self._state != "ready" or self._rules or self._ports:
            raise NetworkError("coordination barrier requires a fresh ready network")
        if (not isinstance(ports, dict) or set(ports) != {"client", "server"}
                or any(type(p) is not int or not 0 < p < 65536 for p in ports.values())):
            raise ValueError("coordination requires both actual nonzero listener ports")
        self._iptables, self._ss = self._ip_lookup("iptables"), self._ip_lookup("ss")
        if not self._iptables or not self._ss:
            raise NetworkError("coordinated TCP requires iptables and ss; no emulated barrier")
        self._ports = dict(ports)
        for role in ("client", "server"):
            other = "server" if role == "client" else "client"
            rule = ["OUTPUT", "-p", "tcp", "-s", self._ADDRESSES[role], "-d", self._ADDRESSES[other],
                    "--sport", str(ports[role]), "--dport", str(ports[other]),
                    "--tcp-flags", "SYN,ACK", "SYN", "-j", "DROP"]
            self._run_checked(self.namespace_command(role, [self._iptables, "-w", "2", "-A", *rule]))
            self._rules[role] = rule

    def observe_syn_sent(self):
        if len(self._rules) != 2 or self._released:
            raise NetworkError("SYN-SENT observation requires both held tuple rules")
        # These are sequential kernel snapshots, not an atomic cross-host view.
        # Actor owners hold both pending attempts until release or cancellation.
        self._syn_sent = {}
        for role in ("client", "server"):
            other = "server" if role == "client" else "client"
            command = self.namespace_command(role, [self._ss, "-H", "-nt", "state", "syn-sent"])
            output = self._run_checked(command).stdout
            if len(output) > 8192:
                raise NetworkError("unbounded native TCP state capture")
            local = f"{self._ADDRESSES[role]}:{self._ports[role]}"
            remote = f"{self._ADDRESSES[other]}:{self._ports[other]}"
            matches = [line for line in output.splitlines() if line.split()[-2:] == [local, remote]]
            if len(matches) > 1:
                raise NetworkError("ambiguous native coordinated TCP socket")
            if matches:
                self._syn_sent[role] = {"local": local, "remote": remote, "stdout": output,
                                        "command": command}
        return set(self._syn_sent) == {"client", "server"}

    def await_syn_sent(self, deadline):
        while time.monotonic() < deadline:
            if self.observe_syn_sent():
                return
            time.sleep(0.01)
        raise NetworkError("both native listener-port sockets did not reach SYN-SENT")

    def release(self):
        if len(self._rules) != 2 or self._released or not self.observe_syn_sent():
            raise NetworkError("cannot release coordination before actual bilateral SYN-SENT")
        for role, rule in tuple(self._rules.items()):
            self._run_checked(self.namespace_command(role, [self._iptables, "-w", "2", "-D", *rule]))
            del self._rules[role]
        self._released = True

    def evidence(self):
        value = super().evidence()
        value["coordination"] = {"ports": dict(self._ports), "syn_sent": dict(self._syn_sent),
                                 "released": self._released}
        return value
