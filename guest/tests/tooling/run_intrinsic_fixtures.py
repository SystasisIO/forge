#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import pathlib
from typing import Dict, List, Optional, Tuple


def read_golden(path: pathlib.Path) -> Dict[str, Tuple[List[str], Optional[str]]]:
    result = {}
    for source_line in path.read_text(encoding="utf-8").splitlines():
        line = source_line.strip()
        if not line or line.startswith("#"):
            continue
        name, parameters, return_type = line.split("|")
        if name in result:
            raise RuntimeError(f"duplicate golden intrinsic: {name}")
        result[name] = (parameters.split(",") if parameters else [], return_type or None)
    return result


def read_full_golden(path: pathlib.Path) -> dict:
    result = {}
    for source_line in path.read_text(encoding="utf-8").splitlines():
        line = source_line.strip()
        if not line or line.startswith("#"):
            continue
        name, capability, header, feature, parameters, return_type = line.split("|")
        if name in result:
            raise RuntimeError(f"duplicate full golden intrinsic: {name}")
        result[name] = {
            "capability": capability,
            "header": header,
            "protocol_feature": feature or None,
            "wasm_parameters": parameters.split(",") if parameters else [],
            "wasm_result": return_type or None,
        }
    return result


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", required=True, type=pathlib.Path)
    parser.add_argument("--golden", required=True, type=pathlib.Path)
    parser.add_argument("--full-golden", required=True, type=pathlib.Path)
    parser.add_argument("--eosio-db-header", required=True, type=pathlib.Path)
    parser.add_argument("--cdt-fixture", required=True, type=pathlib.Path)
    args = parser.parse_args()

    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    imports = manifest["imports"]
    names = [entry["import"] for entry in imports]
    database = {entry["import"]: entry for entry in imports if entry["import"].startswith("db_")}
    golden = read_golden(args.golden)
    full_golden = read_full_golden(args.full_golden)

    assert manifest["schema_version"] == 1
    assert manifest["interface_version"] == 1
    assert len(imports) == 203
    assert len(names) == len(set(names))
    assert len(database) == 60
    assert database.keys() == golden.keys()
    baseline_imports = [entry for entry in imports if entry["capability"] != "expression_v1"]
    extensions = [entry for entry in imports if entry["capability"] == "expression_v1"]
    assert {entry["import"] for entry in baseline_imports} == set(full_golden)
    assert set(manifest["capabilities"]) == {
        "bls",
        "call",
        "core",
        "crypto_ext",
        "database",
        "instant_finality",
        "privileged",
        "runtime",
        "expression_v1",
    }

    public_imports = [entry for entry in baseline_imports if entry["capability"] != "runtime"]
    runtime_imports = [entry for entry in imports if entry["capability"] == "runtime"]
    assert len(public_imports) == 152
    assert len(runtime_imports) == 48
    assert len(extensions) == 3
    assert {entry["import"] for entry in extensions} == {
        "expression_compile_v1", "expression_import_v1", "expression_evaluate_v1"
    }
    for entry in extensions:
        assert entry["interface_version"] == 1
        assert entry["identifier"] == entry["import"]
        assert entry["module"] == "env"
        assert entry["header"] == "expression_v1"
        assert entry["protocol_feature"] is None
        assert entry["wasm_parameters"] == ["i32"] * 4
        assert entry["wasm_result"] == "i32"

    for name, entry in database.items():
        expected_parameters, expected_result = golden[name]
        assert entry["interface_version"] == 1
        assert entry["identifier"] == name
        assert entry["module"] == "env"
        assert entry["capability"] == "database"
        assert entry["header"] == "db"
        assert entry["protocol_feature"] is None
        assert entry["wasm_parameters"] == expected_parameters
        assert entry["wasm_result"] == expected_result

    for entry in baseline_imports:
        expected = full_golden[entry["import"]]
        for field, value in expected.items():
            assert entry[field] == value, f"{entry['import']} has unexpected {field}"

    eosio_header = args.eosio_db_header.read_text(encoding="utf-8")
    for name in database:
        assert f" {name}(" in eosio_header
    assert " require_auth(" not in eosio_header

    fixture = args.cdt_fixture.read_bytes()
    assert hashlib.sha256(fixture).hexdigest() == "82683e6ab0732382df65266f3cc24428dab6dd658d37dc28457976431624f260"
    donor_root = os.environ.get("FORGE_CONTRACT_CDT_DONOR")
    if donor_root:
        donor = pathlib.Path(donor_root) / "tests/unit/test_contracts/capi/db.c"
        assert fixture == donor.read_bytes()


if __name__ == "__main__":
    main()
