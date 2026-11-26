#!/usr/bin/env python3

from pathlib import Path
import re
import sys


def require(text: str, needle: str, source: Path) -> None:
    if needle not in text:
        raise SystemExit(f"{source}: missing required State API 3.1 surface: {needle}")


def forbid(text: str, needle: str, source: Path) -> None:
    if needle in text:
        raise SystemExit(f"{source}: removed State API surface returned: {needle}")


def forbid_pattern(text: str, pattern: str, source: Path, label: str) -> None:
    if re.search(pattern, text):
        raise SystemExit(f"{source}: removed State API surface returned: {label}")


def main() -> int:
    root = Path(sys.argv[1]).resolve()
    protocol_path = root / "libraries/chain/protocol/include/forge/chain/protocol/state_query.cppm"
    table_path = root / "libraries/chain/protocol/include/forge/chain/protocol/table.cppm"
    state_path = root / "libraries/chain/api/include/forge/chain/api/state.cppm"
    exceptions_path = root / "libraries/chain/api/include/forge/chain/api/exceptions.cppm"
    openapi_path = root / "libraries/api/http/openapi.cpp"
    protocol_cmake_path = root / "libraries/chain/protocol/CMakeLists.txt"
    package_dir = root / "tests/package_chain_api_component"
    package_p2p_paths = (
        package_dir / "read_e2e_p2p_state.cpp",
        package_dir / "read_e2e_p2p_state_client.cpp",
        package_dir / "read_e2e_p2p_state_server.cpp",
    )
    package_cmake_path = package_dir / "CMakeLists.txt"
    package_protocol_path = root / "tests/package_chain_protocol_component/main.cpp"

    protocol = protocol_path.read_text(encoding="utf-8")
    table = table_path.read_text(encoding="utf-8")
    state = state_path.read_text(encoding="utf-8")
    exceptions = exceptions_path.read_text(encoding="utf-8")
    openapi = openapi_path.read_text(encoding="utf-8")
    protocol_cmake = protocol_cmake_path.read_text(encoding="utf-8")
    package_cmake = package_cmake_path.read_text(encoding="utf-8")
    package_p2p = "\n".join(path.read_text(encoding="utf-8") for path in package_p2p_paths)
    package_files = sorted((*package_dir.glob("*.cpp"), *package_dir.glob("*.cppm"), *package_dir.glob("*.hpp")))
    package = "\n".join(path.read_text(encoding="utf-8") for path in package_files)
    package_protocol = package_protocol_path.read_text(encoding="utf-8")

    for needle in (
        "struct account_request : account_selector",
        "BOOST_DESCRIBE_STRUCT(account_request, (account_selector), (anchor, finality_from, audit))",
        "full_account account;",
        "struct code_request : account_selector",
        "forge::chain::protocol::code code;",
        "std::optional<bytes> wasm;",
        "std::optional<bytes> abi;",
        "struct permission_links_request : account_selector",
        "struct permission_links_response : audited_response",
        "std::vector<permission_link> links;",
        "std::optional<account_authority> authority;",
        "std::vector<table> tables;",
        "forge::chain::protocol::currency_stats stats;",
        "std::optional<time_point> lower_bound;",
        "std::optional<time_point> upper_bound;",
        "std::vector<generated_transaction> transactions;",
        "struct authorizers_request",
        "struct authorizers_response : audited_response",
        "std::optional<bytes> cursor;",
        "std::optional<bytes> next;",
    ):
        require(protocol, needle, protocol_path)

    for symbol in (
        "struct state_point_request",
        "struct state_point_response",
        "struct state_range_request",
        "struct state_range_response",
        "struct state_changes_request",
        "struct state_changes_response",
        "struct state_changes_cursor",
        "struct state_mutation",
        "struct state_change_range",
        "struct state_change_batch",
        "struct key_range",
        "struct account_permission",
        "struct account_state",
        "struct scheduled_transaction",
        "struct table_scope_row",
        "struct authorizers_cursor",
        "enum class authorizer_source",
        "include_code",
        "bool json",
        "time_limit_ms",
        "forge::variant stats",
        "std::string more",
        "raw_abi",
        "get_object",
    ):
        forbid(protocol, symbol, protocol_path)

    forbid_pattern(protocol, r"struct\s+\w*cursor\b", protocol_path, "structured cursor type")
    forbid_pattern(
        protocol,
        r"std::(?:optional<)?(?:std::)?string[^;\n]*\b(?:cursor|next|more)\b",
        protocol_path,
        "text cursor or continuation",
    )
    forbid_pattern(
        protocol,
        r"std::optional<[^>\n]*cursor[^>\n]*>\s+(?:cursor|next)\b",
        protocol_path,
        "structured cursor field",
    )
    forbid_pattern(protocol, r"\bforge::variant\s+\w+\s*;", protocol_path, "public variant field")
    for line in protocol.splitlines():
        if re.search(r"\b(?:cursor|next|more)\s*;", line) and "std::optional<bytes>" not in line:
            raise SystemExit(
                f"{protocol_path}: State API continuation is not optional opaque bytes: {line.strip()}"
            )

    require(table, "struct table {", table_path)
    require(table, "BOOST_DESCRIBE_STRUCT(table, (), (id, code, scope, table, payer, count))", table_path)
    forbid(table, "table_record", table_path)
    forbid_pattern(table, r"\busing\s+table\s*=", table_path, "table compatibility alias")

    require(state, 'FORGE_API_CONTRACT("forge.chain.api.state", 3, 1)', state_path)
    require(state, "FORGE_API_METHOD_TYPED(call, ::forge::chain::protocol::contract_call_request", state_path)
    require(state, 'FORGE_HTTP_POST(call, "/v1/chain/state/call"', state_path)
    require(state, "FORGE_API_METHOD_TYPED(get_permission_links", state_path)
    require(state, "FORGE_HTTP_GET(get_permission_links", state_path)
    require(state, "/v1/chain/state/accounts?id={id}&key={key}", state_path)
    require(state, "/v1/chain/state/codes?id={id}&key={key}", state_path)
    require(state, "/v1/chain/state/permission-links?id={id}&key={key}", state_path)
    require(state, "FORGE_HTTP_POST(get_account_changes", state_path)
    require(state, "FORGE_HTTP_POST(get_table_changes", state_path)
    for method in ("get_point", "get_range", "get_changes"):
        forbid(state, method, state_path)
    for route in (
        '"/v1/chain/state/point"',
        '"/v1/chain/state/range"',
        '"/v1/chain/state/changes"',
        '"/v1/chain/state/accounts/{account}"',
    ):
        forbid(state, route, state_path)
    forbid(state, "include_code", state_path)
    forbid(state, "json={json}", state_path)
    forbid(state, "get_object", state_path)

    require(openapi, '("style", "form")("explode", true)', openapi_path)
    forbid(openapi, "x-forge-query-schema", openapi_path)

    require(exceptions, "not_found = 16", exceptions_path)
    require(exceptions, "using not_found", exceptions_path)
    require(exceptions, "invalid_request = 1", exceptions_path)
    require(exceptions, "unavailable = 11", exceptions_path)
    require(exceptions, "resource_exhausted = 12", exceptions_path)

    for needle in (
        "protocol::full_account",
        "protocol::account_authority",
        "protocol::currency_stats",
        "protocol::generated_transaction",
        "chain_api::state::ref().major == 3U",
        ".major = 3, .min_revision = 0",
    ):
        require(package, needle, package_dir)
    require(package, "run_p2p_state_e2e", package_dir)
    require(package, "std::optional<protocol::bytes>", package_dir)
    for needle in (
        "import forge.chain.api.state;",
        "import forge.chain.api.limits;",
        "export_api<chain_api::state>(chain_api::state::ref())",
        'resolver->resolve(server_peer, {.id = {"forge.chain.api.state"}, .major = 3, .min_revision = 0})',
        "connection.get_remote_api<chain_api::state>()",
        "protocol::audit_mode::required",
        "oversized_request_rejected",
    ):
        require(package_p2p, needle, package_dir)
    for api in ("admin", "submission", "transaction"):
        forbid(package_p2p, f"import forge.chain.api.{api};", package_dir)
        forbid(package_p2p, f"export_api<chain_api::{api}>", package_dir)
    require(protocol_cmake, "state_query.cpp", protocol_cmake_path)
    for source in (
        "p2p_runtime.cpp",
        "read_e2e.cpp",
        "read_e2e_p2p_state.cpp",
        "read_e2e_p2p_state_client.cpp",
        "read_e2e_p2p_state_server.cpp",
        "read_fixture.cpp",
    ):
        require(package_cmake, source, package_cmake_path)

    require(package_protocol, "account_response{}.account", package_protocol_path)
    require(package_protocol, "authorizers_request{}.cursor", package_protocol_path)
    require(package_protocol, "authorizers_response{}.next", package_protocol_path)
    forbid(package_protocol, "account_state", package_protocol_path)
    forbid(package_protocol, "table_record", package_protocol_path)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
