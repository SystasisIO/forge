#!/usr/bin/env python3

import tempfile
import unittest
from pathlib import Path

from check_structure import check_vm_wasm_execution_boundaries


class WasmFamilyTests(unittest.TestCase):
   def setUp(self):
      directory = tempfile.TemporaryDirectory(prefix="forge-wasm-layout-")
      self.addCleanup(directory.cleanup)
      self.root = Path(directory.name)
      self.write("execution/CMakeLists.txt", "target_link_libraries(execution forge_chain_protocol)")
      self.write("execution/intrinsics.cpp", "FORGE_CONTRACT_INTRINSICS(bind)")
      self.write("execution/details/intrinsics.hxx", "// Private declarations")

   def write(self, path, source):
      destination = self.root / "libraries/vm/wasm" / path
      destination.parent.mkdir(parents=True, exist_ok=True)
      destination.write_text(source)

   def errors(self):
      errors = []
      check_vm_wasm_execution_boundaries(self.root, errors)
      return errors

   def test_separate_sdk_and_compiler_dependencies_pass(self):
      self.write("compiler/CMakeLists.txt", "find_package(LLVM REQUIRED)")
      self.write("execution/imports.cppm", "import forge.chain.protocol.name;")
      self.assertEqual(self.errors(), [])

   def test_reverse_imports_fail(self):
      for name in ("interpret", "optimizing", "compiler"):
         for dependency in ("chain.protocol", "vm.wasm.execution", "tooling.testing"):
            with self.subTest(name=name, dependency=dependency):
               self.write(f"{name}/probe.cppm", f"import forge.{dependency}.types;")
               self.assertTrue(any("generic VM must not import" in error for error in self.errors()))
               self.write(f"{name}/probe.cppm", "")

   def test_sdk_header_fails(self):
      self.write("interpret/probe.cpp", '#include <forge/contract/types.h>')
      self.assertTrue(any("generic VM must not import" in error for error in self.errors()))

   def test_reverse_target_dependency_fails(self):
      self.write("compiler/CMakeLists.txt", "target_link_libraries(compiler forge_vm_wasm_execution)")
      self.assertTrue(any("generic VM target must not link" in error for error in self.errors()))

   def test_runtime_compiler_dependency_fails(self):
      for dependency in ("LLVM", "clangBasic", "forge_vm_wasm_compiler"):
         with self.subTest(dependency=dependency):
            self.write("interpret/CMakeLists.txt", f"target_link_libraries(interpret {dependency})")
            self.assertTrue(any("runtime VM must not acquire" in error for error in self.errors()))

   def test_explicit_optional_jit_dependency_passes(self):
      self.write("interpret/CMakeLists.txt", """
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   find_package(LLVM REQUIRED)
   if(NOT LLVM_ENABLE_RTTI)
      message(FATAL_ERROR "LLVM RTTI required")
   endif()
   target_link_libraries(interpret PRIVATE LLVM)
endif()
""")
      self.assertEqual(self.errors(), [])

   def test_optional_jit_else_does_not_hide_default_dependency(self):
      self.write("interpret/CMakeLists.txt", """
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   target_link_libraries(interpret PRIVATE LLVM)
else()
   target_link_libraries(interpret PRIVATE LLVM)
endif()
""")
      self.assertTrue(any("runtime VM must not acquire" in error for error in self.errors()))

   def test_optional_jit_does_not_allow_compiler_or_reverse_dependencies(self):
      for dependency in ("forge_vm_wasm_compiler", "clangBasic", "forge_vm_wasm_execution"):
         with self.subTest(dependency=dependency):
            self.write("interpret/CMakeLists.txt", f"""
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   target_link_libraries(interpret PRIVATE {dependency})
endif()
""")
            self.assertTrue(self.errors())

   def test_optional_jit_guard_does_not_apply_to_other_leaves(self):
      self.write("optimizing/CMakeLists.txt", """
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   target_link_libraries(optimizing PRIVATE LLVM)
endif()
""")
      self.assertTrue(any("runtime VM must not acquire" in error for error in self.errors()))

   def test_execution_runtime_policy_fails(self):
      self.write("execution/probe.cppm", "import forge.asio.runtime;")
      self.assertTrue(any("product/runtime policy" in error for error in self.errors()))

   def test_old_identity_fails(self):
      self.write("execution/probe.cppm", "export module forge.execution.wasm.profile;")
      self.assertTrue(any("old execution identity" in error for error in self.errors()))

   def test_old_dispatcher_fails(self):
      old = self.root / "libraries/execution/CMakeLists.txt"
      old.parent.mkdir(parents=True)
      old.write_text("add_subdirectory(wasm)")
      self.assertTrue(any("old family dispatcher" in error for error in self.errors()))


if __name__ == "__main__":
   unittest.main()
