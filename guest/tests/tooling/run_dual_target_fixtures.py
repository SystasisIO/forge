#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
from pathlib import Path


def run(*command: str, cwd: Path | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"command failed:\n{' '.join(command)}\n{result.stdout}"
        )
    return result.stdout


def run_failure(*command: str, contains: str, cwd: Path | None = None) -> None:
    result = subprocess.run(
        command,
        cwd=cwd,
        check=False,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode == 0:
        raise RuntimeError(
            f"command unexpectedly succeeded:\n{' '.join(command)}"
        )
    normalized = " ".join(result.stdout.split())
    if contains not in normalized:
        raise RuntimeError(
            f"command did not report {contains!r}:\n"
            f"{' '.join(command)}\n{result.stdout}"
        )


def configure(
    *,
    cmake: str,
    source: Path,
    build: Path,
    cxx_compiler: Path,
    forge_package: Path,
    contract_package: Path,
    guest: bool,
) -> None:
    command = [
        cmake,
        "-S",
        str(source),
        "-B",
        str(build),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug",
        "-DCMAKE_NO_SYSTEM_FROM_IMPORTED=ON",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
        f"-DForgeContract_DIR={contract_package}",
    ]
    if guest:
        command.extend(
            (
                f"-DCMAKE_TOOLCHAIN_FILE={contract_package / 'ForgeContractToolchain.cmake'}",
                f"-DFORGE_CONTRACT_SOURCE_ROOT={source.parent}",
            )
        )
    else:
        command.append(f"-DForge_DIR={forge_package}")
        if sys.platform == "darwin":
            sdk = run("xcrun", "--sdk", "macosx", "--show-sdk-path").strip()
            command.append(f"-DCMAKE_OSX_SYSROOT={sdk}")
    run(*command)


def build(cmake: str, directory: Path, *targets: str) -> None:
    run(
        cmake,
        "--build",
        str(directory),
        "--config",
        "Debug",
        "--target",
        *targets,
        "-j",
        "4",
    )


def artifact_set(directory: Path) -> dict[str, bytes]:
    result = {}
    for suffix in ("wasm", "abi", "contract.json"):
        path = directory / f"product.{suffix}"
        if not path.is_file() or path.stat().st_size == 0:
            raise RuntimeError(f"missing contract artifact: {path}")
        result[suffix] = path.read_bytes()
    return result


def verify_abi(data: bytes) -> None:
    abi = json.loads(data)
    action = next(
        item for item in abi["actions"] if item["name"] == "beginrev"
    )
    if action["type"] != "begin_revision":
        raise RuntimeError("named action did not use its payload type directly")
    record = next(
        item for item in abi["structs"] if item["name"] == "begin_revision"
    )
    if record["fields"] != [
        {"name": "workspace", "type": "workspace_id"},
        {"name": "inode", "type": "inode_id"},
        {"name": "size", "type": "uint64"},
    ]:
        raise RuntimeError("named action ABI fields are not direct")
    table = next(
        item for item in abi["tables"] if item["name"] == "revisions"
    )
    if table["type"] != "revision":
        raise RuntimeError("typed table did not use its persisted value directly")
    if any(item["name"] == "unusedaudit" for item in abi["tables"]):
        raise RuntimeError("unused imported typed row leaked into the contract ABI")
    bls_action = next(item for item in abi["actions"] if item["name"] == "blswire")
    if bls_action["type"] != "blswire":
        raise RuntimeError("BLS value action did not preserve its generated payload")
    bls_record = next(item for item in abi["structs"] if item["name"] == "blswire")
    if bls_record["fields"] != [{"name": "value", "type": "finalizer"}]:
        raise RuntimeError("BLS value action did not preserve its canonical finalizer")
    finalizer = next(item for item in abi["structs"] if item["name"] == "finalizer")
    if finalizer["fields"] != [
        {"name": "description", "type": "string"},
        {"name": "weight", "type": "uint64"},
        {"name": "public_key", "type": "bytes"},
    ]:
        raise RuntimeError("BLS finalizer did not use the canonical bytes ABI")


def verify_manifest(data: bytes) -> None:
    manifest = json.loads(data)
    if manifest["schema_version"] != 3:
        raise RuntimeError("contract runtime manifest is not schema 3")
    if "source_graph" in manifest:
        raise RuntimeError("runtime manifest contains removed source attestation")
    if len(manifest["wasm"]["sha256"]) != 64:
        raise RuntimeError("runtime manifest has no WASM digest")
    if len(manifest["abi"]["sha256"]) != 64:
        raise RuntimeError("runtime manifest has no ABI digest")


def validate_multi_config(
    *,
    cmake: str,
    source: Path,
    output: Path,
    contract_package: Path,
) -> None:
    build_directory = output / "multi-config"
    run(
        cmake,
        "-S",
        str(source / "multi_config"),
        "-B",
        str(build_directory),
        "-G",
        "Ninja Multi-Config",
        f"-DForgeContract_DIR={contract_package}",
    )
    run(
        cmake,
        "--build",
        str(build_directory),
        "--config",
        "Debug",
        "--target",
        "configuration_guest",
        "-j",
        "4",
    )
    artifact_root = build_directory / "configuration.guest" / "artifacts"
    debug_artifacts = artifact_root / "Debug"
    debug_abi_path = debug_artifacts / "configuration.abi"
    debug_abi_before_release = debug_abi_path.read_bytes()
    debug_actions = {
        item["name"] for item in json.loads(debug_abi_before_release)["actions"]
    }
    if "debugmode" not in debug_actions or "releasemode" in debug_actions:
        raise RuntimeError("Debug build did not produce its configuration-specific ABI")

    run(
        cmake,
        "--build",
        str(build_directory),
        "--config",
        "Release",
        "--target",
        "configuration_guest",
        "-j",
        "4",
    )
    release_artifacts = artifact_root / "Release"
    release_abi_path = release_artifacts / "configuration.abi"
    release_actions = {
        item["name"]
        for item in json.loads(release_abi_path.read_bytes())["actions"]
    }
    if "releasemode" not in release_actions or "debugmode" in release_actions:
        raise RuntimeError("Release build reused the Debug ABI output")
    if debug_abi_path.read_bytes() != debug_abi_before_release:
        raise RuntimeError("Release build overwrote the Debug ABI output")

    for configuration, directory in (
        ("Debug", debug_artifacts),
        ("Release", release_artifacts),
    ):
        if not (artifact_root / f"built-{configuration}.txt").is_file():
            raise RuntimeError(
                f"launcher did not forward the {configuration} configuration"
            )
        for suffix in ("wasm", "abi", "contract.json"):
            if not (directory / f"configuration.{suffix}").is_file():
                raise RuntimeError(
                    f"{configuration} contract artifact is missing: {suffix}"
                )
        properties = (
            build_directory / f"artifact-properties-{configuration}.txt"
        ).read_text(encoding="utf-8").splitlines()
        expected = [
            str(directory / "configuration.wasm"),
            str(directory / "configuration.abi"),
            str(directory / "configuration.contract.json"),
        ]
        if properties != expected:
            raise RuntimeError(
                f"{configuration} launcher properties are not configuration-specific"
            )

    commands = subprocess.run(
        (
            "ninja",
            "-C",
            str(build_directory / "configuration.guest"),
            "-f",
            "build-Release.ninja",
            "-t",
            "commands",
        ),
        check=True,
        capture_output=True,
        text=True,
    ).stdout.splitlines()
    compilation = next(
        (
            command
            for command in commands
            if "configuration.cpp" in command and "clang++" in command and " -c " in command
        ),
        "",
    )
    abigen = next(
        (
            command
            for command in commands
            if "/bin/abigen " in command and "/entry.cpp" in command
        ),
        "",
    )
    if not compilation or " -O3 " not in compilation or " -DNDEBUG " not in compilation:
        raise RuntimeError("Release guest compilation did not use the canonical profile")
    if " -g " in compilation:
        raise RuntimeError("Release guest compilation leaked Debug flags")
    if (
        not abigen
        or "--compiler-argument=-O3" not in abigen
        or "--compiler-argument=-DNDEBUG" not in abigen
    ):
        raise RuntimeError("Release Abigen invocation did not use the canonical profile")
    if "--compiler-argument=-g" in abigen:
        raise RuntimeError("Release Abigen invocation leaked Debug flags")


def write_negative_project(
    root: Path,
    *,
    cmake_body: str,
    modules: dict[str, str],
    contract: str | None = None,
) -> None:
    root.mkdir(parents=True)
    (root / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.31)
project(ForgeContractNegative LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ForgeContract CONFIG REQUIRED)
"""
        + cmake_body,
        encoding="utf-8",
    )
    for relative, source in modules.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(source, encoding="utf-8")
    if contract is not None:
        (root / "contract.cpp").write_text(contract, encoding="utf-8")


def validate_generated_project(
    *,
    cmake: str,
    contract_package: Path,
    output: Path,
) -> None:
    source = output / "generated-source"
    build_directory = output / "generated-build"
    source.mkdir(parents=True)
    (source / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.31)
project(ForgeContractGeneratedInputs LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(ForgeContract CONFIG REQUIRED)

set(generated_directory "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${generated_directory}")
add_custom_command(
   OUTPUT "${generated_directory}/request.cppm"
   COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/request.cppm.in"
      "${generated_directory}/request.cppm"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/request.cppm.in"
   VERBATIM
)
add_custom_command(
   OUTPUT "${generated_directory}/entry.cpp"
   COMMAND
      "${CMAKE_COMMAND}" -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp.in"
      "${generated_directory}/entry.cpp"
   DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/entry.cpp.in"
   VERBATIM
)

forge_add_contract_library(
   generated_protocol
   ID fixture.generated.protocol
   MODULE_BASE_DIRS "${generated_directory}"
   MODULE_SOURCES "${generated_directory}/request.cppm"
   PUBLIC_LIBRARIES Forge::forge_chain_protocol
)
forge_add_contract(
   generated
   SOURCES "${generated_directory}/entry.cpp"
   LIBRARIES generated_protocol
)
""",
        encoding="utf-8",
    )
    (source / "request.cppm.in").write_text(
        """module;
#include <cstdint>
export module fixture.generated.protocol;
export import forge.chain.protocol.action;

export namespace fixture::generated {

struct request {
   std::uint64_t value = 0;

   static constexpr forge::chain::protocol::action_name get_name() {
      return forge::chain::protocol::make_name("generate");
   }
};

} // namespace fixture::generated
""",
        encoding="utf-8",
    )
    (source / "entry.cpp.in").write_text(
        """import fixture.generated.protocol;
import forge.contract;

class [[forge::contract("generated")]] generated final
   : public forge::contract::context {
 public:
   using context::context;

   [[forge::action]] void apply(fixture::generated::request) {}
};
""",
        encoding="utf-8",
    )

    run(
        cmake,
        "-S",
        str(source),
        "-B",
        str(build_directory),
        "-G",
        "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={contract_package / 'ForgeContractToolchain.cmake'}",
        f"-DForgeContract_DIR={contract_package}",
        f"-DFORGE_CONTRACT_SOURCE_ROOT={source}",
    )
    run(
        cmake,
        "--build",
        str(build_directory),
        "--target",
        "generated_artifacts",
        "-j",
        "4",
    )
    abi = json.loads(
        (build_directory / "artifacts" / "generated.abi").read_text(
            encoding="utf-8"
        )
    )
    action = next(item for item in abi["actions"] if item["name"] == "generate")
    if action["type"] != "request":
        raise RuntimeError("generated named action did not preserve direct ABI layout")


def validate_negative_projects(
    *,
    cmake: str,
    cxx_compiler: Path,
    contract_package: Path,
    output: Path,
) -> None:
    source_root = output / "negative-source"
    build_root = output / "negative-build"

    host_source = output / "host-source"
    host_project = host_source / "project"
    host_library = host_source / "shared"
    host_project.mkdir(parents=True)
    host_library.mkdir(parents=True)
    (host_project / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.31)
project(ForgeContractHostSibling LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(ForgeContract CONFIG REQUIRED)
add_subdirectory(../shared shared-build)
""",
        encoding="utf-8",
    )
    (host_library / "CMakeLists.txt").write_text(
        """forge_add_contract_library(
   host_sibling ID host.sibling
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/value.cppm
)
""",
        encoding="utf-8",
    )
    (host_library / "include").mkdir()
    (host_library / "include" / "value.cppm").write_text(
        "export module host.sibling;\n",
        encoding="utf-8",
    )
    host_command = [
        cmake,
        "-S",
        str(host_project),
        "-B",
        str(output / "host-build"),
        "-G",
        "Ninja",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
        f"-DForgeContract_DIR={contract_package}",
    ]
    if sys.platform == "darwin":
        sdk = run("xcrun", "--sdk", "macosx", "--show-sdk-path").strip()
        host_command.append(f"-DCMAKE_OSX_SYSROOT={sdk}")
    run(*host_command)

    duplicate = source_root / "duplicate-id"
    write_negative_project(
        duplicate,
        cmake_body="""
forge_add_contract_library(
   negative_first ID negative.duplicate
   MODULE_BASE_DIRS first
   MODULE_SOURCES first/value.cppm
)
forge_add_contract_library(
   negative_second ID negative.duplicate
   MODULE_BASE_DIRS second
   MODULE_SOURCES second/value.cppm
)
""",
        modules={
            "first/value.cppm": "export module negative.first;\n",
            "second/value.cppm": "export module negative.second;\n",
        },
    )

    host_only = source_root / "host-only"
    write_negative_project(
        host_only,
        cmake_body="""
add_library(host_only INTERFACE)
forge_add_contract_library(
   negative_protocol ID negative.host_only
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
   PUBLIC_LIBRARIES host_only
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    forward_edge = source_root / "forward-edge"
    write_negative_project(
        forward_edge,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.forward
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
   PUBLIC_LIBRARIES dependency_declared_later
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    mutated_target = source_root / "mutated-target"
    write_negative_project(
        mutated_target,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.mutated
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
target_compile_definitions(negative_protocol PRIVATE MUTATED_AFTER_DECLARATION=1)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    mutated_dependencies = source_root / "mutated-dependencies"
    write_negative_project(
        mutated_dependencies,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.mutated_dependencies
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
target_link_libraries(negative_protocol PRIVATE Forge::forge_raw)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    mutated_source = source_root / "mutated-source"
    write_negative_project(
        mutated_source,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.mutated_source
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
set_source_files_properties(
   include/protocol.cppm
   TARGET_DIRECTORY negative_protocol
   PROPERTIES COMPILE_DEFINITIONS MUTATED_SOURCE_AFTER_DECLARATION=1
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    mutated_module_set = source_root / "mutated-module-set"
    write_negative_project(
        mutated_module_set,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.mutated_module_set
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
target_sources(
   negative_protocol
   PUBLIC
      FILE_SET forge_contract_modules TYPE CXX_MODULES
      BASE_DIRS include
      FILES include/extra.cppm
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n",
            "include/extra.cppm": "export module negative.extra;\n",
        },
    )

    mutated_precompiled_header = source_root / "mutated-precompiled-header"
    write_negative_project(
        mutated_precompiled_header,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.mutated_precompiled_header
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
target_precompile_headers(
   negative_protocol PRIVATE include/profile.hxx
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n",
            "include/profile.hxx": "#define MUTATED_PCH_PROFILE 1\n",
        },
    )

    mutated_launcher = source_root / "mutated-launcher"
    write_negative_project(
        mutated_launcher,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.mutated_launcher
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
set_property(
   TARGET negative_protocol
   PROPERTY CXX_COMPILER_LAUNCHER "${CMAKE_COMMAND};-E;env"
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    custom_configuration = source_root / "custom-configuration"
    write_negative_project(
        custom_configuration,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.custom_configuration
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    directory_launcher = source_root / "directory-launcher"
    write_negative_project(
        directory_launcher,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.directory_launcher
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
set_property(
   DIRECTORY PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_COMMAND};-E;env"
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    global_launcher = source_root / "global-launcher"
    write_negative_project(
        global_launcher,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.global_launcher
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
set_property(
   GLOBAL PROPERTY RULE_LAUNCH_COMPILE "${CMAKE_COMMAND};-E;env"
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    deferred_mutation = source_root / "deferred-mutation"
    write_negative_project(
        deferred_mutation,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.deferred_mutation
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
cmake_language(
   DEFER CALL set_property
   TARGET negative_protocol APPEND
   PROPERTY COMPILE_DEFINITIONS DEFERRED_MUTATION=1
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    nested_deferred_mutation = source_root / "nested-deferred-mutation"
    write_negative_project(
        nested_deferred_mutation,
        cmake_body="""
function(schedule_nested_mutation)
   cmake_language(
      DEFER CALL set_property
      TARGET negative_protocol APPEND
      PROPERTY COMPILE_DEFINITIONS NESTED_DEFERRED_MUTATION=1
   )
endfunction()

forge_add_contract_library(
   negative_protocol ID negative.nested_deferred_mutation
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
cmake_language(DEFER CALL schedule_nested_mutation)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    named_nested_deferred_mutation = (
        source_root / "named-nested-deferred-mutation"
    )
    write_negative_project(
        named_nested_deferred_mutation,
        cmake_body="""
function(schedule_named_nested_mutation)
   cmake_language(DEFER GET_CALL_IDS pending_calls)
   foreach(call_id IN LISTS pending_calls)
      cmake_language(DEFER GET_CALL "${call_id}" deferred_call)
      list(GET deferred_call 0 command)
      if(command STREQUAL "_forge_contract_validate_guest_targets_final")
         set(target_validation_id "${call_id}")
         break()
      endif()
   endforeach()
   if(NOT target_validation_id)
      message(FATAL_ERROR "Forge target validator was not scheduled")
   endif()
   cmake_language(
      DEFER ID "${target_validation_id}"
      CALL set_property
      TARGET negative_protocol APPEND
      PROPERTY COMPILE_DEFINITIONS NAMED_NESTED_DEFERRED_MUTATION=1
   )
endfunction()

forge_add_contract_library(
   negative_protocol ID negative.named_nested_deferred_mutation
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
cmake_language(DEFER CALL schedule_named_nested_mutation)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    directory_profile = source_root / "directory-profile"
    write_negative_project(
        directory_profile,
        cmake_body="""
add_compile_definitions(UNDECLARED_GUEST_PROFILE=1)
forge_add_contract_library(
   negative_protocol ID negative.directory_profile
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    implicit_current_directory = source_root / "implicit-current-directory"
    write_negative_project(
        implicit_current_directory,
        cmake_body="""
set(CMAKE_INCLUDE_CURRENT_DIR ON)
forge_add_contract_library(
   negative_protocol ID negative.implicit_current_directory
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    changed_standard_includes = source_root / "changed-standard-includes"
    write_negative_project(
        changed_standard_includes,
        cmake_body="""
list(APPEND CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR}")
forge_add_contract_library(
   negative_protocol ID negative.changed_standard_includes
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    changed_source_root = source_root / "changed-source-root"
    write_negative_project(
        changed_source_root,
        cmake_body="""
set(FORGE_CONTRACT_SOURCE_ROOT "${CMAKE_CURRENT_BINARY_DIR}")
forge_add_contract_library(
   negative_protocol ID negative.changed_source_root
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    changed_dialect = source_root / "changed-dialect"
    write_negative_project(
        changed_dialect,
        cmake_body="""
set(CMAKE_CXX_EXTENSIONS ON)
forge_add_contract_library(
   negative_protocol ID negative.changed_dialect
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    late_profile = source_root / "late-profile"
    write_negative_project(
        late_profile,
        cmake_body="""
forge_add_contract_library(
   negative_protocol ID negative.late_profile
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
set(CMAKE_CXX_FLAGS_RELEASE "-O0")
""",
        modules={
            "include/protocol.cppm": "export module negative.protocol;\n"
        },
    )

    nested_late_profile = source_root / "nested-late-profile"
    (nested_late_profile / "library" / "include").mkdir(parents=True)
    (nested_late_profile / "CMakeLists.txt").write_text(
        """cmake_minimum_required(VERSION 3.31)
project(ForgeContractNestedLateProfile LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(ForgeContract CONFIG REQUIRED)
add_subdirectory(library)
set(CMAKE_CXX_FLAGS_RELEASE "-O0" CACHE STRING "" FORCE)
""",
        encoding="utf-8",
    )
    (nested_late_profile / "library" / "CMakeLists.txt").write_text(
        """forge_add_contract_library(
   negative_protocol ID negative.nested_late_profile
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/protocol.cppm
)
""",
        encoding="utf-8",
    )
    (
        nested_late_profile / "library" / "include" / "protocol.cppm"
    ).write_text(
        "export module negative.protocol;\n",
        encoding="utf-8",
    )

    external_input = source_root / "external-input"
    write_negative_project(
        external_input,
        cmake_body="""
forge_add_contract(
   negative
   SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
   SOURCES ../external.cpp
)
""",
        modules={},
    )
    (source_root / "external.cpp").write_text(
        "class external_input {};\n",
        encoding="utf-8",
    )

    missing_input = source_root / "missing-input"
    write_negative_project(
        missing_input,
        cmake_body="""
forge_add_contract(
   missing
   SOURCES missing.cpp
)
""",
        modules={},
    )

    table_mismatch = source_root / "table-name-mismatch"
    write_negative_project(
        table_mismatch,
        cmake_body="""
forge_add_contract_library(
   mismatched_state ID negative.table_mismatch
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/state.cppm
   PUBLIC_LIBRARIES Forge::forge_contract_runtime
)
forge_add_contract(
   mismatch
   SOURCES contract.cpp
   LIBRARIES mismatched_state
)
""",
        modules={
            "include/state.cppm": """module;
#include <cstdint>
export module negative.table_mismatch;
export import forge.contract;
export import forge.contract.multi_index;
export namespace negative {
using forge::chain::protocol::literals::operator""_n;
struct row {
   std::uint64_t id = 0;
   static constexpr forge::chain::protocol::table_name get_table_name() {
      return forge::chain::protocol::make_name("expected");
   }
   std::uint64_t primary_key() const { return id; }
};
using rows = forge::contract::multi_index<"actual"_n, row>;
}
""",
        },
        contract="""import negative.table_mismatch;
class [[forge::contract("mismatch")]] mismatch final
   : public forge::contract::context {
 public:
   using context::context;
   [[forge::action]] void create() {
      negative::rows rows{get_self(), get_self().value};
      rows.emplace(get_self(), [](auto& row) { row.id = 1; });
   }
};
""",
    )

    definition_cases = []
    for name, definitions, diagnostic in (
        ("definition-prefix", '"-DVALUE=1"', "unsupported compile definition"),
        ("definition-function", '"VALUE(x)=x"', "unsupported compile definition"),
        ("definition-generator", '"VALUE=$<CONFIG>"', "unsupported compile definition"),
        ("definition-list", '"VALUE=a;b"', "unsupported compile definition"),
        ("definition-hash", '"VALUE=#"', "unsupported compile definition"),
        ("definition-newline", '"VALUE=a\\nb"', "unsupported compile definition"),
        ("definition-sdk", '"FORGE_CONTRACT_GUEST=0"', "reserved compile definition"),
        ("definition-dispatch", '"FORGE_CONTRACT_DEFER_EOSIO_DISPATCH=0"', "reserved compile definition"),
        ("definition-profile", '"NDEBUG=0"', "reserved compile definition"),
        ("definition-compiler", '"__cplusplus=0"', "reserved compile definition"),
        ("definition-runtime", '"_LIBCPP_HAS_LOCALIZATION=0"', "reserved compile definition"),
        ("definition-duplicate", '"VALUE=1" "VALUE=2"', "duplicate compile definition"),
        ("definition-empty", "", "COMPILE_DEFINITIONS requires values"),
    ):
        definition_source = source_root / name
        write_negative_project(
            definition_source,
            cmake_body=f"forge_add_contract(negative SOURCES contract.cpp COMPILE_DEFINITIONS {definitions})\n",
            modules={},
            contract="class negative {};\n",
        )
        definition_cases.append((definition_source, diagnostic))

    definition_mutation = source_root / "definition-post-declaration"
    write_negative_project(
        definition_mutation,
        cmake_body="""
forge_add_contract(negative SOURCES contract.cpp COMPILE_DEFINITIONS VALUE=1)
target_compile_definitions(negative PRIVATE UNDECLARED_VALUE=2)
""",
        modules={},
        contract="class negative {};\n",
    )
    definition_cases.append((definition_mutation, "changed property: COMPILE_DEFINITIONS"))

    toolchain = contract_package / "ForgeContractToolchain.cmake"
    cases = (
        (duplicate, "duplicate Forge Contract owner ID"),
        (host_only, "contract dependency is not guest-compatible"),
        (forward_edge, "unknown Contract SDK dependency target"),
        (mutated_target, "post-declaration target mutation is unsupported"),
        (
            mutated_dependencies,
            "changed property: LINK_LIBRARIES",
        ),
        (
            mutated_source,
            "changed source property: COMPILE_DEFINITIONS",
        ),
        (
            mutated_module_set,
            "changed property: CXX_MODULE_SET_forge_contract_modules",
        ),
        (
            mutated_precompiled_header,
            "changed property: PRECOMPILE_HEADERS",
        ),
        (
            mutated_launcher,
            "changed property: CXX_COMPILER_LAUNCHER",
        ),
        (
            directory_launcher,
            "directory RULE_LAUNCH_COMPILE are unsupported",
        ),
        (
            global_launcher,
            "global RULE_LAUNCH_COMPILE is unsupported",
        ),
        (
            deferred_mutation,
            "changed property: COMPILE_DEFINITIONS",
        ),
        (
            nested_deferred_mutation,
            "changed property: COMPILE_DEFINITIONS",
        ),
        (
            named_nested_deferred_mutation,
            "changed property: COMPILE_DEFINITIONS",
        ),
        (directory_profile, "directory COMPILE_DEFINITIONS are unsupported"),
        (
            implicit_current_directory,
            "CMAKE_INCLUDE_CURRENT_DIR and "
            "CMAKE_INCLUDE_CURRENT_DIR_IN_INTERFACE must remain disabled",
        ),
        (
            changed_standard_includes,
            "CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES must remain",
        ),
        (
            changed_source_root,
            "FORGE_CONTRACT_SOURCE_ROOT changed after the guest SDK fixed",
        ),
        (changed_dialect, "require strict C++23"),
        (late_profile, "CMAKE_CXX_FLAGS_RELEASE must remain"),
        (nested_late_profile, "CMAKE_CXX_FLAGS_RELEASE must remain"),
        (external_input, "contract source is outside its declared root"),
        (
            missing_input,
            "does not exist and is not a declared generated output",
        ),
    )
    for source, expected in (*cases, *definition_cases):
        run_failure(
            cmake,
            "-S",
            str(source),
            "-B",
            str(build_root / source.name),
            "-G",
            "Ninja",
            f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
            f"-DForgeContract_DIR={contract_package}",
            contains=expected,
        )
    run_failure(
        cmake,
        "-S",
        str(custom_configuration),
        "-B",
        str(build_root / custom_configuration.name),
        "-G",
        "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DForgeContract_DIR={contract_package}",
        "-DCMAKE_BUILD_TYPE=ASan",
        "-DCMAKE_CXX_FLAGS_ASAN=-DFEATURE=1",
        contains="unsupported Forge Contract guest configuration: ASan",
    )
    run_failure(
        cmake,
        "-S",
        str(mutated_target),
        "-B",
        str(build_root / "command-line-profile"),
        "-G",
        "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DForgeContract_DIR={contract_package}",
        "-DCMAKE_CXX_FLAGS=-DUNDECLARED_GUEST_PROFILE=1",
        contains="CMAKE_CXX_FLAGS is owned by the Forge Contract guest toolchain",
    )

    mismatch_build = build_root / table_mismatch.name
    run(
        cmake,
        "-S",
        str(table_mismatch),
        "-B",
        str(mismatch_build),
        "-G",
        "Ninja",
        f"-DCMAKE_TOOLCHAIN_FILE={toolchain}",
        f"-DForgeContract_DIR={contract_package}",
    )
    run_failure(
        cmake,
        "--build",
        str(mismatch_build),
        "-j",
        "4",
        contains="table name does not match",
    )


def validate(
    *,
    cmake: str,
    cxx_compiler: Path,
    forge_package: Path,
    contract_package: Path,
    forge_source_root: Path | None = None,
    source: Path,
    output: Path,
) -> None:
    shutil.rmtree(output, ignore_errors=True)
    output.mkdir(parents=True)

    producer = source / "producer"
    host_build = output / "host"
    direct_build = output / "direct-guest"

    configure(
        cmake=cmake,
        source=producer,
        build=host_build,
        cxx_compiler=cxx_compiler,
        forge_package=forge_package,
        contract_package=contract_package,
        guest=False,
    )
    build(
        cmake,
        host_build,
        "product_protocol_host_tests",
        "product_protocol_vm_tests",
    )
    run(str(host_build / "product_protocol_host_tests"))
    run(str(host_build / "product_protocol_vm_tests"))

    if forge_source_root is not None:
        source_helper_build = output / "source-helper-host"
        source_helper_command = [
            cmake,
            "-S",
            str(producer),
            "-B",
            str(source_helper_build),
            "-G",
            "Ninja",
            "-DCMAKE_BUILD_TYPE=Debug",
            "-DCMAKE_NO_SYSTEM_FROM_IMPORTED=ON",
            f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
            f"-DForge_DIR={forge_package}",
            f"-DForgeContract_DIR={contract_package}",
            f"-DPRODUCT_FORGE_SOURCE_ROOT={forge_source_root}",
        ]
        if sys.platform == "darwin":
            sdk = run("xcrun", "--sdk", "macosx", "--show-sdk-path").strip()
            source_helper_command.append(f"-DCMAKE_OSX_SYSROOT={sdk}")
        run(*source_helper_command)
        build(cmake, source_helper_build, "product_protocol_vm_tests")
        run(str(source_helper_build / "product_protocol_vm_tests"))
    install_prefix = output / "native-install"
    run(
        cmake,
        "--install",
        str(host_build),
        "--prefix",
        str(install_prefix),
    )
    exported_targets = (
        install_prefix
        / "lib"
        / "cmake"
        / "ProductProtocol"
        / "ProductProtocolTargets.cmake"
    )
    if not exported_targets.is_file():
        raise RuntimeError("native contract library export is missing")
    exported_text = exported_targets.read_text(encoding="utf-8")
    if str(producer) in exported_text or str(host_build) in exported_text:
        raise RuntimeError("native contract library export contains build paths")
    relocated_prefix = output / "native-relocated"
    shutil.move(install_prefix, relocated_prefix)
    consumer_build = output / "native-consumer"
    consumer_command = [
        cmake,
        "-S",
        str(source / "native_consumer"),
        "-B",
        str(consumer_build),
        "-G",
        "Ninja",
        "-DCMAKE_BUILD_TYPE=Debug",
        f"-DCMAKE_CXX_COMPILER={cxx_compiler}",
        f"-DForge_DIR={forge_package}",
        (
            "-DPRODUCT_PROTOCOL_TARGETS="
            f"{relocated_prefix / 'lib' / 'cmake' / 'ProductProtocol' / 'ProductProtocolTargets.cmake'}"
        ),
    ]
    if sys.platform == "darwin":
        sdk = run("xcrun", "--sdk", "macosx", "--show-sdk-path").strip()
        consumer_command.append(f"-DCMAKE_OSX_SYSROOT={sdk}")
    run(*consumer_command)
    build(cmake, consumer_build, "product_protocol_installed_consumer")
    run(str(consumer_build / "product_protocol_installed_consumer"))

    configure(
        cmake=cmake,
        source=producer / "guest",
        build=direct_build,
        cxx_compiler=cxx_compiler,
        forge_package=forge_package,
        contract_package=contract_package,
        guest=True,
    )
    build(cmake, direct_build, "product_artifacts")

    helper = artifact_set(host_build / "product.guest" / "artifacts")
    direct = artifact_set(direct_build / "artifacts")
    for name in helper:
        if helper[name] != direct[name]:
            raise RuntimeError(
                f"direct and launcher contract artifacts differ: {name}"
            )
    verify_abi(direct["abi"])
    verify_manifest(direct["contract.json"])
    validate_multi_config(
        cmake=cmake,
        source=source,
        output=output,
        contract_package=contract_package,
    )
    validate_generated_project(
        cmake=cmake,
        contract_package=contract_package,
        output=output,
    )
    validate_negative_projects(
        cmake=cmake,
        cxx_compiler=cxx_compiler,
        contract_package=contract_package,
        output=output,
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cmake", required=True)
    parser.add_argument("--cxx-compiler", required=True, type=Path)
    parser.add_argument("--forge-package", required=True, type=Path)
    parser.add_argument("--contract-package", required=True, type=Path)
    parser.add_argument("--forge-source-root", type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    validate(
        cmake=args.cmake,
        cxx_compiler=args.cxx_compiler,
        forge_package=args.forge_package,
        contract_package=args.contract_package,
        forge_source_root=(
            args.forge_source_root.resolve()
            if args.forge_source_root is not None
            else None
        ),
        source=args.source.resolve(),
        output=args.output.resolve(),
    )


if __name__ == "__main__":
    main()
