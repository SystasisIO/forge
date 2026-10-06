"""Owned Linux conntrack NAT topology; no host-network or userspace-NAT fallback.

Run from the same unshared net/mount namespace required by IsolatedNetwork.
The three fixture processes are owned by process_lifecycle, not by this module.
"""

import json
from pathlib import Path
import time

from isolated_network import IsolatedNetwork, NetworkError
from process_lifecycle import StopBudget, enter_scope, exit_scope, spawn_owned


class PathNetwork(IsolatedNetwork):
    _PREFIX = "path"
    _LABEL = "DCUtR NAT"
    _KIND = "linux_dcutr_conntrack_nat"
    _ADDRESSES = {"relay": "11.0.0.1", "source_router": "11.0.0.2",
                  "destination_router": "11.0.0.3", "source": "10.1.0.2",
                  "destination": "10.2.0.2"}
    _OUTER_LINKS = {"relay": "r0", "source_router": "a0", "destination_router": "b0"}

    def __init__(self, **support):
        super().__init__(**support)
        token = self._namespaces["outer"].removeprefix("path-o-")
        self._roles = ["relay", "source_router", "destination_router", "source", "destination"]
        self._namespaces = {"outer": self._namespaces["outer"], **{
            role: f"path-{role}-{token}" for role in self._roles}}
        self._tools = {}
        self._snapshots = []
        self._faults = []

    @property
    def addresses(self):
        return {role: (address,) for role, address in self._ADDRESSES.items()}

    def _tool(self, role, name, *args):
        return self._run_checked([self._ip, "netns", "exec", self._namespaces[role],
                                  self._tools[name], *args])

    def setup(self):
        if self._state == "ready":
            return self
        if self._state != "new":
            raise NetworkError("NAT topology is single-use")
        self._state = "setting_up"
        try:
            self._require_linux_iproute()
            for name in ("iptables", "iptables-save", "conntrack", "sysctl"):
                self._tools[name] = self._ip_lookup(name)
                if not self._tools[name]:
                    raise NetworkError(f"DCUtR NAT requires {name}; no emulated fallback")
            self._add_namespace("outer")
            self._inside("outer", "link", "set", "lo", "up")
            self._verify_isolated_namespace("outer", {"lo"})
            for role in self._roles:
                self._add_namespace(role)
            self._inside("outer", "link", "add", "br0", "type", "bridge")
            self._inside("outer", "link", "set", "br0", "up")
            for role in self._OUTER_LINKS:
                self._connect_role(role)
            self._verify_isolated_namespace("outer", {"lo", "br0", "r0", "a0", "b0"})
            self._verify_isolated_namespace("relay", {"lo", "eth0"})
            for index, role in enumerate(("source", "destination"), 1):
                router = role + "_router"
                self._inside(router, "link", "add", "lan0", "type", "veth", "peer", "name", "eth0",
                             "netns", self._namespaces[role])
                self._inside(router, "addr", "add", f"10.{index}.0.1/24", "dev", "lan0")
                self._inside(router, "link", "set", "lan0", "up")
                self._inside(role, "link", "set", "lo", "up")
                self._configure_role_addresses(role)
                self._inside(role, "link", "set", "eth0", "up")
                self._verify_isolated_namespace(router, {"lo", "eth0", "lan0"})
                self._verify_isolated_namespace(role, {"lo", "eth0"})
                self._inside(role, "route", "add", "default", "via", f"10.{index}.0.1", "dev", "eth0")
                routes = self._json_output(self._inside_command(self._namespaces[role],
                                          "-j", "route", "show", "default"), "owned LAN route")
                if len(routes) != 1 or routes[0].get("gateway") != f"10.{index}.0.1" \
                        or routes[0].get("dev") != "eth0":
                    raise NetworkError("LAN default route does not point to its owned NAT router")
                self._tool(router, "sysctl", "-w", "net.ipv4.ip_forward=1")
                # Default DROP blocks unilateral inbound traffic. Coordinated native
                # UDP creates both conntrack mappings; there is no rule-release gate.
                self._tool(router, "iptables", "-w", "2", "-P", "FORWARD", "DROP")
                # Unmapped WAN UDP must be silently discarded before confirmation;
                # otherwise the router's own ICMP port-unreachable removes the
                # opposite mapping and makes this a rejector, not a dropping NAT.
                self._tool(router, "iptables", "-w", "2", "-A", "INPUT", "-i", "eth0", "-p", "udp", "-j", "DROP")
                self._tool(router, "iptables", "-w", "2", "-A", "FORWARD", "-i", "lan0", "-o", "eth0", "-j", "ACCEPT")
                self._tool(router, "iptables", "-w", "2", "-A", "FORWARD", "-i", "eth0", "-o", "lan0",
                           "-m", "conntrack", "--ctstate", "ESTABLISHED,RELATED", "-j", "ACCEPT")
                self._tool(router, "iptables", "-w", "2", "-t", "nat", "-A", "POSTROUTING",
                           "-s", f"10.{index}.0.0/24", "-o", "eth0", "-j", "SNAT",
                           "--to-source", self._ADDRESSES[router])
            self._isolation_verified = True
            self._state = "ready"
            self.capture("before_connect")
            return self
        except BaseException as error:
            self._state = "failed"
            self._attach_cleanup_failures(error, self.close())
            raise

    def capture(self, phase):
        if self._state != "ready" or phase not in ("before_connect", "barrier", "after_upgrade", "after_fault"):
            raise NetworkError("invalid NAT capture phase/state")
        snapshot = {"phase": phase, "routers": {}}
        for role in ("source_router", "destination_router"):
            rules = self._tool(role, "iptables-save", "-c").stdout
            flows = self._tool(role, "conntrack", "-L", "-p", "udp", "-o", "extended").stdout
            snapshot["routers"][role] = {"rules": rules, "conntrack": flows}
        self._snapshots.append(snapshot)
        return snapshot

    def block_peer_udp(self):
        """Fail coordinated UDP without disrupting either peer's relay transport."""
        if self._state != "ready" or self._faults:
            raise NetworkError("UDP fault requires a ready, unfaulted network")
        for role, other in (("source_router", "destination_router"), ("destination_router", "source_router")):
            self._tool(role, "iptables", "-w", "2", "-I", "FORWARD", "1", "-i", "eth0", "-o", "lan0",
                       "-p", "udp", "-s", self._ADDRESSES[other], "-j", "DROP")
        self._faults.append({"kind": "peer_udp_drop", "before_circuit_connect": True})
        self.capture("after_fault")

    def evidence(self):
        value = super().evidence()
        value.update({"snapshots": list(self._snapshots), "faults": list(self._faults),
                      "nat": {"source": {"lan": "10.1.0.2", "wan": "11.0.0.2"},
                              "destination": {"lan": "10.2.0.2", "wan": "11.0.0.3"}}})
        return value

    def probe_datagrams(self, root):
        """Network-only preflight, in a disposable topology separate from DCUtR.

        Real bound UDP sockets and Linux conntrack establish the packet rules,
        not libp2p acceptance. Scope owns both processes, including failure paths.
        """
        if self._state != "ready":
            raise NetworkError("UDP preflight requires ready isolated NAT")
        python = self._ip_lookup("python3")
        if not python:
            raise NetworkError("UDP preflight requires Python, no packet emulation fallback")
        root = Path(root)
        root.mkdir(parents=True, exist_ok=False)
        scope, token = enter_scope()
        script = r'''
import argparse, json, pathlib, socket, time
p = argparse.ArgumentParser()
for key in ("role", "ip", "wan", "ready-file", "result-file", "control-file", "stop-file"):
    p.add_argument("--" + key, required=True)
a = p.parse_args()
def write(path, value):
    path = pathlib.Path(path); tmp = path.with_suffix(".tmp")
    tmp.write_text(json.dumps(value)); tmp.replace(path)
def wait_file(path):
    end = time.monotonic() + 5
    while not pathlib.Path(path).exists():
        if time.monotonic() > end: raise TimeoutError("UDP preflight control")
        time.sleep(.01)
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
    s.setsockopt(socket.IPPROTO_IP, socket.IP_TTL, 4)
    s.bind((a.ip, 40010)); s.settimeout(.5)
    if a.role == "source":
        wait_file(a.control_file + ".unilateral")
        s.sendto(b"unilateral", (a.wan, 40010))
        write(a.ready_file, {"bound": list(s.getsockname()), "unilateral_sent": True})
    else:
        write(a.ready_file, {"bound": list(s.getsockname()), "listening": True})
        wait_file(a.control_file + ".unilateral")
        try: s.recvfrom(128); raise AssertionError("unilateral inbound passed NAT")
        except socket.timeout: pass
        write(a.result_file, {"unilateral_blocked": True})
    wait_file(a.control_file)
    s.settimeout(3)
    if a.role == "destination":
        s.sendto(b"destination-open", (a.wan, 40010))
        data, peer = s.recvfrom(128)
        assert data == b"coordinated" and peer == (a.wan, 40010), (data, peer)
        s.sendto(data, peer)
    else:
        data, peer = s.recvfrom(128)
        assert data == b"destination-open" and peer == (a.wan, 40010), (data, peer)
        s.sendto(b"coordinated", peer)
        data, peer = s.recvfrom(128)
        assert data == b"coordinated" and peer == (a.wan, 40010), (data, peer)
    write(a.result_file, {"unilateral_blocked": True, "coordinated_echo": True, "bound": list(s.getsockname())})
    wait_file(a.stop_file)
'''
        actors, errors = {}, []
        try:
            deadline = time.monotonic() + 12

            def wait(path, predicate):
                while time.monotonic() < deadline:
                    if path.exists():
                        value = json.loads(path.read_text())
                        if predicate(value):
                            return value
                    if any(owner.process.poll() is not None for owner, _ in actors.values()):
                        raise NetworkError("UDP preflight process exited early")
                    time.sleep(.01)
                raise NetworkError("bounded UDP preflight deadline")

            for role in ("destination", "source"):
                files = {name: root / f"{role}.{name}" for name in ("ready", "result", "control", "stop")}
                other = "source_router" if role == "destination" else "destination_router"
                args = [python, "-c", script, "--role", role, "--ip", self._ADDRESSES[role],
                        "--wan", self._ADDRESSES[other]]
                for key, flag in (("ready", "ready-file"), ("result", "result-file"),
                                  ("control", "control-file"), ("stop", "stop-file")):
                    args += ["--" + flag, str(files[key])]
                owner = spawn_owned(self.namespace_command(role, args), root / f"{role}.log", files["stop"],
                                    stop_budget=StopBudget(3, 0, 1))
                actors[role] = (owner, files)
                if role == "destination":
                    owner.ready = wait(files["ready"], lambda v: v.get("listening") is True)
            for _, files in actors.values():
                Path(str(files["control"]) + ".unilateral").write_text("send\n")
            actors["source"][0].ready = wait(actors["source"][1]["ready"], lambda v: v.get("unilateral_sent") is True)
            wait(actors["destination"][1]["result"], lambda v: v.get("unilateral_blocked") is True)
            self.capture("barrier")
            for _, files in actors.values():
                files["control"].write_text("coordinate\n")
            for _, files in actors.values():
                wait(files["result"], lambda v: v.get("coordinated_echo") is True)
            self.capture("after_upgrade")
        except Exception as error:
            errors.append(str(error))
        finally:
            errors.extend(scope.close())
            result = {"kind": "udp_network_preflight_not_dcutr_acceptance", "errors": errors,
                      "processes": scope.evidence(), "raw": {}}
            for role, (_, files) in actors.items():
                if files["result"].exists():
                    result["raw"][role] = json.loads(files["result"].read_text())
            exit_scope(token)
        if errors:
            raise NetworkError("UDP preflight failed: " + "; ".join(errors))
        return result
