#!/usr/bin/env python3
"""Small metadata-only regression suite; no compiler, download or source build."""

import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
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


class ProvisioningTests(unittest.TestCase):
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
