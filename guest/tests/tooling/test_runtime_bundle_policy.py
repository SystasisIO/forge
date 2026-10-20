#!/usr/bin/env python3

import ast
import os
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).parent))

import run_dual_target_fixtures  # noqa: E402
import run_relocation  # noqa: E402
import run_reproducibility  # noqa: E402
from run_relocation import is_linux_sdk_runtime  # noqa: E402


class RuntimeBundlePolicyTests(unittest.TestCase):
    def test_llvm_cxx_runtimes_are_sdk_owned(self) -> None:
        self.assertTrue(is_linux_sdk_runtime("/usr/lib/x86_64-linux-gnu/libstdc++.so.6"))
        self.assertTrue(is_linux_sdk_runtime("/usr/lib/llvm-22/lib/libc++.so.1"))
        self.assertTrue(is_linux_sdk_runtime("/usr/lib/llvm-22/lib/libc++abi.so.1"))
        self.assertTrue(is_linux_sdk_runtime("/lib/x86_64-linux-gnu/libunwind.so.1"))
        self.assertTrue(is_linux_sdk_runtime("/usr/lib/llvm-22/lib/libLLVM.so.22.1"))
        self.assertTrue(is_linux_sdk_runtime("/usr/lib/llvm-22/lib/libclang-cpp.so.22.1"))
        self.assertTrue(is_linux_sdk_runtime("/usr/lib/llvm-22/lib/liblldWasm.so.22.1"))

    def test_baseline_system_runtimes_remain_external(self) -> None:
        self.assertFalse(is_linux_sdk_runtime("/usr/lib/x86_64-linux-gnu/libc.so.6"))


class CanonicalBuildJobsTests(unittest.TestCase):
    def test_default_and_explicit_jobs(self) -> None:
        for value, expected in ((None, "4"), ("1", "1"), ("2", "2"), ("8", "8"), ("0002", "0002")):
            with self.subTest(value=value):
                environment = {} if value is None else {"CMAKE_BUILD_PARALLEL_LEVEL": value}
                with patch.dict(os.environ, environment, clear=True):
                    for module in (run_dual_target_fixtures, run_relocation, run_reproducibility):
                        self.assertEqual(module.build_jobs(), expected)

    def test_invalid_jobs_are_refused(self) -> None:
        for value in ("", "0", "000", "-1", "1.5", "native", " 2", "+2", "\u0662"):
            with self.subTest(value=value):
                with patch.dict(os.environ, {"CMAKE_BUILD_PARALLEL_LEVEL": value}, clear=True):
                    for module in (run_dual_target_fixtures, run_relocation, run_reproducibility):
                        with self.assertRaisesRegex(ValueError, "positive integer"):
                            module.build_jobs()

    def test_every_python_child_build_forwards_validated_jobs(self) -> None:
        for module, expected in ((run_dual_target_fixtures, 5), (run_relocation, 2), (run_reproducibility, 1)):
            with self.subTest(module=module.__name__):
                tree = ast.parse(Path(module.__file__).read_text(encoding="utf-8"))
                builds = []
                for node in ast.walk(tree):
                    if not isinstance(node, ast.Call):
                        continue
                    arguments = node.args
                    if len(arguments) == 1 and isinstance(arguments[0], ast.List):
                        arguments = arguments[0].elts
                    if not any(isinstance(arg, ast.Constant) and arg.value == "--build" for arg in arguments):
                        continue
                    option = next(i for i, arg in enumerate(arguments) if isinstance(arg, ast.Constant) and arg.value == "-j")
                    jobs = arguments[option + 1]
                    self.assertIsInstance(jobs, ast.Call)
                    self.assertIsInstance(jobs.func, ast.Name)
                    self.assertEqual(jobs.func.id, "build_jobs")
                    self.assertEqual(jobs.args, [])
                    builds.append(node)
                self.assertEqual(len(builds), expected)


if __name__ == "__main__":
    unittest.main()
