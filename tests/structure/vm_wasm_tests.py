#!/usr/bin/env python3

"""Narrow regression for the two owned WASM leaves; retired root stays forbidden."""

import tempfile
import unittest
from pathlib import Path

from check_structure import (
   LEGACY_VM_WASM_CMAKE_IDENTITIES,
   LEGACY_VM_WASM_SOURCE_IDENTITIES,
   check_contract_tooling_boundaries,
   check_layout,
   check_pairing,
   check_vm_wasm_interpret_boundaries,
)


class WasmLeafTests(unittest.TestCase):
   @staticmethod
   def private_fixture(root):
      component = root / "libraries/vm/wasm/interpret"
      include = component / "include/forge/vm/wasm/interpret"
      include.mkdir(parents=True)
      (component / "details").mkdir()
      for name in ("host_function.hpp", "opcode_macros.hpp"):
         (include / name).write_text("#pragma once\n#define FORGE_VM_WASM_INTERPRET_TEST 1\n")
      (component / "details/helper.hxx").write_text("#pragma once\nvoid helper();\n")
      (component / "helper.cpp").write_text('#include "details/helper.hxx"\nvoid helper() {}\n')
      (component / "CMakeLists.txt").write_text("add_library(forge_vm_wasm_interpret STATIC helper.cpp)\n")
      return component, include

   @staticmethod
   def private_errors(root):
      errors = []
      check_layout(root, errors)
      check_pairing(root, errors)
      check_vm_wasm_interpret_boundaries(root, errors)
      return errors

   def test_paired_private_component_remains_private_and_passes_shared_checks(self):
      with tempfile.TemporaryDirectory(prefix="forge-wasm-private-") as directory:
         root = Path(directory)
         component, _ = self.private_fixture(root)
         with (component / "CMakeLists.txt").open("a") as cmake:
            cmake.write('target_include_directories(forge_vm_wasm_interpret PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/details")\n')
            cmake.write('target_include_directories(forge_vm_wasm_interpret PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")\n')
            cmake.write('target_include_directories(forge_vm_wasm_interpret PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>")\n')
         (root / "CMakeLists.txt").write_text('list(APPEND FORGE_INSTALL_INCLUDE_DIRS libraries/vm/wasm/interpret/include/forge)\n')
         self.assertEqual(self.private_errors(root), [])
         (component / "details/helper.hxx").unlink()
         self.assertTrue(any("implementation needs an exact" in error for error in self.private_errors(root)))

   def test_private_layout_and_retired_public_headers_stay_forbidden(self):
      mutations = (
         ("details/helper.hpp", "details/ headers must use .hxx"),
         ("include/forge/vm/wasm/interpret/helper.hxx", "private .hxx must live under details/"),
         ("include/forge/vm/wasm/interpret/legacy_allocator.hpp", "unexpected public headers"),
      )
      for relative, expected in mutations:
         with self.subTest(relative=relative), tempfile.TemporaryDirectory(prefix="forge-wasm-layout-") as directory:
            root = Path(directory)
            component, _ = self.private_fixture(root)
            (component / relative).write_text("#pragma once\n")
            self.assertTrue(any(expected in error for error in self.private_errors(root)))

   def test_public_vm_module_must_not_include_private_source_header(self):
      with tempfile.TemporaryDirectory(prefix="forge-wasm-public-") as directory:
         root = Path(directory)
         _, include = self.private_fixture(root)
         (include / "api.cppm").write_text('module;\n#include "details/helper.hxx"\nexport module forge.vm.wasm.interpret.api;\n')
         self.assertTrue(any("public VM module includes a private source header" in error
                             for error in self.private_errors(root)))

   def test_private_headers_must_not_be_installed_or_exported(self):
      registrations = (
         ('install(FILES details/helper.hxx DESTINATION include)\n', "must not be installed"),
         ('install(DIRECTORY details/ DESTINATION include)\n', "must not be installed"),
         ('install(DIRECTORY libraries/vm/wasm/interpret DESTINATION include)\n', "must not be installed"),
         ('install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/" DESTINATION include)\n', "must not be installed"),
         ('set(FORGE_INSTALL_INCLUDE_DIRS libraries/vm/wasm/interpret/details)\n', "must not be installed"),
         ('list(APPEND FORGE_INSTALL_INCLUDE_DIRS libraries/vm/wasm/interpret/details)\n', "must not be installed"),
         ('list(APPEND FORGE_INSTALL_INCLUDE_DIRS libraries/vm/wasm/interpret)\n', "must not be installed"),
      ) + tuple(
         (f'target_{command}(forge_vm_wasm_interpret {visibility} {path})\n', "public usage requirements")
         for visibility in ("PUBLIC", "INTERFACE")
         for command, path in (
            ("sources", "details/helper.hxx"),
            ("include_directories", "details"),
            ("include_directories", '"${CMAKE_CURRENT_SOURCE_DIR}"'),
            ("include_directories", "libraries/vm/wasm/interpret/"),
            ("include_directories", '"$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"'),
         )
      )
      for registration, expected in registrations:
         with self.subTest(registration=registration), tempfile.TemporaryDirectory(prefix="forge-wasm-install-") as directory:
            root = Path(directory)
            component, _ = self.private_fixture(root)
            for cmake in (component / "CMakeLists.txt", root / "CMakeLists.txt"):
               original = cmake.read_text() if cmake.exists() else ""
               cmake.write_text(original + registration)
               self.assertTrue(any(expected in error for error in self.private_errors(root)))
               cmake.write_text(original)

   def test_tooling_allows_exact_compiler_leaf_without_root_facade(self):
      with tempfile.TemporaryDirectory(prefix="forge-wasm-tooling-") as directory:
         root = Path(directory)
         family = root / "libraries/tooling"
         for leaf in ("abi", "attributes", "validation", "manifest", "testing", "wasm_compiler"):
            (family / leaf).mkdir(parents=True)
         errors = []
         check_contract_tooling_boundaries(root, [], errors)
         self.assertEqual(errors, [])
         aggregate = family / "CMakeLists.txt"
         aggregate.write_text("add_library(forge_tooling INTERFACE)\n")
         errors = []
         check_contract_tooling_boundaries(root, [], errors)
         self.assertTrue(any("must not define an aggregate target" in error for error in errors))
         (family / "wasm_compiler_alias").mkdir()
         errors = []
         check_contract_tooling_boundaries(root, [], errors)
         self.assertTrue(any("family root contains: wasm_compiler_alias" in error for error in errors))

   def test_exact_siblings_pass_source_and_cmake_identities(self):
      for leaf, macro in (("interpret", "INTERPRET"), ("optimizing", "OPTIMIZING")):
         source = (f"import forge.vm.wasm.{leaf}.types;\n"
                   f"namespace forge::vm::wasm::{leaf} {{ }}\n"
                   f"#include <forge/vm/wasm/{leaf}/macros.hpp>\n"
                   f"#define FORGE_VM_WASM_{macro}_TEST 1\n")
         cmake = f"add_library(forge_vm_wasm_{leaf} STATIC real.cpp)\nset(component vm_wasm_{leaf})\n"
         self.assertFalse(any(pattern.search(source) for pattern, _ in LEGACY_VM_WASM_SOURCE_IDENTITIES))
         self.assertFalse(any(pattern.search(cmake) for pattern, _ in LEGACY_VM_WASM_CMAKE_IDENTITIES))

   def test_old_root_and_unapproved_siblings_stay_forbidden(self):
      for source in ("import forge.vm.wasm;", "import forge.vm.wasm.interpreter;",
                     "import forge.vm.wasm.optimizing_alias.types;", "namespace forge::vm::wasm { }",
                     "namespace forge::vm::wasm::optimizing_alias { }", "#include <forge/vm/wasm/types.hpp>",
                     "#include <forge/vm/wasm/optimizing_alias/types.hpp>", "#define FORGE_VM_WASM_TEST 1",
                     "#define FORGE_VM_WASM_OPTIMIZINGALIAS_TEST 1"):
         self.assertTrue(any(pattern.search(source) for pattern, _ in LEGACY_VM_WASM_SOURCE_IDENTITIES), source)
      for cmake in ("add_library(forge_vm_wasm INTERFACE)", "add_library(forge_vm_wasm_optimized INTERFACE)",
                    "set(component vm_wasm)", "set(component vm_wasm_aot)",
                    "include_directories(libraries/vm/wasm/include)"):
         self.assertTrue(any(pattern.search(cmake) for pattern, _ in LEGACY_VM_WASM_CMAKE_IDENTITIES), cmake)

   def test_family_allows_only_two_real_leaves_and_no_root_facade(self):
      with tempfile.TemporaryDirectory(prefix="forge-wasm-leaves-") as directory:
         root = Path(directory)
         family = root / "libraries/vm/wasm"
         include = family / "interpret/include/forge/vm/wasm/interpret"
         include.mkdir(parents=True)
         for name in ("host_function.hpp", "opcode_macros.hpp"):
            (include / name).write_text("#pragma once\n#define FORGE_VM_WASM_INTERPRET_TEST 1\n")
         (family / "optimizing").mkdir()
         errors = []
         check_vm_wasm_interpret_boundaries(root, errors)
         self.assertEqual(errors, [])
         (family / "include").mkdir()
         errors = []
         check_vm_wasm_interpret_boundaries(root, errors)
         self.assertTrue(any("family root contains: include" in error for error in errors))


if __name__ == "__main__":
   unittest.main()
