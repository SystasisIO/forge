#!/usr/bin/env python3
"""Reproduce the Boost.Test port from the pinned EOS VM donor tests."""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass
import hashlib
import json
import pathlib
import re
import shutil
import sys
import tempfile


DONOR_COMMIT = "e5b1fc79c4b8d78f32749afa94a8d4c4d071f67f"
FIXTURE_COMMIT = "ffbbb552e6020623d8ae148d81a152c8ef700325"
DONOR_MANIFEST_SHA256 = "ede2eae24fe49c00c8ccfa99f8e4c8f2fbc370734e012c99bd225c38c36ca186"
HOST_FUNCTIONS_DONOR_SHA256 = "7eeddf96ad2340bb4de11ae131a68991d3a0f9138fa4ea9c5691f9650811a69f"
HOST_FUNCTIONS_PORT_SHA256 = "10b6d05a534c08055279a5144d3108dde96f7576779d811e1ad4e48d3687ad7d"
HOST_FUNCTIONS_HARNESS = (
    ("namespace {\n\ntemplate<typename Host>",
     'namespace {\n#include "host_functions_support.hpp"\ntemplate<typename Host>', 1),
    ("   using type_converter<Host>::type_converter;",
     "   cnv(Host* host, execution_interface interface) : type_converter<Host>(host, host_function_test_interface(interface)) {}", 1),
    ("bkend.get_context().linear_memory() + offset",
     "host_function_test_memory(bkend.get_context().linear_memory()) + offset", 4),
)

CASE_PATTERN = re.compile(
    r'\b(BACKEND_TEST_CASE|TEST_CASE)\s*\(\s*"((?:[^"\\]|\\.)*)"\s*,\s*"((?:[^"\\]|\\.)*)"\s*\)'
)


@dataclass(frozen=True)
class ProfileSource:
    donor_sha256: str
    historical_port_sha256: str
    added_assertions: int


@dataclass(frozen=True)
class ProfileExpectation:
    id: str
    source: str
    title: str
    tags: str
    category: str
    replacements: tuple[tuple[str, str], ...]
    fixtures: tuple[str, ...] = ()


# Both the pinned input and the syntax-only port must match before adapting an oracle.
PROFILE_SOURCES = {
    "allow_code_after_function_end_tests.cpp": ProfileSource(
        "711005a791821b96628b3d7e6d6a6e0c21fa723afa4705369f0a1a67ce9bc6c0",
        "a9b4427e88031e024a98c080e44887bee65fc86013c15b0ab5ad886e15563633", 2),
    "allow_invalid_empty_local_set_tests.cpp": ProfileSource(
        "b7334ab72d303495b7049eb84ad09c0acd4c282baf26d0244851f3abb6d04ea4",
        "de612f3c16b95a516ef8e4751dd333375c5875bae292b13d3899c086ac80e480", 2),
    "allow_u32_limits_flags_tests.cpp": ProfileSource(
        "430148c302450980652babd7093748f707c452d75116c727e02600523140977d",
        "7e8f95c8f7f01c41b393a474342532e413979978c15cfcef8e0c37d800b7f06d", 8),
    "implementation_limits_tests.cpp": ProfileSource(
        "efed154e042d5f263087b1c3e9911868e39149a4ee61c2650e1b5aa83095aae6",
        "12a0933c9899c62ee862166c531a35e8c89c88cdfdc563be3e20a48eee16f3fa", 1),
    "spec/binary_tests.cpp": ProfileSource(
        "28149f8eb5d1f67a4b9a0feb7f78b785458638f89b9257d44e911125b22b0015",
        "54e8b8058c12bf8c552e6edf51d1064f07422ab2ea99d27b08882c31de83c6d2", 0),
    "spec/unreached-invalid_tests.cpp": ProfileSource(
        "c349f52609e80f69d7de4dbe65b4c3178e8d8a4cd50c39fb1b28bdbb3b8e98cf",
        "d2378a06e9b32b8ca550a36556f751f9ae85477dc5664e9b55127380056ac2a9", 0),
    "spec/e_memory_tests.cpp": ProfileSource(
        "b8a995617f88453669f06c7107793cb520a8a52fbef30d90c8589eac85750000",
        "578bb82755ad12cd10d28ac0fcdb21b64f83fcb0596fc463bef3a98399509a8b", 1),
}

PROFILE_EXPECTATIONS = (
    ProfileExpectation(
        "retired_after_end_static", "allow_code_after_function_end_tests.cpp",
        "Test allow_code_after_function_end static pass", "[allow_code_after_function_end_test]",
        "retired_malformed", ((
            "   backend_t backend(code_after_end_wasm, &wa);",
            "   BOOST_CHECK_THROW(backend_t(code_after_end_wasm, &wa), exceptions::parse);"),)),
    ProfileExpectation(
        "retired_after_end_dynamic", "allow_code_after_function_end_tests.cpp",
        "Test allow_code_after_function_end dynamic pass", "[allow_code_after_function_end_test]",
        "retired_malformed", ((
            "   backend_t backend(code_after_end_wasm, nullptr, dynamic_options{true});",
            "   BOOST_CHECK_THROW(backend_t(code_after_end_wasm, nullptr, dynamic_options{true}), exceptions::parse);"),)),
    ProfileExpectation(
        "retired_empty_local_static", "allow_invalid_empty_local_set_tests.cpp",
        "Test allow_invalid_empty_local_set static pass", "[allow_invalid_empty_local_set_test]",
        "retired_malformed", ((
            "   backend_t backend(bad_local_wasm, &wa);",
            "   BOOST_CHECK_THROW(backend_t(bad_local_wasm, &wa), exceptions::parse);"),)),
    ProfileExpectation(
        "retired_empty_local_dynamic", "allow_invalid_empty_local_set_tests.cpp",
        "Test allow_invalid_empty_local_set dynamic pass", "[allow_invalid_empty_local_set_test]",
        "retired_malformed", ((
            "   backend_t backend(bad_local_wasm, nullptr, dynamic_options{true});",
            "   BOOST_CHECK_THROW(backend_t(bad_local_wasm, nullptr, dynamic_options{true}), exceptions::parse);"),)),
    ProfileExpectation(
        "retired_limits_flags_static", "allow_u32_limits_flags_tests.cpp",
        "Test allow_u32_limits_flags static pass", "[allow_u32_limits_flags_test]",
        "retired_malformed", tuple(
            (f"   backend_t backend_{name}({name}_wasm, &wa);",
             f"   BOOST_CHECK_THROW(backend_t({name}_wasm, &wa), exceptions::parse);")
            for name in ("mem_no_max", "mem_max", "table_no_max", "table_max"))),
    ProfileExpectation(
        "retired_limits_flags_dynamic", "allow_u32_limits_flags_tests.cpp",
        "Test allow_u32_limits_flags dynamic pass", "[allow_u32_limits_flags_test]",
        "retired_malformed", tuple(
            (f"   backend_t backend_{name}({name}_wasm, nullptr, dynamic_options{{true}});",
             f"   BOOST_CHECK_THROW(backend_t({name}_wasm, nullptr, dynamic_options{{true}}), exceptions::parse);")
            for name in ("mem_max", "mem_no_max", "table_max", "table_no_max"))),
    ProfileExpectation(
        "capacity_depth", "implementation_limits_tests.cpp",
        "Test call depth dynamic", "[call_depth]", "capacity", (
            ("   // Very large call depth requires dynamically allocating a new stack",
             "   // Refuse over-profile configuration, then recover with the supported depth."),
            ('   bkend.initialize(nullptr, dynamic_options{1024*1024});\n\n'
             '   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call", (uint32_t)1024*1024 - 1)));',
             '   BOOST_CHECK_THROW(bkend.initialize(nullptr, dynamic_options{1024*1024}), exceptions::profile);\n'
             '   bkend.initialize(nullptr, dynamic_options{51});\n'
             '   BOOST_TEST(static_cast<bool>(!bkend.call_with_return("env", "call", (uint32_t)50)));'))),
    ProfileExpectation(
        "capacity_initial_pages", "spec/e_memory_tests.cpp",
        "Testing wasm <e_memory_1_wasm>", "[e_memory_1_wasm_tests]", "capacity", ((
            "   backend_t bkend( code, &wa );",
            "   BOOST_CHECK_THROW(backend_t(code, &wa), exceptions::profile);"),), ("e_memory.1.wasm",)),
) + tuple(
    ProfileExpectation(
        f"modern_binary_{number}", "spec/binary_tests.cpp",
        f"Testing wasm <binary_{number}_wasm>", f"[binary_{number}_wasm_tests]", "modern_grammar",
        (("   BOOST_CHECK_THROW(backend_t(code, nullptr), std::exception);",
          "   BOOST_REQUIRE_NO_THROW(backend_t(code, nullptr));"),), (f"binary.{number}.wasm",))
    for number in (38, 39, 40, 41, 43, 44, 45, 46)
) + (
    ProfileExpectation(
        "modern_unreachable_br_table", "spec/unreached-invalid_tests.cpp",
        "Testing wasm <unreached-invalid_87_wasm>", "[unreached-invalid_87_wasm_tests]", "modern_typing",
        (("   BOOST_CHECK_THROW(backend_t(code, nullptr), std::exception);",
          "   BOOST_REQUIRE_NO_THROW(backend_t(code, nullptr));"),), ("unreached-invalid.87.wasm",)),
)

UNIT_SOURCES = (
    "allocator_tests.cpp",
    "eosio_max_nested_structures_tests.cpp",
    "guarded_ptr_tests.cpp",
    "varint_tests.cpp",
    "variant_tests.cpp",
    "host_functions_tests.cpp",
    "preconditions_tests.cpp",
    "allow_code_after_function_end_tests.cpp",
    "allow_invalid_empty_local_set_tests.cpp",
    "allow_u32_limits_flags_tests.cpp",
    "allow_zero_blocktype_tests.cpp",
    "forbid_export_mutable_globals_tests.cpp",
    "max_br_table_elements_tests.cpp",
    "max_code_bytes_tests.cpp",
    "max_func_local_bytes_tests.cpp",
    "max_linear_memory_init_tests.cpp",
    "max_local_sets_tests.cpp",
    "max_memory_offset_tests.cpp",
    "max_mutable_globals_tests.cpp",
    "max_nested_structures_tests.cpp",
    "max_pages_tests.cpp",
    "max_section_elements_tests.cpp",
    "max_table_elements_tests.cpp",
    "null_tests.cpp",
    "reentry_tests.cpp",
    "signals_tests.cpp",
    "stack_restriction_tests.cpp",
    "watchdog_tests.cpp",
    "implementation_limits_tests.cpp",
    "instantiation_tests.cpp",
    "backend_tests.cpp",
    "vector_tests.cpp",
)

SPEC_SOURCES = (
    "address", "align", "binary", "binary-leb128", "block", "br", "br_if", "br_table",
    "break-drop", "call_indirect", "call", "const", "conversions", "custom", "data", "elem",
    "endianness", "f32", "f32_bitwise", "f32_cmp", "f64", "f64_bitwise", "f64_cmp", "fac",
    "float_exprs", "float_literals", "float_memory", "float_misc", "forward", "func", "func_ptrs",
    "globals", "i32", "i64", "if", "int_exprs", "int_literals", "labels", "left-to-right", "load",
    "local_get", "local_set", "local_tee", "loop", "memory", "memory_redundancy", "memory_grow",
    "memory_size", "memory_trap", "names", "nop", "return", "select", "stack", "start", "store",
    "switch", "traps", "type", "typecheck", "unreachable", "unreached-invalid", "unwind",
    "utf8-custom-section-id", "utf8-import-field", "utf8-import-module",
    "e_block", "e_function", "e_globals", "e_locals", "e_memory", "e_module", "e_table",
)

SUPPORT_FILES = (
    "host_functions_tests_0.wasm.hpp",
    "host_functions_tests_1.wasm.hpp",
    "implementation_limits.hpp",
    "reentry.wasm.hpp",
    "host.wasm",
)

EXCEPTION_RENAMES = {
    "guarded_ptr_exception": "exceptions::pointer_out_of_bounds",
    "wasm_bad_alloc": "exceptions::allocation",
    "wasm_exit_exception": "exceptions::exit",
    "wasm_interpreter_exception": "exceptions::interpreter",
    "wasm_memory_exception": "exceptions::memory",
    "wasm_parse_exception": "exceptions::parse",
    "wasm_section_length_exception": "exceptions::section_length",
    "wasm_unsupported_import_exception": "exceptions::unsupported_import",
    "wasm_vector_oob_exception": "exceptions::vector_out_of_bounds",
}

VM_HEADER_MODULES = {
    "allocator": "forge.vm.wasm.interpret.allocator",
    "argument_proxy": "forge.vm.wasm.interpret.argument_proxy",
    "backend": "forge.vm.wasm.interpret.backend",
    "guarded_ptr": "forge.vm.wasm.interpret.guarded_ptr",
    "host_function": "forge.vm.wasm.interpret.host_function",
    "stack_elem": "forge.vm.wasm.interpret.stack_elem",
    "types": "forge.vm.wasm.interpret.types",
    "utils": "forge.vm.wasm.interpret.utils",
    "variant": "forge.vm.wasm.interpret.variant",
    "vector": "forge.vm.wasm.interpret.vector",
    "watchdog": "forge.vm.wasm.interpret.watchdog",
}

VM_HEADER_PARTITIONS = {
    "leb128": ":leb128",
    "signals": ":signals",
}

TEST_SUPPORT_MODULES = (
    "forge.vm.wasm.interpret.allocator",
    "forge.vm.wasm.interpret.stack_elem",
    "forge.vm.wasm.interpret.utils",
)


def sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def git_head(path: pathlib.Path) -> str:
    import subprocess

    return subprocess.check_output(
        ["git", "-C", str(path), "rev-parse", "HEAD"], text=True
    ).strip()


def active_sources(donor: pathlib.Path) -> list[pathlib.Path]:
    tests = donor / "tests"
    result = [tests / name for name in UNIT_SOURCES]
    result.extend(tests / "spec" / f"{name}_tests.cpp" for name in SPEC_SOURCES)
    missing = [str(path) for path in result if not path.is_file()]
    if missing:
        raise RuntimeError("missing donor tests:\n" + "\n".join(missing))
    return result


def case_manifest(text: str, source: str) -> list[dict[str, object]]:
    cases = [
        {
            "kind": match.group(1),
            "title": match.group(2),
            "tags": match.group(3),
            "line": text.count("\n", 0, match.start()) + 1,
        }
        for match in CASE_PATTERN.finditer(text)
    ]
    if source == "max_section_elements_tests.cpp":
        for name in ("type", "import", "function", "global", "export", "element", "data"):
            cases.append(
                {
                    "kind": "BACKEND_TEST_CASE",
                    "title": f"Test max_{name}_section_elements",
                    "tags": "[max_section_elements_test]",
                    "line": None,
                }
            )
    return cases


def assertion_manifest(text: str) -> dict[str, int]:
    return {
        name: len(re.findall(rf"\b{name}\s*\(", text))
        for name in ("CHECK", "CHECK_THROWS_AS", "REQUIRE")
    }


def build_manifest(donor: pathlib.Path, fixtures: pathlib.Path) -> dict[str, object]:
    if git_head(donor) != DONOR_COMMIT:
        raise RuntimeError(f"donor HEAD must be {DONOR_COMMIT}")
    if git_head(fixtures) != FIXTURE_COMMIT:
        raise RuntimeError(f"fixture HEAD must be {FIXTURE_COMMIT}")

    sources = []
    for path in active_sources(donor):
        text = path.read_text()
        sources.append(
            {
                "path": path.relative_to(donor).as_posix(),
                "sha256": sha256(path),
                "cases": case_manifest(text, path.name),
                "assertions": assertion_manifest(text),
            }
        )

    fixture_entries = [
        {
            "path": path.relative_to(fixtures).as_posix(),
            "sha256": sha256(path),
        }
        for path in sorted(fixtures.rglob("*.wasm"))
    ]
    return {
        "donor": {
            "repository": "https://github.com/AntelopeIO/eos-vm.git",
            "commit": DONOR_COMMIT,
        },
        "fixtures": {
            "repository": "https://github.com/EOSIO/eos-vm-test-wasms.git",
            "commit": FIXTURE_COMMIT,
            "files": fixture_entries,
        },
        "backend_matrix": {
            "x86_64": ["interpreter", "jit"],
            "other": ["interpreter"],
        },
        "sources": sources,
    }


def wrap_boolean_assertions(text: str, source: str, target: str) -> str:
    """Map a Catch boolean assertion without enabling Boost's C-string comparison."""
    pattern = re.compile(rf"\b{source}\s*\(")
    output: list[str] = []
    offset = 0

    while match := pattern.search(text, offset):
        opening = text.find("(", match.start())
        depth = 1
        index = opening + 1
        quote: str | None = None
        line_comment = False
        block_comment = False

        while index < len(text) and depth:
            char = text[index]
            following = text[index + 1] if index + 1 < len(text) else ""

            if line_comment:
                line_comment = char != "\n"
            elif block_comment:
                if char == "*" and following == "/":
                    block_comment = False
                    index += 1
            elif quote:
                if char == "\\":
                    index += 1
                elif char == quote:
                    quote = None
            elif char == "/" and following == "/":
                line_comment = True
                index += 1
            elif char == "/" and following == "*":
                block_comment = True
                index += 1
            elif char in {'"', "'"}:
                quote = char
            elif char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
            index += 1

        if depth:
            raise RuntimeError(f"unterminated {source} assertion")

        expression = text[opening + 1 : index - 1]
        output.append(text[offset : match.start()])
        output.append(f"{target}(static_cast<bool>({expression}))")
        offset = index

    output.append(text[offset:])
    return "".join(output)


def replace_live_symbols(text: str) -> str:
    text = re.sub(r"^\s*#\s*include\s*[<\"]catch2/catch\.hpp[>\"]\s*$", "", text, flags=re.M)
    text = re.sub(r"^\s*#\s*include\s*[<\"]eosio/vm/[^>\"]+[>\"]\s*$", "", text, flags=re.M)
    text = re.sub(r"^\s*#\s*include\s*[<\"](?:utils|wasm_config)\.hpp[>\"]\s*$", "", text, flags=re.M)
    text = text.replace("using namespace eosio::vm;", "using namespace forge::vm::wasm::interpret;")
    text = text.replace("using namespace eosio;", "")
    text = text.replace("eosio::vm::", "forge::vm::wasm::interpret::")
    text = text.replace("eosio_options", "compatibility_options")
    text = text.replace("eosio_max_nested_structures", "max_legacy_nested_structures")
    text = text.replace("EOS_VM_FROM_WASM", "FORGE_VM_WASM_INTERPRET_FROM_WASM")
    text = text.replace("EOS_VM_PRECONDITION", "FORGE_VM_WASM_INTERPRET_PRECONDITION")
    text = text.replace("EOS_VM_INVOKE_ON_ALL", "FORGE_VM_WASM_INTERPRET_INVOKE_ON_ALL")
    text = text.replace("EOS_VM_INVOKE_ONCE", "FORGE_VM_WASM_INTERPRET_INVOKE_ONCE")
    text = text.replace("EOS_VM_INVOKE_ON", "FORGE_VM_WASM_INTERPRET_INVOKE_ON")
    for old, new in EXCEPTION_RENAMES.items():
        text = re.sub(rf"\b{old}\b", new, text)
    text = re.sub(r"\bCHECK_THROWS_AS\s*\(", "BOOST_CHECK_THROW(", text)
    text = wrap_boolean_assertions(text, "CHECK", "BOOST_TEST")
    text = wrap_boolean_assertions(text, "REQUIRE", "BOOST_REQUIRE")
    return text


def required_imports(text: str, relative: pathlib.Path) -> tuple[list[str], list[str]]:
    headers = re.findall(r'^\s*#\s*include\s*[<"]eosio/vm/([^>"]+)\.hpp[>"]\s*$', text, flags=re.M)
    modules = list(TEST_SUPPORT_MODULES)
    partitions: list[str] = []

    for header in headers:
        if header in VM_HEADER_MODULES:
            modules.append(VM_HEADER_MODULES[header])
        elif header in VM_HEADER_PARTITIONS:
            partitions.append(VM_HEADER_PARTITIONS[header])
        else:
            raise RuntimeError(f"{relative}: no Forge module mapping for eosio/vm/{header}.hpp")

    if partitions and relative.name not in {"varint_tests.cpp", "signals_tests.cpp"}:
        raise RuntimeError(f"{relative}: backend partition import requires an internal module test")

    return list(dict.fromkeys(modules)), list(dict.fromkeys(partitions))


def transform_text(text: str, relative: pathlib.Path) -> str:
    modules, partitions = required_imports(text, relative)
    uses_backend = "BACKEND_TEST_CASE" in text
    text = replace_live_symbols(text)
    internal = relative.name in {"varint_tests.cpp", "signals_tests.cpp"}
    prefix = re.sub(r"[^a-zA-Z0-9_]", "_", relative.with_suffix("").as_posix())
    if internal:
        include_pattern = r"^[ \t]*#[ \t]*include[ \t]*[<\"][^>\"]+[>\"][ \t]*$"
        includes = re.findall(include_pattern, text, flags=re.M)
        text = re.sub(include_pattern, "", text, flags=re.M)
        heading = (
            "module;\n\n"
            f'#include "test_prelude.hpp"\n'
            + "\n".join(includes)
            + "\n\nmodule forge.vm.wasm.interpret.backend;\n\n"
            + "\n".join(f"import {name};" for name in partitions + modules)
            + "\n\n"
            "#define FORGE_VM_WASM_INTERPRET_INTERNAL_TESTS\n"
            f'#include "test_support.hpp"\n\n'
            f"#define FORGE_VM_WASM_INTERPRET_TEST_FILE {prefix}\n\n"
        )
    else:
        backend_marker = "#define FORGE_VM_WASM_INTERPRET_TEST_USES_BACKEND\n" if uses_backend else ""
        heading = (
            f'#include "test_prelude.hpp"\n'
            + "\n".join(f"import {name};" for name in modules)
            + "\n"
            + backend_marker
            + f'#include "test_support.hpp"\n\n'
            f"#define FORGE_VM_WASM_INTERPRET_TEST_FILE {prefix}\n\n"
        )
    transformed = heading + text.lstrip()
    lines = [line.rstrip() for line in transformed.splitlines()]
    while lines and not lines[-1]:
        lines.pop()
    return "\n".join(lines) + "\n"


def transform_source(path: pathlib.Path, donor: pathlib.Path) -> str:
    relative = path.relative_to(donor / "tests")
    prepared = apply_profile_expectations(transform_text(path.read_text(), relative), relative, sha256(path))
    return adapt_host_function_harness(prepared, relative, sha256(path))


def adapt_host_function_harness(text: str, relative: pathlib.Path, donor_sha256: str) -> str:
    if relative.as_posix() != "host_functions_tests.cpp":
        return text
    if donor_sha256 != HOST_FUNCTIONS_DONOR_SHA256:
        raise RuntimeError("host functions harness: pinned donor source hash differs")
    if hashlib.sha256(text.encode()).hexdigest() != HOST_FUNCTIONS_PORT_SHA256:
        raise RuntimeError("host functions harness: historical port hash differs")
    before = text
    for old, new, count in HOST_FUNCTIONS_HARNESS:
        if text.count(old) != count:
            raise RuntimeError(f"host functions harness: expected {count} statement anchors for {old!r}")
        text = text.replace(old, new)
    if (text.count("\n") != before.count("\n")
            or case_manifest(text, relative.name) != case_manifest(before, relative.name)
            or ported_assertions(text) != ported_assertions(before)):
        raise RuntimeError("host functions harness: changed cases, lines or assertions")
    return text


def ported_assertions(text: str) -> dict[str, int]:
    return {
        name: len(re.findall(rf"\b{name}\s*\(", text))
        for name in ("BOOST_TEST", "BOOST_REQUIRE", "BOOST_CHECK_THROW", "BOOST_REQUIRE_NO_THROW")
    }


def profile_case_span(text: str, rule: ProfileExpectation) -> tuple[int, int]:
    matches = list(CASE_PATTERN.finditer(text))
    selected = [
        index for index, match in enumerate(matches)
        if match.group(1) == "BACKEND_TEST_CASE" and match.group(2) == rule.title and match.group(3) == rule.tags
    ]
    if len(selected) != 1:
        raise RuntimeError(f"{rule.id}: expected exactly one title/tags anchor, got {len(selected)}")
    index = selected[0]
    return matches[index].start(), matches[index + 1].start() if index + 1 < len(matches) else len(text)


def apply_profile_expectations(text: str, relative: pathlib.Path, donor_sha256: str) -> str:
    source = relative.as_posix()
    policy = PROFILE_SOURCES.get(source)
    if policy is None:
        return text
    if donor_sha256 != policy.donor_sha256:
        raise RuntimeError(f"{source}: pinned donor source hash differs")
    if hashlib.sha256(text.encode()).hexdigest() != policy.historical_port_sha256:
        raise RuntimeError(f"{source}: historical port hash differs")

    before = text
    for rule in PROFILE_EXPECTATIONS:
        if rule.source != source:
            continue
        start, end = profile_case_span(text, rule)
        block = text[start:end]
        for name in rule.fixtures:
            if block.count(f'"{name}"') != 1:
                raise RuntimeError(f"{rule.id}: fixture name is not bound to this exact case")
        for old, new in rule.replacements:
            if old.count("\n") != new.count("\n"):
                raise RuntimeError(f"{rule.id}: replacement would change compiled case line IDs")
            if block.count(old) != 1:
                raise RuntimeError(f"{rule.id}: expected exactly one statement anchor")
            block = block.replace(old, new, 1)
        text = text[:start] + block + text[end:]

    if case_manifest(before, relative.name) != case_manifest(text, relative.name):
        raise RuntimeError(f"{source}: profile adaptation changed case registrations")
    if sum(ported_assertions(text).values()) - sum(ported_assertions(before).values()) != policy.added_assertions:
        raise RuntimeError(f"{source}: profile adaptation changed the reviewed assertion count")
    return text


def build_profile_manifest(donor: pathlib.Path, fixtures: pathlib.Path) -> dict[str, object]:
    categories = {name: sum(rule.category == name for rule in PROFILE_EXPECTATIONS)
                  for name in ("retired_malformed", "capacity", "modern_grammar", "modern_typing")}
    if (len(PROFILE_EXPECTATIONS) != 17 or len({rule.id for rule in PROFILE_EXPECTATIONS}) != 17
            or {rule.source for rule in PROFILE_EXPECTATIONS} != set(PROFILE_SOURCES)
            or categories != {"retired_malformed": 6, "capacity": 2, "modern_grammar": 8, "modern_typing": 1}):
        raise RuntimeError("profile rule inventory differs from the approved 17 cases")

    sources = []
    for path in active_sources(donor):
        relative = path.relative_to(donor / "tests")
        original = path.read_text()
        historical = transform_text(original, relative)
        prepared = apply_profile_expectations(historical, relative, sha256(path))
        prepared = adapt_host_function_harness(prepared, relative, sha256(path))
        cases = case_manifest(historical, relative.name)
        if cases != case_manifest(prepared, relative.name):
            raise RuntimeError(f"{relative}: changed case registrations")
        sources.append({
            "source": relative.as_posix(),
            "output": (pathlib.Path("spec") / path.name if relative.parts[0] == "spec"
                       else pathlib.Path("unit") / path.name).as_posix(),
            "donor_sha256": sha256(path),
            "historical_port_sha256": hashlib.sha256(historical.encode()).hexdigest(),
            "profile_port_sha256": hashlib.sha256(prepared.encode()).hexdigest(),
            "case_count": len(cases),
            "registration_sha256": hashlib.sha256(json.dumps(cases, sort_keys=True).encode()).hexdigest(),
            "donor_assertions": assertion_manifest(original),
            "historical_port_assertions": ported_assertions(historical),
            "profile_port_assertions": ported_assertions(prepared),
        })

    rules = []
    for rule in PROFILE_EXPECTATIONS:
        original = (donor / "tests" / rule.source).read_text()
        historical = transform_text(original, pathlib.Path(rule.source))
        start, end = profile_case_span(historical, rule)
        row = asdict(rule)
        row["compiled_case_line"] = historical.count("\n", 0, start) + 1
        row["historical_case_sha256"] = hashlib.sha256(historical[start:end].encode()).hexdigest()
        row["fixture_inputs"] = [{"path": name, "sha256": sha256(fixtures / name)} for name in rule.fixtures]
        rules.append(row)

    here = pathlib.Path(__file__).parent
    return {
        "schema_version": 1,
        "oracle": "forge-approved-wamr-profile-v1; not historical EOS VM parity",
        "donor_commit": DONOR_COMMIT,
        "fixture_commit": FIXTURE_COMMIT,
        "historical_donor_manifest_sha256": DONOR_MANIFEST_SHA256,
        "policy": {
            "max_call_depth": 1024, "max_initial_pages": 65535,
            "parse_code": 4000006, "profile_code": 4030000,
            "approval": "docs/releases/unreleased-wamr-bounded-profile.md#retired-parser-permissions",
            "modern_inputs": "u32 zero memidx encodings; unreachable br_table bottom typing",
        },
        "tool_inputs": [{"path": name, "sha256": sha256(here / name)} for name in
                        ("port_donor_tests.py", "test_port_donor_tests.py", "PROFILE_ORACLE.md")],
        "harness_inputs": [{"path": name, "sha256": sha256(here / name)} for name in
                           ("CMakeLists.txt", "test_support.hpp", "test_prelude.hpp", "host_functions_support.hpp",
                            "donor/sources.cmake")],
        "harness_adaptations": [{
            "source": "host_functions_tests.cpp",
            "donor_sha256": HOST_FUNCTIONS_DONOR_SHA256,
            "historical_port_sha256": HOST_FUNCTIONS_PORT_SHA256,
            "purpose": "Real host pointer/reference identity storage for the unchanged memoryless fixture",
            "replacements": [{"old": old, "new": new, "count": count}
                             for old, new, count in HOST_FUNCTIONS_HARNESS],
        }],
        "support_inputs": [{"path": f"support/{name}", "sha256": sha256(donor / "tests" / name)}
                           for name in SUPPORT_FILES],
        "verification_boundary": "Source transformation only; C++ compilation/runtime NOT_RUN by this slice",
        "rule_count": len(rules),
        "case_results_when_interpreter_and_jit_enabled": 2 * len(rules),
        "rule_categories": categories,
        "source_count": len(sources),
        "case_count": sum(row["case_count"] for row in sources),
        "historical_assertion_sites": sum(sum(row["historical_port_assertions"].values()) for row in sources),
        "profile_assertion_sites": sum(sum(row["profile_port_assertions"].values()) for row in sources),
        "rules": rules,
        "sources": sources,
    }


def write_port(donor: pathlib.Path, output: pathlib.Path) -> None:
    # Check every hash/anchor before writing any generated translation unit.
    transformed = {source: transform_source(source, donor) for source in active_sources(donor)}
    unit = output / "unit"
    spec = output / "spec"
    support = output / "support"
    unit.mkdir(parents=True, exist_ok=True)
    spec.mkdir(parents=True, exist_ok=True)
    support.mkdir(parents=True, exist_ok=True)

    for source, text in transformed.items():
        relative = source.relative_to(donor / "tests")
        destination = spec / source.name if relative.parts[0] == "spec" else unit / source.name
        destination.write_text(text)

    for name in SUPPORT_FILES:
        shutil.copyfile(donor / "tests" / name, support / name)

    regular_units = [name for name in UNIT_SOURCES if name not in {"varint_tests.cpp", "signals_tests.cpp"}]
    source_lines = ["# Generated by port_donor_tests.py. Do not edit.", ""]
    source_lines.append("set(FORGE_VM_WASM_INTERPRET_DONOR_UNIT_SOURCES")
    source_lines.extend(f"   ${{CMAKE_CURRENT_LIST_DIR}}/unit/{name}" for name in regular_units)
    source_lines.append(")")
    source_lines.append("")
    source_lines.append("set(FORGE_VM_WASM_INTERPRET_DONOR_INTERNAL_SOURCES")
    source_lines.extend(
        f"   ${{CMAKE_CURRENT_LIST_DIR}}/unit/{name}" for name in ("varint_tests.cpp", "signals_tests.cpp")
    )
    source_lines.append(")")
    source_lines.append("")
    source_lines.append("set(FORGE_VM_WASM_INTERPRET_DONOR_SPEC_SOURCES")
    source_lines.extend(f"   ${{CMAKE_CURRENT_LIST_DIR}}/spec/{name}_tests.cpp" for name in SPEC_SOURCES)
    source_lines.append(")")
    source_lines.append("")
    (output / "sources.cmake").write_text("\n".join(source_lines))


def compare_tree(expected: pathlib.Path, actual: pathlib.Path) -> list[str]:
    expected_files = {
        path.relative_to(expected): path for path in expected.rglob("*") if path.is_file()
    }
    actual_files = {
        path.relative_to(actual): path
        for path in actual.rglob("*")
        if path.is_file() and path.parts[-1] not in {"donor_manifest.json"}
    }
    differences = []
    for relative in sorted(expected_files.keys() | actual_files.keys()):
        if relative not in expected_files:
            differences.append(f"unexpected {relative}")
        elif relative not in actual_files:
            differences.append(f"missing {relative}")
        elif expected_files[relative].read_bytes() != actual_files[relative].read_bytes():
            differences.append(f"changed {relative}")
    return differences


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--donor", type=pathlib.Path, required=True)
    parser.add_argument("--fixtures", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path(__file__).parent / "donor")
    parser.add_argument("--manifest", type=pathlib.Path, default=pathlib.Path(__file__).parent / "donor_manifest.json")
    parser.add_argument("--profile-manifest", type=pathlib.Path,
                        default=pathlib.Path(__file__).parent / "profile_oracle_manifest.json")
    parser.add_argument("--write", action="store_true")
    parser.add_argument("--check", action="store_true")
    arguments = parser.parse_args()

    if arguments.write == arguments.check:
        parser.error("choose exactly one of --write or --check")

    manifest = build_manifest(arguments.donor, arguments.fixtures)
    manifest_text = json.dumps(manifest, indent=2, sort_keys=True) + "\n"

    if (not arguments.manifest.is_file() or sha256(arguments.manifest) != DONOR_MANIFEST_SHA256
            or arguments.manifest.read_text() != manifest_text):
        print("historical donor manifest differs from the pinned source/corpus; refusing to rebaseline", file=sys.stderr)
        return 1
    profile = build_profile_manifest(arguments.donor, arguments.fixtures)
    profile_text = json.dumps(profile, indent=2, sort_keys=True) + "\n"

    if arguments.write:
        write_port(arguments.donor, arguments.output)
        arguments.profile_manifest.write_text(profile_text)
        return 0

    if not arguments.profile_manifest.is_file() or arguments.profile_manifest.read_text() != profile_text:
        print("profile oracle manifest differs from the reviewed transformation", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as temporary:
        generated = pathlib.Path(temporary) / "donor"
        write_port(arguments.donor, generated)
        differences = compare_tree(generated, arguments.output)
    if differences:
        print("donor test port differs:", file=sys.stderr)
        print("\n".join(differences), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
