#!/usr/bin/env python3

import argparse
import json
import pathlib
import re


ENTRY = re.compile(
    r"FORGE_CONTRACT_INTRINSIC\(\s*(\d+)\s*,\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*,\s*"
    r"([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*,\s*([A-Za-z_]\w*)\s*,\s*"
    r"([A-Za-z_]\w*)\s*,\s*([^,]+?)\s*,\s*\((.*?)\)\s*\)",
    re.DOTALL,
)

CAPABILITIES = {
    "core",
    "database",
    "privileged",
    "crypto_ext",
    "bls",
    "call",
    "instant_finality",
    "runtime",
    "expression_v1",
}

PUBLIC_INTRINSIC_COUNT = 152
RUNTIME_INTRINSIC_COUNT = 48
RUNTIME_HEADER = "runtime"
EXTENSION_COUNTS = {"expression_v1": 3}

EOSIO_HEADERS = (
    "action",
    "call",
    "chain",
    "crypto",
    "crypto_bls_ext",
    "crypto_ext",
    "db",
    "instant_finality",
    "permission",
    "print",
    "privileged",
    "system",
    "transaction",
)


def wasm_type(cpp_type: str) -> str:
    normalized = " ".join(cpp_type.split())
    if "*" in normalized:
        return "i32"
    if normalized == "float":
        return "f32"
    if normalized == "double":
        return "f64"
    if normalized in {
        "bool",
        "forge_contract_size_t",
        "std::uint32_t",
        "std::int32_t",
    }:
        return "i32"
    if normalized in {"std::uint64_t", "std::int64_t"}:
        return "i64"
    raise ValueError(f"intrinsic type has no WASM mapping: {normalized}")


def wasm_parameters(parameters: str) -> list[str]:
    normalized = " ".join(parameters.split())
    if not normalized:
        return []

    result = []
    for parameter in normalized.split(","):
        declaration = parameter.strip()
        type_name, _, _ = declaration.rpartition(" ")
        if not type_name:
            raise ValueError(f"intrinsic parameter has no name: {declaration}")
        result.append(wasm_type(type_name))
    return result


def c_type(value: str) -> str:
    normalized = " ".join(value.split())
    replacements = (
        ("unsigned __int128", "uint128_t"),
        ("__int128", "int128_t"),
        ("forge_contract_size_t", "size_t"),
        ("std::uint32_t", "uint32_t"),
        ("std::int32_t", "int32_t"),
        ("std::uint64_t", "uint64_t"),
        ("std::int64_t", "int64_t"),
    )
    for source, target in replacements:
        normalized = normalized.replace(source, target)
    normalized = re.sub(r"(?<!struct )\bcapi_checksum(160|256|512)\b", r"struct capi_checksum\1", normalized)
    return normalized


def write_types_header(source: pathlib.Path, path: pathlib.Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(source.read_text(encoding="utf-8"), encoding="utf-8")


def write_c_header(path: pathlib.Path, entries: list[dict], *, eosio: bool = False) -> None:
    types_header = "eosio/types.h" if eosio else "forge/contract/types.h"
    lines = [
        "#pragma once",
        "",
        f"#include <{types_header}>",
        "",
        "#include <stddef.h>",
        "",
        "#if defined(__clang__) && __has_attribute(import_module)",
        "#define FORGE_CONTRACT_IMPORT(module, name) \\",
        "   __attribute__((import_module(module), import_name(name)))",
        "#else",
        "#define FORGE_CONTRACT_IMPORT(module, name)",
        "#endif",
        "",
    ]
    lines.extend(["#ifdef __cplusplus", 'extern "C" {', "#endif", ""])
    for entry in entries:
        result = c_type(entry["result"])
        parameters = c_type(entry["parameters"]) or "void"
        noreturn = " __attribute__((noreturn))" if entry["identifier"] == "eosio_exit" else ""
        lines.append(
            f'{result} {entry["identifier"]}({parameters}){noreturn} '
            f'FORGE_CONTRACT_IMPORT("{entry["module"]}", "{entry["import"]}");'
        )
    lines.extend(
        [
            "",
            "#ifdef __cplusplus",
            "}",
            "#endif",
            "",
            "#undef FORGE_CONTRACT_IMPORT",
            "",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def write_internal_header(
    path: pathlib.Path,
    entries: list[dict],
    namespace: str,
    excluded: set[str] | None = None,
    global_alias: bool = False,
) -> None:
    excluded = excluded or set()
    visible_entries = [entry for entry in entries if entry["identifier"] not in excluded]
    lines = [
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <stdint.h>",
        "",
        "struct capi_checksum160;",
        "struct capi_checksum256;",
        "struct capi_checksum512;",
        "",
    ]
    if global_alias:
        lines.extend(['extern "C" {', ""])
    else:
        lines.extend(
            [
                f"namespace {namespace} {{",
                "using uint128_t = unsigned __int128;",
                "using int128_t = __int128;",
                "",
            ]
        )
    for entry in visible_entries:
        result = c_type(entry["result"])
        parameters = c_type(entry["parameters"]) or "void"
        if global_alias:
            result = re.sub(r"\buint128_t\b", "unsigned __int128", result)
            result = re.sub(r"\bint128_t\b", "__int128", result)
            parameters = re.sub(r"\buint128_t\b", "unsigned __int128", parameters)
            parameters = re.sub(r"\bint128_t\b", "__int128", parameters)
        parameters = re.sub(r"struct capi_checksum(160|256|512)", r"::capi_checksum\1", parameters)
        noreturn = " __attribute__((noreturn))" if entry["identifier"] == "eosio_exit" else ""
        lines.append(
            f'{result} {entry["identifier"]}({parameters}){noreturn} '
            f'__attribute__((import_module("{entry["module"]}"), import_name("{entry["import"]}")));'
        )
    if global_alias:
        lines.extend(["", "}"])
    if global_alias:
        lines.extend(["", f"namespace {namespace} {{"])
        lines.extend(f"using ::{entry['identifier']};" for entry in visible_entries)
        lines.extend([f"}} // namespace {namespace}", ""])
    else:
        lines.extend([f"}} // namespace {namespace}", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="utf-8")


def parse_registry(path: pathlib.Path) -> list[dict]:
    source = path.read_text(encoding="utf-8").replace("\\\n", " ")
    entries = []
    for match in ENTRY.finditer(source):
        (
            version,
            capability,
            header,
            protocol_feature,
            identifier,
            module,
            import_name,
            result,
            parameters,
        ) = match.groups()
        normalized_result = " ".join(result.split())
        normalized_parameters = " ".join(parameters.split())
        entries.append(
            {
                "interface_version": int(version),
                "capability": capability,
                "header": header,
                "protocol_feature": None if protocol_feature == "none" else protocol_feature,
                "identifier": identifier,
                "module": module,
                "import": import_name,
                "result": normalized_result,
                "parameters": normalized_parameters,
                "wasm_result": None if normalized_result == "void" else wasm_type(normalized_result),
                "wasm_parameters": wasm_parameters(normalized_parameters),
            }
        )

    expected_count = PUBLIC_INTRINSIC_COUNT + RUNTIME_INTRINSIC_COUNT + sum(EXTENSION_COUNTS.values())
    if len(entries) != expected_count:
        raise SystemExit(f"intrinsic registry must contain {expected_count} declarations, found {len(entries)}")
    if len({entry["identifier"] for entry in entries}) != len(entries):
        raise SystemExit("intrinsic registry contains duplicate identifiers")
    if {entry["capability"] for entry in entries} - CAPABILITIES:
        raise SystemExit("intrinsic registry contains an unknown capability")
    if {entry["header"] for entry in entries} - (set(EOSIO_HEADERS) | {RUNTIME_HEADER} | EXTENSION_COUNTS.keys()):
        raise SystemExit("intrinsic registry contains an unknown EOSIO header")
    if sum(entry["header"] in EOSIO_HEADERS for entry in entries) != PUBLIC_INTRINSIC_COUNT:
        raise SystemExit(f"intrinsic registry must contain {PUBLIC_INTRINSIC_COUNT} public declarations")
    if sum(entry["header"] == RUNTIME_HEADER for entry in entries) != RUNTIME_INTRINSIC_COUNT:
        raise SystemExit(f"intrinsic registry must contain {RUNTIME_INTRINSIC_COUNT} runtime declarations")
    for capability, count in EXTENSION_COUNTS.items():
        extension = [entry for entry in entries if entry["capability"] == capability]
        if len(extension) != count or any(entry["header"] != capability for entry in extension):
            raise SystemExit(f"intrinsic capability {capability} must contain {count} declarations")
    return entries


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--registry", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--include-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()

    entries = parse_registry(args.registry)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "interface_version": max(entry["interface_version"] for entry in entries),
                "capabilities": sorted(CAPABILITIES),
                "imports": entries,
            },
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )

    types_header = pathlib.Path(__file__).parent / "types.h.in"
    write_types_header(types_header, args.include_dir / "forge/contract/types.h")
    eosio_types = args.include_dir / "eosio/types.h"
    eosio_types.parent.mkdir(parents=True, exist_ok=True)
    eosio_types.write_text("#pragma once\n\n#include <forge/contract/types.h>\n", encoding="utf-8")
    public_entries = [entry for entry in entries if entry["header"] != RUNTIME_HEADER]
    write_c_header(args.include_dir / "forge/contract/intrinsics.h", public_entries)
    for header in EOSIO_HEADERS:
        write_c_header(
            args.include_dir / f"eosio/{header}.h",
            [entry for entry in entries if entry["header"] == header],
            eosio=True,
        )
    write_internal_header(
        args.include_dir / "forge/contract/internal/intrinsics.hpp", entries, "forge::contract::internal"
    )
    for header in EOSIO_HEADERS:
        header_entries = [entry for entry in entries if entry["header"] == header]
        write_internal_header(
            args.include_dir / f"eosio/internal/{header}.hpp",
            header_entries,
            "eosio::internal_use_do_not_use",
            {"set_action_return_value"},
            header == "db",
        )


if __name__ == "__main__":
    main()
