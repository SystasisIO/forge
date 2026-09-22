"""AutoNAT profile over shared namespace ownership; evidence/API stay unchanged."""

from __future__ import annotations

import platform
import shutil
from typing import Callable

from isolated_network import (
    CommandResult, CommandRunner, IsolatedNetwork, NetworkError,
    _caller_has_unshared_outer_namespaces, _subprocess_command as _run_command,
)


class AutonatNetworkError(NetworkError):
    """The fixture network could not be constructed or cleaned up safely."""


def _subprocess_command(command: list[str]) -> CommandResult:
    try:
        return _run_command(command)
    except NetworkError as error:
        raise AutonatNetworkError(str(error)) from error


class IsolatedAutonatNetwork(IsolatedNetwork):
    _PREFIX = "autonat"
    _LABEL = "AutoNAT"
    _KIND = "linux_isolated_autonat_netns"
    _error_type = AutonatNetworkError

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
        super().__init__(
            include_client_secondary_address,
            command_runner=command_runner, system=system, ip_lookup=ip_lookup,
            outer_namespace_isolated=outer_namespace_isolated, namespace_token=namespace_token,
        )
