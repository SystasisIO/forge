#!/usr/bin/env python3

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys


PASS_FIXTURES = {
    "action_results_test": "action_results_test",
    "aliased_type_variant_template_arg": "aliased_type_variant_template_arg",
    "nested_container": "nested_container",
    "ricardian_contract_test": "ricardian_contract_test",
    "singleton_contract": "singleton_contract",
    "struct_base_typedefd": "struct_base_typedefd",
    "sync_call_test": "sync_call_test",
    "tagged_number_test": "tagged_number_test",
    "using_std_array": "using_std_array",
}

FAIL_FIXTURES = {
    "empty_contract": "hello",
    "empty_contract_with_other_contract": "hello",
    "wrong_contract_name": "nothello",
}


def invoke(
    args,
    contract,
    source,
    output,
    *,
    succeeds=True,
    additional_sources=(),
    ricardian_contracts=None,
    ricardian_clauses=None,
    bare_outputs=False,
    error_contains=None,
    compiler_arguments=(),
):
    output.mkdir(parents=True, exist_ok=True)
    abi = output / f"{source.stem}.abi"
    dispatch = output / f"{source.stem}.dispatcher.cpp"
    abi_argument = abi.name if bare_outputs else abi
    dispatch_argument = dispatch.name if bare_outputs else dispatch
    command = [
        str(args.abigen),
        "--contract",
        contract,
        "--abi",
        str(abi_argument),
        "--dispatch",
        str(dispatch_argument),
        "--attribute-plugin",
        str(args.plugin),
        "--sysroot",
        str(args.sysroot),
        "--sdk-include",
        str(args.include),
        "--sdk-include",
        str(args.modules),
    ]
    module_dirs = sorted(
        path
        for path in (args.build_dir / "CMakeFiles").glob("*.dir")
        if any(path.glob("*.pcm"))
    )
    if not module_dirs:
        raise RuntimeError(f"no prebuilt module directories found under {args.build_dir}")
    for module_dir in module_dirs:
        command.extend(("--module-path", str(module_dir)))
    if ricardian_contracts is not None:
        command.extend(("--ricardian-contracts", str(ricardian_contracts)))
    if ricardian_clauses is not None:
        command.extend(("--ricardian-clauses", str(ricardian_clauses)))
    command.extend(f"--compiler-argument={argument}" for argument in compiler_arguments)
    source_wrappers = []
    for index, _ in enumerate(additional_sources, start=1):
        wrapper = output / f"{source.stem}.source-{index}.cpp"
        source_wrappers.append(wrapper)
        command.extend(("--source-wrapper", str(wrapper)))
    command.extend((str(source), *(str(item) for item in additional_sources)))

    result = subprocess.run(
        command,
        cwd=output if bare_outputs else None,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if succeeds and result.returncode != 0:
        raise RuntimeError(f"abigen failed for {source.name}:\n{result.stdout}{result.stderr}")
    if not succeeds and result.returncode == 0:
        raise RuntimeError(f"abigen unexpectedly accepted {source.name}")
    if not succeeds:
        diagnostics = result.stdout + result.stderr
        if error_contains is not None and error_contains not in diagnostics:
            raise RuntimeError(
                f"abigen rejected {source.name} without expected diagnostic {error_contains!r}:\n{diagnostics}"
            )
        return None
    return json.loads(abi.read_text(encoding="utf-8"))


def by_name(values):
    return {value["name"]: value for value in values}


def check_features(abi):
    assert abi["version"] == "eosio::abi/1.3"
    assert {entry["new_type_name"]: entry["type"] for entry in abi["types"]} == {
        "B_map_string_string_E": "pair_string_string[]",
        "B_vector_int32_E": "int32[]",
        "base_alias": "base_value",
        "my_account": "name",
        "signed_int": "varint32",
        "str": "string",
        "unsigned_int": "varuint32",
    }

    structs = by_name(abi["structs"])
    assert structs["derived_value"]["base"] == "base_alias"
    assert structs["tagged_number_3472950412842106880"]["fields"] == [
        {"name": "value", "type": "uint64"}
    ]
    assert by_name(structs["nested"]["fields"])["arg2"]["type"] == "uint32?"
    assert "pair_string_B_map_string_string_E" in structs
    assert "tuple_int32_float64_string_B_vector_int32_E" in structs

    variants = by_name(abi["variants"])
    assert variants["variant_uint64_str"]["types"] == ["uint64", "str"]
    tables = by_name(abi["tables"])
    assert set(tables) == {"defaultrec", "indextable", "owned", "records", "singletn", "varints"}
    assert tables["defaultrec"]["type"] == "defaultrec"
    assert tables["records"]["type"] == "record"
    assert tables["owned"]["type"] == "owned_record"
    assert tables["indextable"]["type"] == "indexed_record"
    assert tables["singletn"]["type"] == "indexed_record"
    assert tables["varints"]["type"] == "varint_record"
    assert by_name(structs["varint_record"]["fields"])["unsigned_value"]["type"] == "varuint32"
    assert by_name(structs["varint_record"]["fields"])["signed_value"]["type"] == "varint32"
    varint_fields = by_name(structs["varintargs"]["fields"])
    assert varint_fields["unsigned_value"]["type"] == "unsigned_int"
    assert varint_fields["signed_value"]["type"] == "signed_int"
    assert by_name(structs["extension"]["fields"])["value"]["type"] == "uint32$"
    assert structs["extended_symbol"]["fields"] == [
        {"name": "sym", "type": "symbol"},
        {"name": "contract", "type": "name"},
    ]
    assert structs["extrewards"]["fields"] == [{"name": "rewards", "type": "extended_symbol"}]
    assert by_name(structs["named"]["fields"])["owner"]["type"] == "my_account"
    assert by_name(abi["action_results"])["result"]["result_type"] == "result_value"
    assert by_name(abi["calls"])["sum"] == {
        "name": "sum",
        "type": "sum",
        "id": 193506202,
        "result_type": "uint32",
    }
    assert by_name(abi["actions"])["alias"]["ricardian_contract"].endswith(
        "This action is a donor-derived ABI fixture."
    )
    clauses = {entry["id"]: entry for entry in abi["ricardian_clauses"]}
    assert clauses["Compatibility"]["body"] == (
        "This clause records Spring and CDT compatibility intent."
    )


def normalize_abi(value):
    value = dict(value)
    value.pop("____comment", None)
    value.setdefault("error_messages", [])
    value.setdefault("abi_extensions", [])
    value.setdefault("variants", [])
    value.setdefault("action_results", [])
    return value


def check_fixture_semantics(name, abi):
    structs = by_name(abi["structs"])
    actions = by_name(abi["actions"])
    tables = by_name(abi["tables"])
    variants = by_name(abi["variants"])
    results = by_name(abi["action_results"])
    checks = {
        "action_results_test": lambda: results["action5"]["result_type"] == "test_res",
        "aliased_type_variant_template_arg": lambda: variants["variant_uint64_str"]["types"] == ["uint64", "str"],
        "nested_container": lambda: structs["settuple2"]["fields"][1]["type"]
        == "tuple_int32_float64_string_B_vector_int32_E",
        "ricardian_contract_test": lambda: actions["test"]["ricardian_contract"].startswith("---\nspec-version"),
        "singleton_contract": lambda: set(tables) == {"config", "config55", "mi.config52", "smpl.conf5", "smpl.config"},
        "struct_base_typedefd": lambda: structs["baz"]["base"] == "bar",
        "sync_call_test": lambda: by_name(abi["calls"])["sum"]["id"] == 193506202,
        "tagged_number_test": lambda: "TaggedNumber_3472950412842106880" in structs,
        "using_std_array": lambda: tables["greeting"]["type"] == "greeting",
    }
    if not checks[name]():
        raise RuntimeError(f"CDT donor fixture semantics changed: {name}")


def run_donor_fixtures(args, manifest):
    donor_root = os.environ.get("FORGE_CONTRACT_CDT_DONOR")
    donor_root = pathlib.Path(donor_root) if donor_root else None
    local_root = args.fixtures / "cdt"

    for name, contract in PASS_FIXTURES.items():
        source = local_root / "abigen-pass" / f"{name}.cpp"
        kwargs = {}
        if name == "ricardian_contract_test":
            kwargs |= {
                "ricardian_contracts": source.with_suffix(".contracts.md"),
                "ricardian_clauses": source.with_suffix(".clauses.md"),
            }
        abi = invoke(args, contract, source, args.output / "cdt", **kwargs)
        check_fixture_semantics(name, abi)
        if donor_root is not None:
            expected_path = donor_root / "tests/toolchain/abigen-pass" / f"{name}.abi"
            expected = json.loads(expected_path.read_text(encoding="utf-8"))
            if normalize_abi(abi) != normalize_abi(expected):
                raise RuntimeError(f"Forge ABI differs from CDT donor fixture: {name}")

    for name, contract in FAIL_FIXTURES.items():
        source = local_root / "abigen-fail" / f"{name}.cpp"
        invoke(
            args,
            contract,
            source,
            args.output / "cdt",
            succeeds=False,
        )

    mapped = {fixture["forge_case"] for fixture in manifest["fixtures"]}
    expected = {
        f"cdt/abigen-pass/{name}.cpp" for name in PASS_FIXTURES
    } | {
        f"cdt/abigen-fail/{name}.cpp" for name in FAIL_FIXTURES
    }
    if mapped != expected:
        raise RuntimeError("CDT donor fixture mapping does not match the executable suite")


def check_donor_manifest(fixtures):
    manifest = json.loads((fixtures / "donor_manifest.json").read_text(encoding="utf-8"))
    if manifest["donor"]["commit"] != "69599db279b7b93d0688502720c15c6962a1401b":
        raise RuntimeError("unexpected CDT donor commit")
    if len(manifest["fixtures"]) != 12:
        raise RuntimeError("CDT donor mapping is incomplete")

    donor_root = os.environ.get("FORGE_CONTRACT_CDT_DONOR")
    if not donor_root:
        return manifest
    donor_root = pathlib.Path(donor_root)
    for fixture in manifest["fixtures"]:
        source = donor_root / fixture["path"]
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if digest != fixture["sha256"]:
            raise RuntimeError(f"CDT donor fixture hash changed: {fixture['path']}")
    return manifest


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--abigen", required=True, type=pathlib.Path)
    parser.add_argument("--plugin", required=True, type=pathlib.Path)
    parser.add_argument("--sysroot", required=True, type=pathlib.Path)
    parser.add_argument("--include", required=True, type=pathlib.Path)
    parser.add_argument("--modules", required=True, type=pathlib.Path)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--fixtures", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = check_donor_manifest(args.fixtures)
    run_donor_fixtures(args, manifest)

    features = invoke(
        args,
        "abifixture",
        args.fixtures / "abigen_features.cpp",
        args.output,
        ricardian_contracts=args.fixtures / "ricardian.contracts.md",
        ricardian_clauses=args.fixtures / "ricardian.clauses.md",
    )
    check_features(features)

    named_action = invoke(
        args,
        "namedaction",
        args.fixtures / "named_action.cpp",
        args.output / "named-action",
    )
    named_action_structs = by_name(named_action["structs"])
    if by_name(named_action["actions"])["beginrev"]["type"] != "begin_revision":
        raise RuntimeError("named action did not use its DTO as the direct ABI payload")
    if named_action_structs["begin_revision"]["fields"] != [
        {"name": "workspace", "type": "uint64"},
        {"name": "inode", "type": "uint64"},
    ]:
        raise RuntimeError("named action ABI payload fields changed")
    if "submit" in named_action_structs:
        raise RuntimeError("named action generated a legacy method wrapper")

    named_helpers = invoke(
        args,
        "namedhelpers",
        args.fixtures / "named_action_unrelated_get_name.cpp",
        args.output / "named-action-unrelated-get-name",
    )
    named_helper_actions = by_name(named_helpers["actions"])
    named_helper_structs = by_name(named_helpers["structs"])
    if named_helper_actions["submit"]["type"] != "submit":
        raise RuntimeError("unrelated get_name method changed the legacy action ABI")
    if named_helper_structs["submit"]["fields"] != [
        {"name": "request", "type": "legacy_request"}
    ]:
        raise RuntimeError("legacy action wrapper fields changed")
    if named_helper_actions["publishrev"]["type"] != "overloaded_named_request":
        raise RuntimeError("valid named action hook was hidden by an unrelated overload")
    if "publish" in named_helper_structs:
        raise RuntimeError("overloaded named action generated a legacy method wrapper")
    if named_helper_actions["archive"]["type"] != "archive":
        raise RuntimeError("inaccessible get_name method changed the legacy action ABI")
    if named_helper_actions["erase"]["type"] != "erase":
        raise RuntimeError("deleted get_name method changed the legacy action ABI")
    if named_helper_actions["inherited"]["type"] != "inherited_named_request":
        raise RuntimeError("public inherited named action hook did not produce direct DTO ABI")
    if "restore" in named_helper_structs:
        raise RuntimeError("inherited named action generated a legacy method wrapper")
    if named_helper_actions["runtime"]["type"] != "runtime":
        raise RuntimeError("non-constexpr get_name method changed the legacy action ABI")
    if named_helper_actions["defaulted"]["type"] != "defaulted":
        raise RuntimeError("get_name method with parameters changed the legacy action ABI")
    if named_helper_actions["hidden"]["type"] != "hidden":
        raise RuntimeError("derived get_name overload did not hide the inherited named action hook")

    invoke(
        args,
        "namedmismatch",
        args.fixtures / "named_action_mismatch.cpp",
        args.output / "named-action-mismatch",
        succeeds=False,
        error_contains="action attribute name does not match payload get_name()",
    )
    invoke(
        args,
        "namedstatic",
        args.fixtures / "named_action_static.cpp",
        args.output / "named-action-static",
        succeeds=False,
        error_contains="contract entry point must be a non-static member function",
    )

    guest_macro = invoke(
        args,
        "guestmacro",
        args.fixtures.parent / "consumer" / "guest_macro_contract.cpp",
        args.output / "guest-macro",
        compiler_arguments=(
            "-DCONSUMER_DECLARATION_VALUE=42",
            r'-DCONSUMER_DECLARATION_TEXT="quoted \"value\" with space"',
        ),
    )
    if len(guest_macro["actions"]) != 2 or {
        action["name"] for action in guest_macro["actions"]
    } != {"run", "declared"}:
        raise RuntimeError("abigen did not analyze the guest contract translation unit")
    if next(
        item for item in guest_macro["action_results"] if item["name"] == "declared"
    )["result_type"] != "string":
        raise RuntimeError("abigen did not preserve the declared macro-selected action result")

    implicit_contract = invoke(
        args,
        "implicit_contract",
        args.fixtures / "implicit_contract_table.cpp",
        args.output / "implicit-contract-table",
    )
    if [table["name"] for table in implicit_contract["tables"]] != ["indextable"]:
        raise RuntimeError("abigen did not infer a table owned by an implicitly named contract")

    multi_source_contract = args.fixtures / "multi_source_contract.cpp"
    multi_source_helper = args.fixtures / "multi_source_helper.cpp"
    invoke(
        args,
        "multisource",
        args.fixtures / "empty_contract.cpp",
        args.output / "multi-source-invalid",
        succeeds=False,
        additional_sources=(multi_source_contract,),
    )
    multi_source = invoke(
        args,
        "multisource",
      multi_source_contract,
      args.output / "multi-source-valid",
      additional_sources=(multi_source_helper,),
   )
    if [action["name"] for action in multi_source["actions"]] != ["next", "previous"]:
        raise RuntimeError("shared multi-source action was not de-duplicated")
    helper_wrapper = args.output / "multi-source-valid/multi_source_contract.source-1.cpp"
    if "__forge_contract_action_" not in helper_wrapper.read_text(encoding="utf-8"):
        raise RuntimeError("secondary translation-unit action did not receive a dispatch thunk")

    user_vector = invoke(
        args,
        "uservector",
        args.fixtures / "user_vector_template.cpp",
        args.output / "user-vector-template",
    )
    user_vector_structs = by_name(user_vector["structs"])
    if user_vector_structs["store"]["fields"][0]["type"] != "vector_uint32":
        raise RuntimeError("user-defined vector template was encoded as a standard ABI array")
    if user_vector_structs["vector_uint32"]["fields"] != [{"name": "value", "type": "uint32"}]:
        raise RuntimeError("user-defined vector template record shape changed")

    eosio_transaction = invoke(
        args,
        "transactionabi",
        args.fixtures / "eosio_transaction_abi.cpp",
        args.output / "eosio-transaction-abi",
    )
    transaction_structs = [item for item in eosio_transaction["structs"] if item["name"] == "transaction"]
    if len(transaction_structs) != 1:
        raise RuntimeError("EOSIO transaction adapter did not reuse the canonical transaction ABI record")
    transaction_records = by_name(eosio_transaction["structs"])
    if transaction_records["permission_level"]["fields"] != [
        {"name": "actor", "type": "name"},
        {"name": "permission", "type": "name"},
    ]:
        raise RuntimeError("EOSIO transaction adapter changed the canonical permission_level ABI record")
    if transaction_records["action"] != {
        "name": "action",
        "base": "",
        "fields": [
            {"name": "account", "type": "name"},
            {"name": "name", "type": "name"},
            {"name": "authorization", "type": "permission_level[]"},
            {"name": "data", "type": "bytes"},
        ],
    }:
        raise RuntimeError("EOSIO transaction adapter leaked its C++ inheritance into the ABI")
    if transaction_records["extension"]["fields"] != [
        {"name": "type", "type": "uint16"},
        {"name": "data", "type": "bytes"},
    ]:
        raise RuntimeError("EOSIO transaction adapter changed the canonical extension ABI record")
    if by_name(eosio_transaction["actions"])["sendtrx"]["type"] != "submit":
        raise RuntimeError("annotated EOSIO action name replaced its donor method ABI type")
    submit_fields = by_name(by_name(eosio_transaction["structs"])["submit"]["fields"])
    if submit_fields["value"]["type"] != "transaction":
        raise RuntimeError("EOSIO transaction action argument changed ABI type")

    eosio_fixed_bytes = invoke(
        args,
        "fixedbytes",
        args.fixtures / "eosio_fixed_bytes_abi.cpp",
        args.output / "eosio-fixed-bytes-abi",
    )
    fixed_bytes_fields = by_name(by_name(eosio_fixed_bytes["structs"])["verify"]["fields"])
    if {name: field["type"] for name, field in fixed_bytes_fields.items()} != {
        "one": "checksum160",
        "two": "checksum256",
        "three": "checksum512",
    }:
        raise RuntimeError("EOSIO fixed_bytes adapters changed their canonical ABI names")
    if any(item["name"].startswith("fixed_bytes") for item in eosio_fixed_bytes["structs"]):
        raise RuntimeError("EOSIO fixed_bytes adapter leaked an implementation record into the ABI")

    sdk_alias = invoke(
        args,
        "sdkalias",
        args.fixtures / "sdk_type_alias.cpp",
        args.output / "sdk-type-alias",
    )
    sdk_alias_types = {item["new_type_name"]: item["type"] for item in sdk_alias["types"]}
    if sdk_alias_types.get("block_signing_authority") != "variant_block_signing_authority_v0":
        raise RuntimeError("SDK-owned type alias was erased from the contract ABI")
    authority_fields = by_name(by_name(sdk_alias["structs"])["producer_authority"]["fields"])
    if authority_fields["authority"]["type"] != "block_signing_authority":
        raise RuntimeError("SDK-owned type alias was desugared at its use site")

    typed_ids = invoke(
        args,
        "typedids",
        args.fixtures / "namespaced_typed_id.cpp",
        args.output / "namespaced-typed-id",
    )
    typed_id_types = {item["new_type_name"]: item["type"] for item in typed_ids["types"]}
    if "id" in typed_id_types:
        raise RuntimeError("namespaced typed IDs leaked a globally ambiguous ABI alias")
    if typed_id_types.get("coding_scheme") != "uint8":
        raise RuntimeError("named protocol enum did not preserve its ABI alias")
    typed_id_fields = by_name(by_name(typed_ids["structs"])["submit"]["fields"])
    if {name: field["type"] for name, field in typed_id_fields.items()} != {
        "application_id": "uint64",
        "workspace_id": "uint64",
        "repair_id": "uint64",
        "native_account_id": "uint64",
        "coding": "coding_scheme",
    }:
        raise RuntimeError("namespaced typed IDs or enum changed their canonical ABI")
    if any(item["name"].startswith("typed_id") for item in typed_ids["structs"]):
        raise RuntimeError("typed ID implementation records leaked into the contract ABI")

    typed_id_structs = by_name(typed_ids["structs"])
    state_fields = by_name(typed_id_structs["statevalues"]["fields"])
    if {name: field["type"] for name, field in state_fields.items() if name != "selector"} != {
        "parameters": "blockchain_parameters",
        "config": "chain_config",
        "wasm": "wasm_parameters",
        "elastic": "elastic_limit_parameters",
        "usage": "usage_accumulator",
        "feature": "activated_protocol_feature",
        "float64_value": "float64",
        "float128_value": "float128",
    }:
        raise RuntimeError("guest ABI did not traverse the canonical protocol state values")
    if "float64" in typed_id_structs or "float128" in typed_id_structs:
        raise RuntimeError("canonical protocol floats leaked their bits wrappers into the guest ABI")
    selector_type = state_fields["selector"]["type"]
    selector_record = typed_id_types.get(selector_type)
    if selector_type != "account_selector" or selector_record != "entity_selector_B_typed_id_1_10_E_name":
        raise RuntimeError("guest ABI did not preserve the account_selector type alias")
    if selector_type in typed_id_structs:
        raise RuntimeError("account_selector alias leaked a duplicate guest ABI record")
    if typed_id_structs[selector_record]["fields"] != [
        {"name": "id", "type": "uint64?"},
        {"name": "key", "type": "name?"},
    ]:
        raise RuntimeError("guest ABI did not traverse account_selector fields")
    if typed_id_structs["chain_config"]["base"] != "blockchain_parameters" or typed_id_structs["chain_config"][
        "fields"
    ] != [{"name": "max_action_return_value_size", "type": "uint32"}]:
        raise RuntimeError("chain_config guest ABI inheritance changed")
    if typed_id_structs["blockchain_parameters"]["fields"] != [
        {"name": "max_block_net_usage", "type": "uint64"},
        {"name": "target_block_net_usage_pct", "type": "uint32"},
        {"name": "max_transaction_net_usage", "type": "uint32"},
        {"name": "base_per_transaction_net_usage", "type": "uint32"},
        {"name": "net_usage_leeway", "type": "uint32"},
        {"name": "context_free_discount_net_usage_num", "type": "uint32"},
        {"name": "context_free_discount_net_usage_den", "type": "uint32"},
        {"name": "max_block_cpu_usage", "type": "uint32"},
        {"name": "target_block_cpu_usage_pct", "type": "uint32"},
        {"name": "max_transaction_cpu_usage", "type": "uint32"},
        {"name": "min_transaction_cpu_usage", "type": "uint32"},
        {"name": "max_transaction_lifetime", "type": "uint32"},
        {"name": "deferred_trx_expiration_window", "type": "uint32"},
        {"name": "max_transaction_delay", "type": "uint32"},
        {"name": "max_inline_action_size", "type": "uint32"},
        {"name": "max_inline_action_depth", "type": "uint16"},
        {"name": "max_authority_depth", "type": "uint16"},
    ]:
        raise RuntimeError("blockchain_parameters guest ABI changed")
    if typed_id_structs["elastic_limit_parameters"]["fields"][-2:] != [
        {"name": "contract_rate", "type": "ratio"},
        {"name": "expand_rate", "type": "ratio"},
    ] or typed_id_structs["ratio"]["fields"] != [
        {"name": "numerator", "type": "uint64"},
        {"name": "denominator", "type": "uint64"},
    ]:
        raise RuntimeError("elastic limit guest ABI did not traverse ratio")
    if typed_id_structs["usage_accumulator"]["fields"] != [
        {"name": "last_ordinal", "type": "uint32"},
        {"name": "value_ex", "type": "uint64"},
        {"name": "consumed", "type": "uint64"},
    ]:
        raise RuntimeError("usage_accumulator guest ABI changed")
    if typed_id_structs["activated_protocol_feature"]["fields"] != [
        {"name": "feature_digest", "type": "checksum256"},
        {"name": "activation_block_num", "type": "uint32"},
    ]:
        raise RuntimeError("activated_protocol_feature guest ABI changed")
    if "protocol_feature" in typed_id_structs or "protocol_feature_specification" in typed_id_structs:
        raise RuntimeError("host protocol feature API values leaked into the guest ABI")

    equivalent_struct = invoke(
        args,
        "equivalent",
        args.fixtures / "equivalent_struct.cpp",
        args.output / "equivalent-struct",
    )
    payload_structs = [item for item in equivalent_struct["structs"] if item["name"] == "payload"]
    if len(payload_structs) != 1 or payload_structs[0]["fields"] != [{"name": "value", "type": "uint32"}]:
        raise RuntimeError("equivalent ABI records were not de-duplicated")

    invoke(args, "duplicate", args.fixtures / "duplicate_action.cpp", args.output / "duplicate-action", succeeds=False)
    invoke(args, "overloaded", args.fixtures / "overloaded_action.cpp", args.output / "overloaded-action", succeeds=False)
    invoke(args, "duplicate", args.fixtures / "duplicate_struct.cpp", args.output / "duplicate-struct", succeeds=False)
    invoke(args, "duplicate", args.fixtures / "duplicate_table.cpp", args.output / "duplicate-table", succeeds=False)
    invoke(
        args,
        "anonymous",
        args.fixtures / "anonymous_record.cpp",
        args.output / "anonymous-record",
        succeeds=False,
        error_contains="anonymous or local scope",
    )
    invoke(
        args,
        "duplicateindex",
        args.fixtures / "duplicate_index_name.cpp",
        args.output / "duplicate-index-name",
        succeeds=False,
        error_contains="invalid index name used in multi_index",
    )
    invoke(
        args,
        "aliasclash",
        args.fixtures / "conflicting_type_alias.cpp",
        args.output / "conflicting-type-alias",
        succeeds=False,
    )
    invoke(
        args,
        "foreignscope",
        args.fixtures / "foreign_contract_attribute.cpp",
        args.output / "foreign-contract-attribute",
        succeeds=False,
    )
    invoke(
        args,
        "foreignscope",
        args.fixtures / "foreign_action_attribute.cpp",
        args.output / "foreign-action-attribute",
        succeeds=False,
    )
    invoke(
        args,
        "unnamedaction",
        args.fixtures / "unnamed_action_parameter.cpp",
        args.output / "unnamed-action-parameter",
        succeeds=False,
    )
    invoke(
        args,
        "unnamedcall",
        args.fixtures / "unnamed_call_parameter.cpp",
        args.output / "unnamed-call-parameter",
        succeeds=False,
    )

    modern = invoke(args, "parity", args.fixtures / "parity_modern.cpp", args.output)
    legacy = invoke(args, "parity", args.fixtures / "parity_legacy.cpp", args.output)
    if modern != legacy:
        raise RuntimeError("forge and eosio attribute spellings produced different canonical ABI")
    modern_dispatcher = (args.output / "parity_modern.dispatcher.cpp").read_text(encoding="utf-8")
    if not modern_dispatcher.startswith("#include <cstdint>\n"):
        raise RuntimeError("generated dispatcher does not declare its fixed-width integer dependency")

    for fixture in ("static_apply", "cpp_linkage_apply"):
        invoke(args, fixture, args.fixtures / f"{fixture}.cpp", args.output)
        dispatcher = (args.output / f"{fixture}.dispatcher.cpp").read_text(encoding="utf-8")
        if 'extern "C" [[gnu::visibility("default")]] void apply(' not in dispatcher:
            raise RuntimeError(f"{fixture} suppressed the generated contract dispatcher")

    invoke(args, "custom_apply", args.fixtures / "custom_apply.cpp", args.output)
    custom_dispatcher = (args.output / "custom_apply.dispatcher.cpp").read_text(encoding="utf-8")
    if 'extern "C" [[gnu::visibility("default")]] void apply(' in custom_dispatcher:
        raise RuntimeError("exported custom apply received a duplicate generated dispatcher")
    if "codec_traits<::custom_record>" not in custom_dispatcher:
        raise RuntimeError("exported custom apply did not receive generated record codecs")
    custom_apply_only = invoke(
        args,
        "custom_apply_only",
        args.fixtures / "custom_apply_only.cpp",
        args.output / "custom-apply-only",
    )
    if any(custom_apply_only[key] for key in ("types", "structs", "actions", "tables")):
        raise RuntimeError("custom apply without ABI declarations generated spurious ABI entries")
    invoke(
        args,
        "invalid_exported_apply",
        args.fixtures / "invalid_exported_apply.cpp",
        args.output,
        succeeds=False,
    )
    invoke(
        args,
        "enumconflict",
        args.fixtures / "conflicting_enum_names.cpp",
        args.output / "conflicting-enum-names",
        succeeds=False,
        error_contains="conflicting contract ABI type alias 'status'",
    )
    invoke(
        args,
        "invalidtable",
        args.fixtures / "invalid_table_name.cpp",
        args.output / "invalid-table-name",
        succeeds=False,
        error_contains="get_table_name() must be a public static constexpr",
    )
    invoke(
        args,
        "attrtable",
        args.fixtures / "attribute_table_name_mismatch.cpp",
        args.output / "attribute-table-name-mismatch",
        succeeds=False,
        error_contains="table attribute name does not match row get_table_name()",
    )
    equivalent_names = invoke(
        args,
        "equivalent",
        args.fixtures / "attribute_name_equivalent.cpp",
        args.output / "attribute-name-equivalent",
    )
    if [table["name"] for table in equivalent_names["tables"]] != ["records"]:
        raise RuntimeError("equivalent typed table names did not produce one canonical ABI table")

    bare_output = args.output / "bare-output"
    invoke(
        args,
        "parity",
        args.fixtures / "parity_modern.cpp",
        bare_output,
        bare_outputs=True,
    )
    if not (bare_output / "parity_modern.abi").is_file():
        raise RuntimeError("abigen did not write a bare ABI output path")
    if not (bare_output / "parity_modern.dispatcher.cpp").is_file():
        raise RuntimeError("abigen did not write a bare dispatcher output path")

    invoke(args, "empty", args.fixtures / "empty_contract.cpp", args.output, succeeds=False)
    invoke(args, "missing", args.fixtures / "wrong_contract.cpp", args.output, succeeds=False)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        print(f"run_abigen_fixtures: {error}", file=sys.stderr)
        raise
