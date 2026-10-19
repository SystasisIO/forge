"""Pure-Python evidence-selection regressions; no compiler/build invocation."""

import importlib.util
import json
import pathlib
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location("qualification_diagnostics", ROOT / ".github/scripts/cjose-qualification-diagnostics.py")
DIAGNOSTICS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(DIAGNOSTICS)


class DiagnosticsTests(unittest.TestCase):
    def test_feature_log_multiline_reason_and_redaction(self):
        text = ("-- begin\nCMake Error in /work/project/CMakeLists.txt:\n"
                "  The link interface of target cjose::cjose_static contains:\n\n"
                "    Jansson::Jansson\n  but the target was not found.\n"
                "  token=never-export-this\n-- finish\n")
        result = DIAGNOSTICS.static_errors(text, (("/work", "$WORK"),))
        self.assertTrue(any("Jansson::Jansson" in line for line in result))
        self.assertTrue(any("target was not found" in line for line in result))
        self.assertNotIn("never-export-this", json.dumps(result))
        self.assertNotIn("/work", json.dumps(result))

    def test_only_selected_try_compile_static_errors(self):
        text = '''events:
  -
    kind: "try_compile-v1"
    checks:
      - "Performing Test FORGE_CJOSE_HAS_EXTERNAL_SIGNING"
    buildResult:
      variable: "FORGE_CJOSE_HAS_EXTERNAL_SIGNING"
      stdout: |
        Run Build Command(s): /compiler SECRET=never-copy-command
        /work/source.c:3:2: error: The qualified external signing API is required
        3 | #error arbitrary source code is not an exported diagnostic
      exitCode: 1
  -
    kind: "try_compile-v1"
    buildResult:
      variable: "UNRELATED"
      stdout: |
        /work/other.c:1:1: error: must-not-copy-unrelated-event
      exitCode: 1
'''
        result = DIAGNOSTICS.try_compile_events(text, (("/work", "$WORK"),))
        self.assertEqual(len(result), 1)
        self.assertEqual(result[0]["exit"], 1)
        self.assertEqual(result[0]["diagnostics"], ["$WORK/source.c:3:2: error: The qualified external signing API is required"])
        self.assertNotIn("never-copy-command", json.dumps(result))

    def test_runtime_logs_never_selected(self):
        with tempfile.TemporaryDirectory() as directory:
            work = pathlib.Path(directory)
            (work / "raw").mkdir()
            (work / "evidence").mkdir()
            (work / "raw/candidate-feature-detection.log").write_text("CMake Error at static.cmake:2 (message):\n  Required symbol is absent.\n")
            (work / "raw/runtime.log").write_text("CMake Error at injected:\n  never-copy-runtime\n")
            DIAGNOSTICS.collect(work, ROOT)
            text = (work / "evidence/safe-build-diagnostics.json").read_text()
            self.assertIn("Required symbol is absent", text)
            self.assertNotIn("never-copy-runtime", text)
            self.assertEqual(json.loads((work / "evidence/feature-trycompile-diagnostics.json").read_text())[0]["log_present"], False)

    def test_sanitizer_compiler_check_linker_errors_and_safe_invocation(self):
        text = '''events:
  -
    kind: "try_compile-v1"
    buildResult:
      variable: "CMAKE_C_COMPILER_WORKS"
      stdout: |
        [1/2] /toolchain/bin/clang -O1 -g -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer -DSECRET=do-not-export -c testCCompiler.c
        Undefined symbols for architecture arm64:
          "_example_missing_symbol", referenced from:
              _main in testCCompiler.c.o
        ld: symbol(s) not found for architecture arm64
        clang: error: linker command failed with exit code 1
      exitCode: 1
'''
        result = DIAGNOSTICS.try_compile_events(text, ())
        self.assertEqual(result[0]["variable"], "CMAKE_C_COMPILER_WORKS")
        self.assertTrue(any("linker command failed" in line for line in result[0]["diagnostics"]))
        self.assertIn("undefined symbol: _example_missing_symbol", result[0]["diagnostics"])
        self.assertIn("-fsanitize=fuzzer-no-link,address,undefined", result[0]["compiler_invocations"][0]["flags"])
        self.assertNotIn("do-not-export", json.dumps(result))

    def test_abi_compiler_check_variable(self):
        text = '''events:
  -
    kind: "try_compile-v1"
    buildResult:
      variable: "CMAKE_C_ABI_COMPILED"
      stdout: |
        clang: error: unsupported option '-fsanitize=example'
      exitCode: 1
'''
        result = DIAGNOSTICS.try_compile_events(text, ())
        self.assertEqual(result[0]["exit"], 1)
        self.assertEqual(result[0]["variable"], "CMAKE_C_ABI_COMPILED")

    def test_dependency_control_and_explicit_macho_linker(self):
        text = '''events:
  -
    kind: "try_compile-v1"
    buildResult:
      variable: "FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES"
      stdout: |
        Configuration search path: /home/runner/.config/clang
        /toolchain/bin/clang -O2 -g -fuse-ld=/toolchain/bin/ld64.lld -o cmTC_example test.c
      exitCode: 0
'''
        result = DIAGNOSTICS.try_compile_events(text, ())
        self.assertEqual(result[0]["variable"], "FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES")
        self.assertEqual(result[0]["exit"], 0)
        self.assertEqual(len(result[0]["compiler_invocations"]), 1)
        self.assertIn("-fuse-ld=/toolchain/bin/ld64.lld", result[0]["compiler_invocations"][0]["flags"])


if __name__ == "__main__":
    unittest.main()
