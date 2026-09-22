"""Linux-only isolated network namespace ownership for fixture scenarios.

This module owns network namespaces and their evidence only. Fixture binaries
remain owned by process_lifecycle; callers pass namespace_command() to it.
"""

from __future__ import annotations

from dataclasses import dataclass
import json
import os
from pathlib import Path
import platform
import re
import secrets
import shutil
import subprocess
from typing import Callable, Mapping, Sequence


class NetworkError(RuntimeError):
    """The fixture network could not be constructed or cleaned up safely."""


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str = ""
    stderr: str = ""


CommandRunner = Callable[[list[str]], CommandResult]


_DOWN_KERNEL_TUNNELS = frozenset(
    {"tunl0", "gre0", "gretap0", "erspan0", "ip_vti0", "ip6_vti0", "sit0", "ip6tnl0", "ip6gre0"}
)


def _caller_has_unshared_outer_namespaces() -> bool:
    try:
        return (
            os.stat("/proc/self/ns/net").st_ino != os.stat("/proc/1/ns/net").st_ino
            and os.stat("/proc/self/ns/mnt").st_ino != os.stat("/proc/1/ns/mnt").st_ino
        )
    except OSError:
        return False


def _subprocess_command(command: list[str]) -> CommandResult:
    try:
        completed = subprocess.run(
            command,
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=10,
        )
    except subprocess.TimeoutExpired as error:
        raise NetworkError(f"network command timed out: {' '.join(command)}") from error
    except OSError as error:
        raise NetworkError(f"could not start network command {' '.join(command)}: {error}") from error
    return CommandResult(completed.returncode, completed.stdout, completed.stderr)


class IsolatedNetwork:
    """A fresh outer netns containing only a bridge and two fixture peers."""

    _PREFIX = "fixture"
    _LABEL = "fixture"
    _KIND = "linux_isolated_fixture_netns"
    _error_type = NetworkError

    _ADDRESSES = {
        "client": "11.0.0.1",
        "server": "11.0.0.2",
    }
    _CLIENT_SECONDARY_ADDRESS = "11.0.0.3"
    _OUTER_LINKS = {
        "client": "c0",
        "server": "s0",
    }

    def __init__(
        self,
        include_client_secondary_address: bool = False,
        *,
        command_runner: CommandRunner = _subprocess_command,
        system: Callable[[], str] = platform.system,
        ip_lookup: Callable[[str], str | None] = shutil.which,
        outer_namespace_isolated: Callable[[], bool] = _caller_has_unshared_outer_namespaces,
        namespace_token: str | None = None,
    ):
        token = namespace_token or f"{os.getpid():x}{secrets.token_hex(4)}"
        if not re.fullmatch(r"[a-z0-9]{1,20}", token):
            raise ValueError("namespace_token must contain only lowercase letters and digits")

        self._command_runner = command_runner
        self._system = system
        self._ip_lookup = ip_lookup
        self._outer_namespace_isolated = outer_namespace_isolated
        self._ip: str | None = None
        self._roles = ["client", "server"]
        self._include_client_secondary_address = include_client_secondary_address
        self._namespaces = {"outer": f"{self._PREFIX}-o-{token}"}
        self._namespaces.update({"client": f"{self._PREFIX}-c-{token}", "server": f"{self._PREFIX}-s-{token}"})
        self._created_children: list[str] = []
        self._outer_created = False
        self._uncertain_namespaces: dict[str, str] = {}
        self._state = "new"
        self._isolation_verified = False
        self._commands: list[dict[str, object]] = []
        self._cleanup_failures: list[str] = []
        self._closed_tcp_ports: list[dict[str, object]] = []

    @property
    def namespaces(self) -> Mapping[str, str]:
        return dict(self._namespaces)

    @property
    def addresses(self) -> Mapping[str, tuple[str, ...]]:
        client = [self._ADDRESSES["client"]]
        if self._include_client_secondary_address:
            client.append(self._CLIENT_SECONDARY_ADDRESS)
        return {"client": tuple(client), "server": (self._ADDRESSES["server"],)}

    def namespace_command(self, role: str, command: Sequence[str | Path]) -> list[str]:
        """Return a command prefix for process_lifecycle.spawn_owned()."""
        if self._state != "ready":
            raise self._error_type("fixture network is not ready")
        if role not in self._roles:
            raise ValueError(f"unknown {self._LABEL} fixture role: {role}")
        assert self._ip is not None
        return [self._ip, "netns", "exec", self._namespaces[role], *(str(value) for value in command)]

    def assert_closed_tcp_port(self, role: str, port: int) -> None:
        """Observe both IP families inside the owned namespace, never on host."""
        if type(port) is not int or not 0 < port < 65536:
            raise ValueError("closed-port assertion requires a nonzero TCP port")
        ss = self._ip_lookup("ss")
        if not ss:
            raise self._error_type("closed-port assertion requires iproute2 'ss'")
        command = self.namespace_command(role, [ss, "-H", "-lnt", "sport", "=", f":{port}"])
        result = self._run_checked(command)
        if result.stdout.strip():
            raise self._error_type(f"negative control TCP port {port} is listening in {role}")
        self._closed_tcp_ports.append({"role": role, "port": port, "transport": "tcp", "listening": False})

    def setup(self) -> "IsolatedNetwork":
        if self._state == "ready":
            return self
        if self._state == "closed":
            raise self._error_type("closed fixture network cannot be reused")
        if self._state == "setting_up":
            raise self._error_type("fixture network setup is already in progress")

        self._state = "setting_up"
        try:
            self._require_linux_iproute()
            self._add_namespace("outer")
            self._inside("outer", "link", "set", "lo", "up")
            self._verify_isolated_namespace("outer", {"lo"})

            for role in self._roles:
                self._add_namespace(role)

            self._inside("outer", "link", "add", "br0", "type", "bridge")
            self._configure_bridge()
            self._inside("outer", "link", "set", "br0", "up")
            for role in self._roles:
                self._connect_role(role)

            self._verify_isolated_namespace(
                "outer", {"lo", "br0", *(self._OUTER_LINKS[role] for role in self._roles)}
            )
            for role in self._roles:
                self._verify_isolated_namespace(role, {"lo", "eth0"})
            self._isolation_verified = True
            self._state = "ready"
            return self
        except BaseException as error:
            self._state = "failed"
            cleanup_errors = self.close()
            self._attach_cleanup_failures(error, cleanup_errors)
            raise

    def close(self) -> list[str]:
        """Delete children before outer netns; retry only namespaces that remain."""
        errors = [self._uncertainty_message(role, reason) for role, reason in self._uncertain_namespaces.items()]
        remaining_children: list[str] = []
        for role in reversed(self._created_children):
            if not self._delete_namespace(self._namespaces[role], errors):
                remaining_children.append(role)
        self._created_children = list(reversed(remaining_children))

        if not self._created_children and not self._uncertain_namespaces and self._outer_created:
            if self._delete_namespace(self._namespaces["outer"], errors):
                self._outer_created = False

        self._cleanup_failures.extend(errors)
        if not self._created_children and not self._outer_created and not self._uncertain_namespaces:
            self._state = "closed"
        else:
            self._state = "failed"
        return errors

    def evidence(self) -> dict[str, object]:
        """Setup evidence only; this does not assert a protocol outcome."""
        return {
            "kind": self._KIND,
            "state": self._state,
            "outer_namespace": self._namespaces["outer"],
            "participants": [
                {"role": role, "namespace": self._namespaces[role], "addresses": list(self.addresses[role])}
                for role in self._roles
            ],
            "outer_network": {
                "bridge": "br0",
                "default_route": "absent" if self._isolation_verified else "not_verified",
                "external_links": "absent" if self._isolation_verified else "not_verified",
            },
            "commands": list(self._commands),
            "cleanup_uncertainty": [
                {"role": role, "namespace": self._namespaces[role], "reason": reason}
                for role, reason in sorted(self._uncertain_namespaces.items())
            ],
            "cleanup_failures": list(self._cleanup_failures),
            "closed_tcp_ports": list(self._closed_tcp_ports),
        }

    def __enter__(self) -> "IsolatedNetwork":
        return self.setup()

    def __exit__(self, exc_type, exc_value, traceback) -> bool:
        errors = self.close()
        if not errors:
            return False
        if exc_value is not None:
            self._attach_cleanup_failures(exc_value, errors)
            return False
        raise self._error_type(f"fixture network cleanup failed: {'; '.join(errors)}")

    def _require_linux_iproute(self) -> None:
        system = self._system()
        if system != "Linux":
            raise self._error_type(f"isolated {self._LABEL} fixture network requires Linux, not {system}")
        self._ip = self._ip_lookup("ip")
        if not self._ip:
            raise self._error_type(
                f"isolated {self._LABEL} fixture network requires iproute2 ('ip'); refusing to fall back to host networking"
            )
        if not self._outer_namespace_isolated():
            raise self._error_type(
                f"isolated {self._LABEL} fixture network requires caller unshare --net --mount; refusing the shared host namespace"
            )
        self._verify_caller_outer_network()

    def _verify_caller_outer_network(self) -> None:
        assert self._ip is not None
        routes = self._json_output([self._ip, "-j", "route", "show", "default"], "caller default-route inspection")
        if not isinstance(routes, list) or routes:
            raise self._error_type("refusing non-isolated caller outer network: default route is present")
        ipv6_routes = self._json_output(
            [self._ip, "-j", "-6", "route", "show", "default"], "caller IPv6 default-route inspection"
        )
        if not isinstance(ipv6_routes, list) or ipv6_routes:
            raise self._error_type("refusing non-isolated caller outer network: IPv6 default route is present")
        links = self._link_records([self._ip, "-j", "link", "show"], "caller link inspection")
        self._validate_isolated_links("caller outer network", links, {"lo"})

    def _connect_role(self, role: str) -> None:
        outer_link = self._OUTER_LINKS[role]
        namespace = self._namespaces[role]
        self._inside(
            "outer", "link", "add", outer_link, "type", "veth", "peer", "name", "eth0", "netns", namespace
        )
        self._inside("outer", "link", "set", outer_link, "master", "br0")
        self._inside("outer", "link", "set", outer_link, "up")
        self._inside(role, "link", "set", "lo", "up")
        self._configure_role_addresses(role)
        self._inside(role, "link", "set", "eth0", "up")

    def _configure_bridge(self) -> None:
        pass

    def _configure_role_addresses(self, role: str) -> None:
        self._inside(role, "addr", "add", f"{self._ADDRESSES[role]}/24", "dev", "eth0")
        if role == "client" and self._include_client_secondary_address:
            self._inside(role, "addr", "add", f"{self._CLIENT_SECONDARY_ADDRESS}/24", "dev", "eth0")

    def _verify_isolated_namespace(self, role: str, expected_links: set[str]) -> None:
        namespace = self._namespaces[role]
        routes = self._json_output(
            self._inside_command(namespace, "-j", "route", "show", "default"), "default-route inspection"
        )
        if not isinstance(routes, list) or routes:
            raise self._error_type(
                f"refusing non-isolated outer network: namespace {namespace} has a default route"
            )
        ipv6_routes = self._json_output(
            self._inside_command(namespace, "-j", "-6", "route", "show", "default"),
            "IPv6 default-route inspection",
        )
        if not isinstance(ipv6_routes, list) or ipv6_routes:
            raise self._error_type(
                f"refusing non-isolated outer network: namespace {namespace} has an IPv6 default route"
            )
        links = self._link_records(self._inside_command(namespace, "-j", "link", "show"), "link inspection")
        self._validate_isolated_links(f"namespace {namespace}", links, expected_links)

    @classmethod
    def _validate_isolated_links(
        cls, scope: str, links: list[dict[str, object]], expected_links: set[str]
    ) -> None:
        actual_links = {link["ifname"] for link in links}
        unexpected = sorted(actual_links - expected_links - _DOWN_KERNEL_TUNNELS)
        missing = sorted(expected_links - actual_links)
        if unexpected or missing:
            raise cls._error_type(
                f"refusing non-isolated {scope}: links differ; "
                f"unexpected={unexpected}; missing={missing}"
            )
        for link in links:
            name = link["ifname"]
            if name not in _DOWN_KERNEL_TUNNELS:
                continue
            flags = link.get("flags")
            if not isinstance(flags, list) or not all(isinstance(flag, str) for flag in flags):
                raise cls._error_type(
                    f"refusing non-isolated {scope}: tunnel {name} is not verifiably DOWN"
                )
            if link.get("operstate") != "DOWN" or "UP" in flags:
                raise cls._error_type(
                    f"refusing non-isolated {scope}: tunnel {name} is not verifiably DOWN"
                )

    def _link_records(self, command: list[str], purpose: str) -> list[dict[str, object]]:
        links = self._json_output(command, purpose)
        if not isinstance(links, list) or any(
            not isinstance(link, dict) or not isinstance(link.get("ifname"), str) for link in links
        ):
            raise self._error_type(f"invalid JSON from {purpose}: {' '.join(command)}")
        return links

    def _json_output(self, command: list[str], purpose: str) -> object:
        result = self._run_checked(command)
        try:
            return json.loads(result.stdout)
        except json.JSONDecodeError as error:
            raise self._error_type(f"invalid JSON from {purpose}: {' '.join(command)}") from error

    def _add_namespace(self, role: str) -> None:
        assert self._ip is not None
        namespace = self._namespaces[role]
        if self._namespace_exists(namespace):
            raise self._error_type(f"fixture network namespace collision: {namespace} already exists")

        command = [self._ip, "netns", "add", namespace]
        try:
            result = self._invoke(command)
        except BaseException as error:
            self._uncertain_namespaces[role] = (
                "netns add raised before ownership could be established; refusing existence-based ownership inference"
            )
            raise
        if result.returncode != 0:
            raise self._error_type(self._command_failure(command, result))
        self._mark_namespace_owned(role)

    def _mark_namespace_owned(self, role: str) -> None:
        self._uncertain_namespaces.pop(role, None)
        if role == "outer":
            self._outer_created = True
        elif role not in self._created_children:
            self._created_children.append(role)

    def _namespace_exists(self, namespace: str) -> bool:
        assert self._ip is not None
        result = self._run_checked([self._ip, "netns", "list"])
        return any(line.split(maxsplit=1)[0] == namespace for line in result.stdout.splitlines() if line.strip())

    @staticmethod
    def _uncertainty_message(role: str, reason: str) -> str:
        return f"namespace ownership uncertain for {role}: {reason}"

    @staticmethod
    def _attach_cleanup_failures(error: BaseException, failures: list[str]) -> None:
        if failures and hasattr(error, "add_note"):
            error.add_note(f"fixture network cleanup failures: {'; '.join(failures)}")

    def _delete_namespace(self, namespace: str, errors: list[str]) -> bool:
        assert self._ip is not None
        pids_command = [self._ip, "netns", "pids", namespace]
        try:
            pids_result = self._invoke(pids_command)
        except BaseException as error:
            errors.append(f"namespace PID inspection failed to start for {namespace}: {error}")
            return False
        if pids_result.returncode != 0:
            if self._namespace_is_missing(pids_result.stderr):
                return True
            errors.append(self._command_failure(pids_command, pids_result))
            return False
        pids = pids_result.stdout.split()
        if any(not pid.isdecimal() for pid in pids):
            errors.append(f"namespace PID inspection returned invalid data for {namespace}: {pids_result.stdout!r}")
            return False
        if pids:
            errors.append(f"refusing namespace cleanup while owned processes remain in {namespace}: {', '.join(pids)}")
            return False
        command = [self._ip, "netns", "del", namespace]
        try:
            result = self._invoke(command)
        except BaseException as error:
            errors.append(f"namespace cleanup command failed to start for {namespace}: {error}")
            return False
        if result.returncode == 0 or self._namespace_is_missing(result.stderr):
            return True
        errors.append(self._command_failure(command, result))
        return False

    @staticmethod
    def _namespace_is_missing(stderr: str) -> bool:
        lower = stderr.lower()
        return "no such file or directory" in lower or "cannot find device" in lower

    def _inside(self, role: str, *arguments: str) -> CommandResult:
        return self._run_checked(self._inside_command(self._namespaces[role], *arguments))

    def _inside_command(self, namespace: str, *arguments: str) -> list[str]:
        assert self._ip is not None
        return [self._ip, "netns", "exec", namespace, self._ip, *arguments]

    def _run_checked(self, command: list[str]) -> CommandResult:
        result = self._invoke(command)
        if result.returncode != 0:
            raise self._error_type(self._command_failure(command, result))
        return result

    def _invoke(self, command: list[str]) -> CommandResult:
        record: dict[str, object] = {"command": list(command)}
        try:
            result = self._command_runner(command)
        except BaseException as error:
            record["exception"] = {"type": type(error).__name__, "message": str(error)}
            self._commands.append(record)
            raise
        record.update({"returncode": result.returncode, "stdout": result.stdout, "stderr": result.stderr})
        self._commands.append(record)
        return result

    @staticmethod
    def _command_failure(command: Sequence[str], result: CommandResult) -> str:
        details = result.stderr.strip() or result.stdout.strip() or f"exit code {result.returncode}"
        lowered = details.lower()
        capability_hint = ""
        if "operation not permitted" in lowered or "permission denied" in lowered:
            capability_hint = " (requires root or CAP_SYS_ADMIN and CAP_NET_ADMIN)"
        return f"network command failed{capability_hint}: {' '.join(command)}: {details}"
