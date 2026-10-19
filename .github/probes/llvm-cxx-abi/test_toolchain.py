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

    def test_sha_receipt_delegates_metadata_and_leaves_native_pending(self):
        # Only pin admission is ours; do not invent a Homebrew archive/receipt schema.
        digests = {self.archive: TOOLCHAIN.INTEL_BOTTLE_SHA256, self.formula: TOOLCHAIN.INTEL_FORMULA_SHA256}
        with patch.object(TOOLCHAIN, "sha256", side_effect=digests.__getitem__), \
                patch.object(TOOLCHAIN.tarfile, "open", side_effect=AssertionError("Homebrew owns bottle parsing")), \
                contextlib.redirect_stdout(io.StringIO()):
            TOOLCHAIN.intel_archive(self.archive, self.formula, self.evidence)
        result = json.loads(self.evidence.read_text())
        self.assertEqual(result, {"state": "SHA_VERIFIED", "scope": "intel-runtime-off",
                                  "bottle_sha256": TOOLCHAIN.INTEL_BOTTLE_SHA256,
                                  "formula_ref": TOOLCHAIN.INTEL_FORMULA_REF,
                                  "formula_sha256": TOOLCHAIN.INTEL_FORMULA_SHA256,
                                  "metadata_owner": "Homebrew", "native_qualification": "PENDING",
                                  "LLVM_library_qualification": "NOT_APPLICABLE_RUNTIME_OFF"})

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

    def brew_formula_metadata(self):
        return {"name": "llvm", "full_name": "forge/vm-ci/llvm", "versions": {"stable": "22.1.8"},
                "dependencies": ["python@3.14", "xz", "zstd", "forge/vm-ci/z3"], "bottle": {
            "stable": {"files": {"sonoma": {"sha256": TOOLCHAIN.INTEL_BOTTLE_SHA256,
                "url": f"https://ghcr.io/v2/homebrew/core/llvm/blobs/sha256:{TOOLCHAIN.INTEL_BOTTLE_SHA256}"}}}}}

    def test_pinned_tap_formula_and_core_sonoma_identity(self):
        self.formula.write_text(json.dumps({"formulae": [self.brew_formula_metadata()]}))
        TOOLCHAIN.intel_formula(self.formula)

    def test_tap_formula_cannot_fall_back_to_core_current_or_other_tap(self):
        for name, full_name, version in (("llvm", "llvm", "22.1.8"), ("llvm", "other/tap/llvm", "22.1.8"),
                                         ("llvm", "forge/vm-ci/llvm", "23.1.0"), ("llvm@22", "llvm@22", "22.1.8")):
            with self.subTest(name=name, full_name=full_name, version=version):
                formula = self.brew_formula_metadata()
                formula.update(name=name, full_name=full_name, versions={"stable": version})
                self.formula.write_text(json.dumps({"formulae": [formula]}))
                with self.assertRaisesRegex(ValueError, "pinned forge/vm-ci/llvm"):
                    TOOLCHAIN.intel_formula(self.formula)

    def test_tap_formula_wrong_sha_origin_or_missing_sonoma_refused(self):
        for mutation in ({"sha256": "0" * 64}, {"url": "https://fixture/other-bottle"}, None):
            with self.subTest(mutation=mutation):
                formula = self.brew_formula_metadata()
                files = formula["bottle"]["stable"]["files"]
                if mutation is None:
                    del files["sonoma"]
                else:
                    files["sonoma"].update(mutation)
                self.formula.write_text(json.dumps({"formulae": [formula]}))
                with self.assertRaisesRegex(ValueError, "pinned Homebrew core Sonoma bottle"):
                    TOOLCHAIN.intel_formula(self.formula)

    def test_ambiguous_tap_formula_metadata_refused(self):
        self.formula.write_text(json.dumps({"formulae": [self.brew_formula_metadata()] * 2}))
        with self.assertRaisesRegex(ValueError, "ambiguous historical Intel tap"):
            TOOLCHAIN.intel_formula(self.formula)

    def test_llvm_missing_or_core_z3_dependency_refused(self):
        for dependencies in ([], ["z3"], ["homebrew/core/z3"], ["forge/vm-ci/z3", "z3"]):
            with self.subTest(dependencies=dependencies):
                formula = self.brew_formula_metadata()
                formula["dependencies"] = dependencies
                self.formula.write_text(json.dumps({"formulae": [formula]}))
                with self.assertRaisesRegex(ValueError, "qualified forge/vm-ci/z3"):
                    TOOLCHAIN.intel_formula(self.formula)

    def z3_metadata(self):
        return {"name": "z3", "full_name": "forge/vm-ci/z3", "versions": {"stable": "4.16.0"},
                "dependencies": [], "bottle": {"stable": {"files": {"sonoma": {
                    "sha256": TOOLCHAIN.INTEL_Z3_BOTTLE_SHA256,
                    "url": f"https://ghcr.io/v2/homebrew/core/z3/blobs/sha256:{TOOLCHAIN.INTEL_Z3_BOTTLE_SHA256}"}}}}}

    def test_pinned_z3_has_no_runtime_dependency_tree(self):
        self.formula.write_text(json.dumps({"formulae": [self.z3_metadata()]}))
        TOOLCHAIN.intel_z3_formula(self.formula)

    def test_z3_wrong_identity_bottle_or_runtime_dependency_refused(self):
        for field, value in (("full_name", "z3"), ("versions", {"stable": "5.1.0"}), ("dependencies", ["gmp"]),
                             ("sha256", "0" * 64), ("url", "https://fixture/other-bottle")):
            with self.subTest(field=field):
                formula = self.z3_metadata()
                if field in ("sha256", "url"):
                    formula["bottle"]["stable"]["files"]["sonoma"][field] = value
                else:
                    formula[field] = value
                self.formula.write_text(json.dumps({"formulae": [formula]}))
                with self.assertRaises(ValueError):
                    TOOLCHAIN.intel_z3_formula(self.formula)

    def test_z3_inputs_reject_wrong_formula_or_patch_sha(self):
        for digests in (("0" * 64,), (TOOLCHAIN.INTEL_Z3_FORMULA_SHA256, "0" * 64)):
            with self.subTest(digests=digests), patch.object(TOOLCHAIN, "sha256", side_effect=digests):
                with self.assertRaisesRegex(ValueError, "SHA mismatch"):
                    TOOLCHAIN.intel_z3_inputs(self.formula, self.root / "dependency.patch")

    def test_derived_formula_records_pins_and_rejects_any_other_bytes(self):
        digests = [TOOLCHAIN.INTEL_FORMULA_SHA256, TOOLCHAIN.INTEL_Z3_PATCH_SHA256, TOOLCHAIN.INTEL_DERIVED_FORMULA_SHA256]
        for wrong in (None, 0, 1, 2):
            values = list(digests)
            if wrong is not None:
                values[wrong] = "0" * 64
            with self.subTest(wrong=wrong), patch.object(TOOLCHAIN, "sha256", side_effect=values):
                args = (self.formula, self.root / "derived.rb", self.root / "dependency.patch", self.evidence)
                if wrong is None:
                    TOOLCHAIN.intel_derived_formula(*args)
                    result = json.loads(self.evidence.read_text())
                    self.assertEqual(result["original_llvm_formula_sha256"], digests[0])
                    self.assertEqual(result["dependency_patch_sha256"], digests[1])
                    self.assertEqual(result["derived_llvm_formula_sha256"], digests[2])
                    self.assertEqual(result["upstream_z3_formula_sha256"], TOOLCHAIN.INTEL_Z3_FORMULA_SHA256)
                    self.assertEqual(result["native_qualification"], "PENDING")
                    self.evidence.unlink()
                else:
                    with self.assertRaises(ValueError):
                        TOOLCHAIN.intel_derived_formula(*args)
                    self.assertFalse(self.evidence.exists())

    def test_reviewed_dependency_patch_has_one_delta_and_requires_exact_context(self):
        dependency_patch = ROOT / ".github/probes/llvm-cxx-abi/intel-llvm-z3.patch"
        self.assertEqual(TOOLCHAIN.sha256(dependency_patch), TOOLCHAIN.INTEL_Z3_PATCH_SHA256)
        lines = dependency_patch.read_text().splitlines()
        self.assertEqual([line for line in lines if line.startswith("-") and not line.startswith("---")],
                         ['-    depends_on "z3"'])
        self.assertEqual([line for line in lines if line.startswith("+") and not line.startswith("+++")],
                         ['+    depends_on "forge/vm-ci/z3"'])
        context = "\n".join(line[1:] for line in lines[3:] if not line.startswith("+")) + "\n"
        for valid in (True, False):
            original = "\n" * 48 + (context if valid else context.replace("on_system", "changed_context"))
            self.formula.write_text(original)
            result = subprocess.run(["patch", "-t", "-N", "-F", "0", str(self.formula), str(dependency_patch)],
                                    capture_output=True, text=True, timeout=10)
            with self.subTest(valid=valid):
                if valid:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    self.assertEqual(self.formula.read_text(), original.replace('depends_on "z3"', 'depends_on "forge/vm-ci/z3"'))
                else:
                    self.assertNotEqual(result.returncode, 0)
                    self.assertEqual(self.formula.read_text(), original)

    def test_z3_library_requires_owned_keg_and_packaged_dylib_target(self):
        keg = Path("/usr/local/Cellar/z3/4.16.0")
        payload = keg / "lib/libz3.4.16.0.0.dylib"
        for wrong_keg, missing_library in ((False, False), (True, False), (False, True)):
            def resolved(file, strict=False):
                if file == Path("/usr/local/opt/z3"):
                    return Path("/outside") if wrong_keg else keg
                return payload

            with self.subTest(wrong_keg=wrong_keg, missing_library=missing_library), \
                    patch.object(Path, "is_symlink", return_value=True), patch.object(Path, "resolve", resolved), \
                    patch.object(Path, "is_file", return_value=not missing_library), \
                    patch.object(TOOLCHAIN, "sha256", return_value="a" * 64):
                if wrong_keg or missing_library:
                    with self.assertRaises(ValueError):
                        TOOLCHAIN.intel_z3_library(self.evidence)
                else:
                    TOOLCHAIN.intel_z3_library(self.evidence)
                    self.assertEqual(json.loads(self.evidence.read_text())["payload"], str(payload))

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
    def final_runtime_command(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        command = source.split("   log_run final-runtimes-configure ", 1)[1].split("\n   log_run ", 1)[0]
        self.assertTrue(command.startswith("/usr/bin/cmake "))
        return source, command.replace("/usr/bin/cmake", "capture_cmake", 1)

    def test_final_runtimes_use_driver_headers_and_bind_stage_a_link_search(self):
        _, command = self.final_runtime_command()
        environment = dict(os.environ, prefix="/fixture/install", source="/fixture/source", work="/fixture/work")
        script = 'set -euo pipefail\ncapture_cmake() { printf "%s\\n" "$@"; }\n' + command
        result = subprocess.run(["bash", "-c", script], env=environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        args = result.stdout.splitlines()
        for flag in ("-DCMAKE_C_COMPILER=/fixture/install/bin/clang",
                     "-DCMAKE_CXX_COMPILER=/fixture/install/bin/clang++",
                     "-DCMAKE_CXX_FLAGS=-stdlib=libc++",
                     "-DCMAKE_BUILD_RPATH=/fixture/install/lib",
                     "-DCMAKE_INSTALL_RPATH=$ORIGIN", "-DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF"):
            self.assertIn(flag, args)
        for kind in ("EXE", "SHARED", "MODULE"):
            with self.subTest(linker=kind):
                self.assertIn(f"-DCMAKE_{kind}_LINKER_FLAGS=-L/fixture/install/lib -fuse-ld=/usr/bin/ld.lld-22", args)
        for forbidden in ("-DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY", "-DCMAKE_CXX_COMPILER_WORKS=ON",
                          "-DCMAKE_CXX_COMPILER_WORKS=TRUE", "-stdlib=libstdc++", "-isystem", "-nostdinc++",
                          "-DCOMPILER_RT_BUILD_LIBFUZZER=OFF", "-DCOMPILER_RT_BUILD_SANITIZERS=OFF",
                          "-DLIBFUZZER_LINK_LIBCXX=OFF"):
            self.assertNotIn(forbidden, " ".join(args))

    def final_runtime_header_checks(self):
        source, _ = self.final_runtime_command()
        checks = "   log_run final-runtimes-headers " + source.split("   log_run final-runtimes-headers ", 1)[1]
        return source, checks.split("   log_run final-runtimes-configure ", 1)[0]

    def test_final_runtime_header_check_uses_installed_tools_without_include_override(self):
        source, checks = self.final_runtime_header_checks()
        command = checks.split("   log_run final-runtimes-header-prefix ", 1)[0]
        script = 'set -euo pipefail\nlog_run() { printf "%s\\n" "$@"; }\n' + command
        environment = dict(os.environ, prefix="/fixture/install", checker="/fixture/checker.py", evidence="/fixture/evidence")
        result = subprocess.run(["bash", "-c", script], env=environment, capture_output=True, text=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.splitlines(), [
            "final-runtimes-headers", "env", "CC=/fixture/install/bin/clang", "CXX=/fixture/install/bin/clang++",
            "CXXFLAGS=-stdlib=libc++", "python3", "/fixture/checker.py", "headers", "/fixture/evidence/final-runtimes-headers"])
        self.assertLess(source.index("log_run llvm-install "), source.index("log_run final-runtimes-headers "))
        self.assertLess(source.index("log_run final-runtimes-header-prefix "), source.index("log_run final-runtimes-configure "))

    def test_final_runtime_header_prefix_guard_resolves_files_and_refuses_escape(self):
        _, checks = self.final_runtime_header_checks()
        command = checks.split("   log_run final-runtimes-header-prefix ", 1)[1]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            prefix = root / "install"
            prefix.mkdir()
            outside = root / "install-other"
            outside.mkdir()
            for directory in (prefix, outside):
                for name in ("vector", "functional"):
                    (directory / name).touch()
            (prefix / "escaped").symlink_to(outside / "vector")
            evidence = root / "evidence/final-runtimes-headers"
            evidence.mkdir(parents=True)
            script = 'set -euo pipefail\n' + command + '\necho PREFIX_ACCEPTED\n'
            environment = dict(os.environ, prefix=str(prefix), evidence=str(evidence.parent))
            for kind in ("owned", "outside", "symlink", "missing", "incomplete"):
                headers = {name: str(prefix / name) for name in ("vector", "functional")}
                if kind == "outside":
                    headers["vector"] = str(outside / "vector")
                elif kind == "symlink":
                    headers["vector"] = str(prefix / "escaped")
                elif kind == "missing":
                    headers["vector"] = str(prefix / "absent")
                elif kind == "incomplete":
                    del headers["functional"]
                (evidence / "header-result.json").write_text(json.dumps({"headers": headers}))
                result = subprocess.run(["bash", "-c", script], env=environment, capture_output=True, text=True, timeout=10)
                with self.subTest(kind=kind):
                    if kind == "owned":
                        self.assertEqual(result.returncode, 0, result.stderr)
                        self.assertIn("PREFIX_ACCEPTED", result.stdout)
                    else:
                        self.assertNotEqual(result.returncode, 0)
                        self.assertNotIn("PREFIX_ACCEPTED", result.stdout)

    def test_final_runtime_header_failure_prevents_configure(self):
        source, checks = self.final_runtime_header_checks()
        prologue = source.split('if [[ "$mode" == intel-runtime-off ]]; then', 1)[0]
        script = (prologue + '\nenv() { echo "fixture header failure" >&2; return 79; }\n'
                  + checks + '\necho UNEXPECTED_CONFIGURE\n')
        with tempfile.TemporaryDirectory() as temporary:
            environment = dict(os.environ, GITHUB_WORKSPACE=str(ROOT), prefix="/fixture/install")
            result = subprocess.run(["bash", "-c", script, "runtime-header-test", temporary], env=environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 79)
            self.assertIn("HARNESS_ERROR", result.stderr)
            self.assertNotIn("UNEXPECTED_CONFIGURE", result.stdout)
            self.assertIn("fixture header failure", (Path(temporary) / "final-runtimes-headers.log").read_text())
            self.assertFalse((Path(temporary) / "final-runtimes-header-prefix.command").exists())

    def test_final_runtime_compiler_check_failure_remains_fatal(self):
        source, command = self.final_runtime_command()
        prologue = source.split('if [[ "$mode" == intel-runtime-off ]]; then', 1)[0]
        script = (prologue + '\ncapture_cmake() { echo "fixture linker failure" >&2; return 78; }\n'
                  + "log_run final-runtimes-configure " + command + '\necho UNEXPECTED_SUCCESS\n')
        with tempfile.TemporaryDirectory() as temporary:
            environment = dict(os.environ, GITHUB_WORKSPACE=str(ROOT), prefix="/fixture/install",
                               source="/fixture/source", work="/fixture/work")
            result = subprocess.run(["bash", "-c", script, "runtime-test", temporary], env=environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 78)
            self.assertIn("HARNESS_ERROR", result.stderr)
            self.assertNotIn("UNEXPECTED_SUCCESS", result.stdout)
            self.assertIn("fixture linker failure", (Path(temporary) / "final-runtimes-configure.log").read_text())

    def test_linux_graphs_execute_the_tools_hashed_by_the_recipe(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        branch = source.split('elif [[ "$mode" == bootstrap-linux ]]; then', 1)[1].split('\nelif ', 1)[0]
        tokens = shlex.split(branch, comments=True)
        for graph in ("runtimes", "llvm", "final-runtimes"):
            for operation in ("configure", "build", "install"):
                name = f"{graph}-{operation}"
                with self.subTest(command=name):
                    start = tokens.index(name) + 1
                    self.assertEqual(tokens[start], "/usr/bin/cmake")
                    if operation == "configure":
                        args = tokens[start:tokens.index("log_run", start)]
                        self.assertIn("-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja", args)
        self.assertNotIn("cmake", tokens)

    def test_linux_qualification_uses_the_same_recipe_tools(self):
        source = (ROOT / ".github/scripts/check-llvm-cxx-abi.py").read_text()
        self.assertIn('command(["/usr/bin/cmake", "-S"', source)
        self.assertIn('"-DCMAKE_MAKE_PROGRAM=/usr/bin/ninja"', source)
        self.assertIn('command(["/usr/bin/cmake", "--build"', source)
        self.assertNotIn('command(["cmake"', source)

    def test_intel_trust_is_only_two_pinned_formula_names_after_checks_before_load(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        branch = source.split('if [[ "$mode" == intel-runtime-off ]]; then', 1)[1].split('elif [[ "$mode" == stage0-linux', 1)[0]
        trusts = [line.strip() for line in branch.splitlines() if "brew trust" in line and shlex.split(line, comments=True)]
        self.assertEqual([shlex.split(line) for line in trusts], [
            ["log_run", "intel-z3-trust", "brew", "trust", "--formula", "forge/vm-ci/z3"],
            ["log_run", "intel-llvm-trust", "brew", "trust", "--formula", "forge/vm-ci/llvm"]])
        first_trust = branch.index(trusts[0])
        for check in ('intel-archive "$archive"', 'intel-z3-inputs "$z3_formula"',
                      'cmp -- "$z3_formula" "$tap_z3_formula"', 'intel-derived-formula \\\n',
                      'shasum -a 256 "$tap_formula"'):
            self.assertLess(branch.index(check), first_trust)
        self.assertLess(branch.index(trusts[-1]), branch.index("brew info"))
        for line in trusts:
            self.assertNotIn("||", line)
        for forbidden in ("HOMEBREW_DEVELOPER", "HOMEBREW_INTERNAL_ALLOW", "--tap", "--all"):
            self.assertNotIn(forbidden, branch)

    def test_unsupported_formula_trust_is_fatal_without_running_host_brew(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        prologue = source.split('if [[ "$mode" == intel-runtime-off ]]; then', 1)[0]
        trusts = "\n".join(line for line in source.splitlines() if "brew trust" in line and shlex.split(line, comments=True))
        # Exercise the actual logging/error wrapper with a shell-local Brew stub.
        script = prologue + '\nbrew() { echo "unsupported brew trust" >&2; return 64; }\n' + trusts + '\necho UNEXPECTED_SUCCESS\n'
        with tempfile.TemporaryDirectory() as temporary:
            environment = dict(os.environ, GITHUB_WORKSPACE=str(ROOT))
            result = subprocess.run(["bash", "-c", script, "trust-test", temporary], env=environment,
                                    capture_output=True, text=True, timeout=10)
            self.assertEqual(result.returncode, 64)
            self.assertIn("HARNESS_ERROR", result.stderr)
            self.assertIn("unsupported brew trust", result.stdout)
            self.assertNotIn("UNEXPECTED_SUCCESS", result.stdout)
            self.assertFalse((Path(temporary) / "intel-llvm-trust.log").exists())

    def test_llvm_pic_checks_precede_native_build_without_masking_driver_warnings(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        tokens = shlex.split(source, comments=True)
        start = tokens.index("llvm-configure") + 1
        stop = tokens.index("log_run", start)
        args = tokens[start:stop]
        self.assertIn("-DLLVM_ENABLE_PIC=ON", args)
        self.assertIn("-DCMAKE_POSITION_INDEPENDENT_CODE=ON", args)
        for kind in ("EXE", "SHARED", "MODULE"):
            self.assertIn(f"-DCMAKE_{kind}_LINKER_FLAGS=-L$prefix/lib -fuse-ld=/usr/bin/ld.lld-22", args)
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

    def test_intel_is_ci_only_pinned_tap_pour_with_bottled_leaf_dependencies(self):
        source = (ROOT / ".github/scripts/setup-vm-wasm-toolchain.sh").read_text()
        branch = source.split('if [[ "$mode" == intel-runtime-off ]]; then', 1)[1].split('elif [[ "$mode" == stage0-linux', 1)[0]
        self.assertLess(branch.index('intel-scope "$mode"'), branch.index("curl --fail"))
        self.assertLess(branch.index('test "${GITHUB_ACTIONS:-}" = true'), branch.index("command -v brew"))
        self.assertLess(branch.index('test "${CI:-}" = true'), branch.index("command -v brew"))
        self.assertLess(branch.index('intel-archive "$archive"'), branch.index("brew tap-new"))
        self.assertLess(branch.index('grep -Fxq "$tap"'), branch.index("brew tap-new"))
        self.assertIn('brew tap-new --no-git "$tap"', branch)
        self.assertIn('tap=forge/vm-ci', branch)
        self.assertIn('target_formula="$tap/llvm"', branch)
        self.assertIn('tap_formula="$(brew --repository "$tap")/Formula/llvm.rb"', branch)
        self.assertIn('cp "$formula" "$tap_formula"', branch)
        self.assertIn('cmp -- "$formula" "$tap_formula"', branch)
        self.assertIn('shasum -a 256 "$tap_formula"', branch)
        self.assertIn('brew info --json=v2 "$target_formula"', branch)
        self.assertIn('brew deps --missing --topological --formula "$target_formula"', branch)
        self.assertIn('brew install --force-bottle --skip-link "$target_formula"', branch)
        self.assertIn('brew install --force-bottle --formula "$dependency"', branch)
        self.assertIn('test ! -s "$evidence/intel-dependency-$index-missing.txt"', branch)
        self.assertIn('test ! -s "$evidence/intel-remaining-deps.txt"', branch)
        self.assertIn("HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1", branch)
        self.assertIn("sysctl.proc_translated", branch)
        self.assertIn("bootstrap_abi.cpp", branch)
        self.assertIn("bootstrap_abi_main.cpp", branch)
        self.assertIn('test ! -e /usr/local/Cellar/z3 && test ! -L /usr/local/Cellar/z3', branch)
        self.assertIn('test ! -e /usr/local/opt/z3 && test ! -L /usr/local/opt/z3', branch)
        self.assertIn('cmp -- "$z3_formula" "$tap_z3_formula"', branch)
        self.assertIn('patch -t -N -F 0 "$tap_formula" "$dependency_patch"', branch)
        self.assertLess(branch.index('intel-z3-inputs "$z3_formula"'), branch.index("brew tap-new"))
        self.assertLess(branch.index('intel-derived-formula \\\n'), branch.index('brew info --json=v2 "$target_formula"'))
        self.assertLess(branch.index('intel-z3-library "$evidence/intel-z3-library.json"'), branch.index('intel-headers "$prefix"'))
        for forbidden in ("--build-from-source", "--ignore-dependencies", "LLVM_PREFIX=", "LLVM_DIR=",
                          "check-llvm-cxx-abi.py", "cmake --build", "HOMEBREW_DEVELOPER", "HOMEBREW_INTERNAL_ALLOW",
                          'brew info --json=v2 "$archive"', '--formula "$archive"', '--skip-link "$archive"',
                          "brew unlink", "brew uninstall", "install_name_tool", "ln -s", "export DYLD"):
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
