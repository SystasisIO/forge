#!/usr/bin/env python3
"""Small metadata-only regression suite; no compiler, download or source build."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shlex
import subprocess
import tarfile
import tempfile
import unittest
from unittest.mock import Mock, patch


ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location("toolchain", ROOT / ".github/scripts/check-vm-wasm-toolchain.py")
TOOLCHAIN = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(TOOLCHAIN)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.prefix = self.root / "original"
        (self.prefix / "bin").mkdir(parents=True)
        self.tool = self.prefix / "bin/tool"
        self.tool.write_text("metadata-only fixture\n")
        self.tool.chmod(0o755)
        (self.prefix / "bin/tool-alias").symlink_to("tool")
        self.recipe = self.root / "recipe.json"
        self.key = "a" * 64
        self.recipe.write_text(json.dumps({"key": self.key, "source_sha256": TOOLCHAIN.SOURCE_SHA256}))
        self.archive = self.root / "fixture.tar.gz"
        self.destination = self.root / "relocated"

    def pack(self):
        with contextlib.redirect_stdout(io.StringIO()):
            TOOLCHAIN.pack(self.prefix, self.recipe, self.archive)
        return TOOLCHAIN.sha256(self.archive)

    def unpack(self, digest=None, key=None):
        with contextlib.redirect_stdout(io.StringIO()):
            TOOLCHAIN.unpack(self.archive, self.destination, digest or TOOLCHAIN.sha256(self.archive), key or self.key)

    def malicious(self, entries):
        with tarfile.open(self.archive, "w:gz") as output:
            for name, target in entries:
                member = tarfile.TarInfo(name)
                if target is not None:
                    member.type = tarfile.SYMTYPE
                    member.linkname = target
                output.addfile(member)

    def test_relocation_hashes_modes_and_relative_alias(self):
        self.unpack(self.pack())
        self.assertEqual(TOOLCHAIN.tree(self.prefix), TOOLCHAIN.tree(self.destination))
        self.assertTrue(os.access(self.destination / "bin/tool-alias", os.X_OK))

    def test_wrong_digest_refused_before_extract(self):
        self.pack()
        with self.assertRaisesRegex(ValueError, "SHA mismatch"):
            self.unpack(digest="0" * 64)
        self.assertFalse(self.destination.exists())

    def test_wrong_recipe_refused(self):
        self.pack()
        with self.assertRaisesRegex(ValueError, "recipe identity mismatch"):
            self.unpack(key="b" * 64)

    def test_existing_destination_refused(self):
        self.pack()
        self.destination.mkdir()
        with self.assertRaisesRegex(ValueError, "already exists"):
            self.unpack()

    def test_modified_manifest_file_refused(self):
        self.pack()
        self.tool.write_text("changed after manifest\n")
        with tarfile.open(self.archive, "w:gz") as output:
            output.add(self.prefix, arcname="toolchain")
        with self.assertRaisesRegex(ValueError, "file/mode/symlink mismatch"):
            self.unpack()

    def test_unsafe_archive_entries_refused_before_extract(self):
        for entries in ([('../escape', None)], [('/absolute', None)], [('toolchain/alias', '../../escape')],
                        [('toolchain/alias', '/absolute')], [('toolchain/alias', 'bin'), ('toolchain/alias/file', None)],
                        [('toolchain/same', None), ('toolchain/same', None)]):
            with self.subTest(entries=entries):
                self.malicious(entries)
                with self.assertRaises(ValueError):
                    self.unpack()
                self.assertFalse(self.destination.exists())

    def test_external_package_symlink_refused(self):
        (self.prefix / "bin/external").symlink_to("/usr/bin/clang")
        with self.assertRaisesRegex(ValueError, "nonrelocatable"):
            self.pack()

    def test_wrong_source_refused(self):
        self.recipe.write_text(json.dumps({"source_sha256": "0" * 64}))
        with self.assertRaisesRegex(ValueError, "wrong source"):
            self.pack()


class HeaderTests(unittest.TestCase):
    def check(self, macros, includes):
        with tempfile.TemporaryDirectory() as temporary:
            calls = []

            def fake_run(argv, input_text=None):
                calls.append((argv, input_text))
                if "--version" in argv:
                    return "clang version 22.1.8\n", ""
                if "-dM" in argv:
                    return macros, ""
                return "", includes

            with patch.dict(os.environ, {"CC": "/fixture/clang", "CXX": "/fixture/clang++",
                                         "CXXFLAGS": "-stdlib=libc++"}), patch.object(TOOLCHAIN, "run", fake_run):
                with contextlib.redirect_stdout(io.StringIO()):
                    TOOLCHAIN.headers(Path(temporary))
            live = [entry for entry in calls if "-E" in entry[0]]
            self.assertEqual(len(live), 2)
            self.assertTrue(all(entry[1] == TOOLCHAIN.SOURCE and "-" in entry[0] for entry in live))

    def test_live_source_pass(self):
        self.check("#define _LIBCPP_VERSION 220108\n", ". /fixture/include/c++/v1/vector\n. /fixture/include/c++/v1/functional\n")

    def test_wrong_header_patch_refused(self):
        with self.assertRaisesRegex(ValueError, "220108"):
            self.check("#define _LIBCPP_VERSION 220109\n", "")

    def test_gnu_headers_refused(self):
        with self.assertRaisesRegex(ValueError, "GNU C\\+\\+"):
            self.check("#define _LIBCPP_VERSION 220108\n#define __GLIBCXX__ 1\n", "")

    def test_empty_header_evidence_refused(self):
        with self.assertRaisesRegex(ValueError, "no live header"):
            self.check("#define _LIBCPP_VERSION 220108\n", "")

    def test_wrong_compiler_or_scanner_patch_refused(self):
        for wrong in ("clang", "clang++", "clang-scan-deps"):
            def fake_run(argv, input_text=None):
                version = "22.1.9" if Path(argv[0]).name == wrong else "22.1.8"
                return f"clang version {version}\n", ""

            with self.subTest(tool=wrong), tempfile.TemporaryDirectory() as temporary, \
                    patch.dict(os.environ, {"CC": "/fixture/clang", "CXX": "/fixture/clang++"}), \
                    patch.object(TOOLCHAIN, "run", fake_run):
                with self.assertRaisesRegex(ValueError, "22.1.8"):
                    TOOLCHAIN.headers(Path(temporary))


class Stage0Tests(unittest.TestCase):
    def metadata(self, mutation=None):
        def fake_run(argv, input_text=None):
            package = argv[-1].split("=", 1)[0]
            record = {"Package": package, "Version": TOOLCHAIN.STAGE0_VERSION, "Architecture": "amd64",
                      "SHA256": TOOLCHAIN.STAGE0_PACKAGES[package], "Filename": f"pool/{package}.deb"}
            if mutation:
                mutation(record)
            return "\n".join(f"{key}: {value}" for key, value in record.items()), ""

        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "packages.json"
            with patch.object(TOOLCHAIN, "run", fake_run), contextlib.redirect_stdout(io.StringIO()):
                TOOLCHAIN.stage0_packages(evidence)
            self.assertEqual(len(json.loads(evidence.read_text())["packages"]), 7)

    def test_pinned_metadata_closure(self):
        self.metadata()

    def test_changed_package_hash_refused(self):
        with self.assertRaisesRegex(ValueError, "identity/hash mismatch"):
            self.metadata(lambda record: record.update(SHA256="0" * 64))

    def test_changed_package_build_refused(self):
        with self.assertRaisesRegex(ValueError, "identity/hash mismatch"):
            self.metadata(lambda record: record.update(Version=TOOLCHAIN.STAGE0_VERSION.replace("ca7933e47d3a", "000000000000")))

    def test_unsupported_clang18_and20_0_refused(self):
        for version in ((18, 1, 8), (20, 0, 9)):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "requires Clang >=20.1"):
                TOOLCHAIN.require_supported_stage0(version)

    def test_supported_but_unpinned_compiler_refused(self):
        for version in ((20, 1, 0), (22, 1, 9), (23, 0, 0)):
            with self.subTest(version=version), self.assertRaisesRegex(ValueError, "pinned noble"):
                TOOLCHAIN.require_supported_stage0(version)

    def guard(self, installed=None, macro_major=22):
        def fake_run(argv, input_text=None):
            if argv[0] == "dpkg-query":
                return installed or TOOLCHAIN.STAGE0_VERSION, ""
            if "-dM" in argv:
                self.assertEqual(input_text, "\n")
                return f"#define __clang_major__ {macro_major}\n#define __clang_minor__ 1\n#define __clang_patchlevel__ 8\n", ""
            if argv[0] == TOOLCHAIN.STAGE0_LD:
                return "Debian LLD 22.1.8\n", ""
            return "Debian clang version 22.1.8\n", ""

        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(TOOLCHAIN, "run", fake_run), patch.object(TOOLCHAIN, "sha256", return_value="a" * 64):
                with contextlib.redirect_stdout(io.StringIO()):
                    TOOLCHAIN.stage0(Path(temporary))
            self.assertEqual(json.loads((Path(temporary) / "stage0.json").read_text())["state"], "READY")

    def test_pinned_guard_pass(self):
        self.guard()

    def test_wrong_installed_package_refused(self):
        with self.assertRaisesRegex(ValueError, "wrong installed"):
            self.guard(installed="1:22.1.9")

    def test_frontend_macros_override_spoofed_banner(self):
        with self.assertRaisesRegex(ValueError, "requires Clang >=20.1"):
            self.guard(macro_major=18)


class IntelRuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / "fixture.bottle.tar.gz"
        self.formula = self.root / "llvm.rb"
        self.formula.write_text("metadata fixture, not a real formula\n")
        self.evidence = self.root / "bottle.json"
        self.prefix = Path("/usr/local/Cellar/llvm/22.1.8")
        for mock in (patch.dict(os.environ, {}, clear=True),
                     patch.object(TOOLCHAIN.platform, "system", return_value="Darwin"),
                     patch.object(TOOLCHAIN.platform, "machine", return_value="x86_64"),
                     patch.object(TOOLCHAIN.platform, "mac_ver", return_value=("15.7", (), ""))):
            mock.start()
            self.addCleanup(mock.stop)

    def test_explicit_runtime_scope(self):
        TOOLCHAIN.intel_scope("intel-runtime-off")

    def test_other_roles_refused(self):
        for scope in ("native", "jit", "bootstrap-linux", "intel-aot"):
            with self.subTest(scope=scope), self.assertRaisesRegex(ValueError, "only intel-runtime-off"):
                TOOLCHAIN.intel_scope(scope)

    def test_wrong_platform_or_architecture_refused(self):
        for system, machine, version in (("Linux", "x86_64", "15.7"), ("Darwin", "arm64", "15.7"),
                                         ("Darwin", "x86_64", "14.8"), ("Darwin", "x86_64", "16.0")):
            with self.subTest(platform=(system, machine, version)), \
                    patch.object(TOOLCHAIN.platform, "system", return_value=system), \
                    patch.object(TOOLCHAIN.platform, "machine", return_value=machine), \
                    patch.object(TOOLCHAIN.platform, "mac_ver", return_value=(version, (), "")):
                with self.assertRaisesRegex(ValueError, "native x86_64 macOS15"):
                    TOOLCHAIN.intel_scope("intel-runtime-off")

    def test_llvm_roles_and_discovery_refused(self):
        for variable in ("WAMR_JIT", "WAMR_AOT", "FORGE_ENABLE_VM_WASM_LLVM_JIT", "FORGE_ENABLE_VM_WASM_OPTIMIZING",
                         "FORGE_ENABLE_WASM_COMPILER", "FORGE_ENABLE_TOOLING", "LLVM_PREFIX", "LLVM_DIR", "Clang_DIR"):
            with self.subTest(variable=variable), patch.dict(os.environ, {variable: "ON"}):
                with self.assertRaisesRegex(ValueError, variable):
                    TOOLCHAIN.intel_scope("intel-runtime-off")
        for variable in ("CMAKE_DISABLE_FIND_PACKAGE_LLVM", "CMAKE_DISABLE_FIND_PACKAGE_Clang"):
            with self.subTest(variable=variable), patch.dict(os.environ, {variable: "OFF"}):
                with self.assertRaisesRegex(ValueError, variable):
                    TOOLCHAIN.intel_scope("intel-runtime-off")

    def bottle_fixture(self, receipt=None, embedded=None, missing=False, duplicate=False):
        receipt = receipt or {"arch": "x86_64", "built_on": {"os_version": "macOS 14.8"}}
        embedded = embedded or f"llvmorg-{TOOLCHAIN.VERSION}/llvm-project-{TOOLCHAIN.VERSION}.src.tar.xz\n{TOOLCHAIN.SOURCE_SHA256}"
        entries = [("INSTALL_RECEIPT.json", json.dumps(receipt))]
        if duplicate:
            entries += entries
        if not missing:
            entries.append((".brew/llvm.rb", embedded))
        with tarfile.open(self.archive, "w:gz") as output:
            for name, contents in entries:
                data = contents.encode()
                member = tarfile.TarInfo("llvm/22.1.8/" + name)
                member.size = len(data)
                output.addfile(member, io.BytesIO(data))

    def inspect_fixture(self):
        # Synthetic tar metadata exercises validation without downloading a bottle.
        digests = {self.archive: TOOLCHAIN.INTEL_BOTTLE_SHA256, self.formula: TOOLCHAIN.INTEL_FORMULA_SHA256}
        with patch.object(TOOLCHAIN, "sha256", side_effect=digests.__getitem__), \
                contextlib.redirect_stdout(io.StringIO()):
            TOOLCHAIN.intel_archive(self.archive, self.formula, self.evidence)

    def test_pinned_metadata_scope_not_llvm_qualification(self):
        self.bottle_fixture()
        self.inspect_fixture()
        result = json.loads(self.evidence.read_text())
        self.assertEqual(result["scope"], "intel-runtime-off")
        self.assertEqual(result["formula_ref"], TOOLCHAIN.INTEL_FORMULA_REF)
        self.assertEqual(result["LLVM_library_qualification"], "NOT_APPLICABLE_RUNTIME_OFF")

    def test_wrong_bottle_sha_refused_before_open(self):
        self.archive.write_bytes(b"not a bottle")
        with self.assertRaisesRegex(ValueError, "bottle SHA mismatch"):
            TOOLCHAIN.intel_archive(self.archive, self.formula, self.evidence)
        self.assertFalse(self.evidence.exists())

    def test_wrong_formula_sha_refused_before_open(self):
        with patch.object(TOOLCHAIN, "sha256", side_effect=[TOOLCHAIN.INTEL_BOTTLE_SHA256, "0" * 64]):
            with self.assertRaisesRegex(ValueError, "formula SHA/ref mismatch"):
                TOOLCHAIN.intel_archive(self.archive, self.formula, self.evidence)
        self.assertFalse(self.evidence.exists())

    def test_bottle_receipt_arch_and_platform_refused(self):
        for receipt in ({"arch": "arm64", "built_on": {"os_version": "macOS 14.8"}},
                        {"arch": "x86_64", "built_on": {"os_version": "macOS 15.0"}}):
            with self.subTest(receipt=receipt):
                self.bottle_fixture(receipt=receipt)
                with self.assertRaisesRegex(ValueError, "architecture/platform mismatch"):
                    self.inspect_fixture()

    def test_wrong_embedded_version_refused(self):
        self.bottle_fixture(embedded="llvmorg-23.0.0/llvm-project-23.0.0.src.tar.xz")
        with self.assertRaisesRegex(ValueError, "source version mismatch"):
            self.inspect_fixture()

    def test_missing_duplicate_and_oversized_metadata_refused(self):
        self.bottle_fixture(missing=True)
        with self.assertRaisesRegex(ValueError, "missing historical"):
            self.inspect_fixture()
        self.bottle_fixture(duplicate=True)
        with self.assertRaisesRegex(ValueError, "invalid historical"):
            self.inspect_fixture()
        self.bottle_fixture(embedded="x" * (256 * 1024 + 1))
        with self.assertRaisesRegex(ValueError, "invalid historical"):
            self.inspect_fixture()

    def test_local_brew_formula_cannot_fall_back_to_current(self):
        for name, version in (("llvm", "22.1.8"), ("llvm", "23.1.0"), ("llvm@22", "22.1.8")):
            with self.subTest(name=name, version=version):
                self.formula.write_text(json.dumps({"formulae": [{"name": name, "versions": {"stable": version}}]}))
                if (name, version) == ("llvm", "22.1.8"):
                    TOOLCHAIN.intel_formula(self.formula)
                else:
                    with self.assertRaisesRegex(ValueError, "historical local"):
                        TOOLCHAIN.intel_formula(self.formula)

    def inspect_headers(self, macros="#define __EXCEPTIONS 1\n#define __GXX_RTTI 1\n", outside=False):
        def fake_headers(directory):
            directory.mkdir()
            header_root = Path("/outside") if outside else self.prefix
            (directory / "header-result.json").write_text(json.dumps({"headers": {
                name: str(header_root / "include/c++/v1" / name) for name in ("vector", "functional")}}))
            (directory / "macros.txt").write_text(macros)

        with patch.dict(os.environ, {"CC": str(self.prefix / "bin/clang"), "CXX": str(self.prefix / "bin/clang++")}), \
                patch.object(Path, "is_file", return_value=True), patch.object(Path, "resolve", lambda file: file), \
                patch.object(TOOLCHAIN, "headers", side_effect=fake_headers), contextlib.redirect_stdout(io.StringIO()):
            TOOLCHAIN.intel_headers(self.prefix, self.root / "headers")

    def test_exact_cellar_headers_and_host_features(self):
        self.inspect_headers()

    def test_host_rtti_or_eh_disabled_refused(self):
        for macros in ("#define __EXCEPTIONS 1\n", "#define __GXX_RTTI 1\n"):
            with self.subTest(macros=macros), tempfile.TemporaryDirectory() as temporary:
                self.root = Path(temporary)
                with self.assertRaisesRegex(ValueError, "host feature disabled"):
                    self.inspect_headers(macros=macros)

    def test_headers_outside_keg_refused(self):
        with self.assertRaisesRegex(ValueError, "headers resolve outside"):
            self.inspect_headers(outside=True)

    def test_alias_or_other_keg_refused(self):
        with self.assertRaisesRegex(ValueError, "exact native Cellar"):
            TOOLCHAIN.intel_headers(Path("/usr/local/opt/llvm"), self.root)
        with patch.dict(os.environ, {"CC": "/usr/local/opt/llvm/bin/clang", "CXX": str(self.prefix / "bin/clang++")}):
            with self.assertRaisesRegex(ValueError, "alias/outside-keg"):
                TOOLCHAIN.intel_headers(self.prefix, self.root)


class ProvisioningTests(unittest.TestCase):
    def test_llvm_pic_checks_precede_native_build_without_masking_driver_warnings(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        tokens = shlex.split(source, comments=True)
        start = tokens.index("llvm-configure") + 1
        stop = tokens.index("log_run", start)
        args = tokens[start:stop]
        self.assertIn("-DLLVM_ENABLE_PIC=ON", args)
        self.assertIn("-DCMAKE_POSITION_INDEPENDENT_CODE=ON", args)
        flags = next(arg for arg in args if arg.startswith("-DCMAKE_CXX_FLAGS="))
        self.assertIn("-stdlib=libc++ -isystem $prefix/include/c++/v1", flags)
        self.assertNotIn("-nostdinc++", flags)
        for forbidden in ("-Wno-unused-command-line-argument", "-Wno-error", "CXX_SUPPORTS_FPIC="):
            self.assertNotIn(forbidden, " ".join(args))
        for name, entry in (("llvm-c-pic-check", "C_SUPPORTS_FPIC:INTERNAL=1"),
                            ("llvm-cxx-pic-check", "CXX_SUPPORTS_FPIC:INTERNAL=1")):
            check = tokens.index(name)
            self.assertGreater(check, start)
            self.assertLess(check, tokens.index("llvm-build"))
            self.assertEqual(tokens[check + 1:check + 4], ["grep", "-Fx", entry])
        workflow = (ROOT / ".github/workflows/vm-wasm.yml").read_text()
        self.assertIn("build/vm-wasm/toolchain intel-runtime-off", workflow)

    def test_intel_is_explicit_local_pour_with_bottled_leaf_dependencies(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        branch = source.split('if [[ "$mode" == intel-runtime-off ]]; then', 1)[1].split('elif [[ "$mode" == stage0-linux', 1)[0]
        self.assertLess(branch.index('intel-scope "$mode"'), branch.index("curl --fail"))
        self.assertLess(branch.index('intel-archive "$archive"'), branch.index("brew info"))
        self.assertIn('brew install --force-bottle --skip-link "$archive"', branch)
        self.assertIn('brew install --force-bottle --formula "$dependency"', branch)
        self.assertIn('test ! -s "$evidence/intel-dependency-$index-missing.txt"', branch)
        self.assertIn('test ! -s "$evidence/intel-remaining-deps.txt"', branch)
        self.assertIn("HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1", branch)
        self.assertIn("sysctl.proc_translated", branch)
        self.assertIn("bootstrap_abi.cpp", branch)
        self.assertIn("bootstrap_abi_main.cpp", branch)
        for forbidden in ("--build-from-source", "--ignore-dependencies", "LLVM_PREFIX=", "LLVM_DIR=",
                          "check-llvm-cxx-abi.py", "brew tap", "cmake --build"):
            self.assertNotIn(forbidden, branch)

    def test_runtime_graphs_exclude_helper_tests_and_retain_runtime_checks(self):
        tokens = shlex.split((ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text(), comments=True)
        for name in ("runtimes-configure", "final-runtimes-configure"):
            start = tokens.index(name) + 1
            args = tokens[start:tokens.index("log_run", start)]
            with self.subTest(graph=name):
                for flag in ("LLVM_INCLUDE_TESTS", "LIBCXX_INCLUDE_TESTS", "LIBCXXABI_INCLUDE_TESTS",
                             "LIBUNWIND_INCLUDE_TESTS", "LIBCXX_INCLUDE_BENCHMARKS", "LIBCXX_INCLUDE_DOCS"):
                    self.assertIn(f"-D{flag}=OFF", args)
                self.assertIn("-DLIBCXX_ENABLE_EXCEPTIONS=ON", args)
                self.assertIn("-DLIBCXXABI_ENABLE_EXCEPTIONS=ON", args)
                if name == "final-runtimes-configure":
                    self.assertIn("-DCOMPILER_RT_INCLUDE_TESTS=OFF", args)
                    self.assertIn("-DCOMPILER_RT_BUILD_SANITIZERS=ON", args)

    def test_insufficient_bootstrap_disk_refuses_and_records_actual_free(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "disk.json"
            free = TOOLCHAIN.MIN_BOOTSTRAP_FREE_BYTES - 1
            with patch.object(TOOLCHAIN.shutil, "disk_usage", return_value=Mock(free=free)):
                with self.assertRaisesRegex(ValueError, "40 GiB"):
                    TOOLCHAIN.disk_preflight(Path(temporary), evidence)
            record = json.loads(evidence.read_text())
            self.assertEqual(record["state"], "REFUSED")
            self.assertEqual(record["free_bytes"], free)

    def test_bootstrap_disk_boundary_accepted(self):
        with tempfile.TemporaryDirectory() as temporary:
            evidence = Path(temporary) / "disk.json"
            with patch.object(TOOLCHAIN.shutil, "disk_usage", return_value=Mock(free=TOOLCHAIN.MIN_BOOTSTRAP_FREE_BYTES)):
                with contextlib.redirect_stdout(io.StringIO()):
                    TOOLCHAIN.disk_preflight(Path(temporary), evidence)
            self.assertEqual(json.loads(evidence.read_text())["state"], "READY")

    def test_linux_native_cannot_fall_back_to_apt_or_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            environment = dict(os.environ, GITHUB_WORKSPACE=str(ROOT), RUNNER_OS="Linux")
            result = subprocess.run(["bash", str(ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh"), temporary],
                                    env=environment, capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 2)
            self.assertIn("dedicated source-bootstrap artifact", result.stderr)

    def test_missing_intel_bottle_is_not_a_skip(self):
        with tempfile.TemporaryDirectory() as temporary:
            file = Path(temporary) / "bottle.json"
            file.write_text(json.dumps({"formulae": [{"versions": {"stable": "22.1.8"}, "bottle": {
                "stable": {"files": {"arm64_sequoia": {"url": "https://fixture/arm", "sha256": "a" * 64}}}}}]}))
            with self.assertRaisesRegex(ValueError, "source build forbidden"):
                TOOLCHAIN.bottle(file, "sequoia")

    def test_other_patch_bottle_refused(self):
        with tempfile.TemporaryDirectory() as temporary:
            file = Path(temporary) / "bottle.json"
            file.write_text(json.dumps({"formulae": [{"versions": {"stable": "22.1.9"}}]}))
            with self.assertRaisesRegex(ValueError, "exactly 22.1.8"):
                TOOLCHAIN.bottle(file, "arm64_sequoia")


if __name__ == "__main__":
    unittest.main()
