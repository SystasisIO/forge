import copy
import difflib
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.dont_write_bytecode = True

import rust_quic_observer as observer
from check_stage6_acceptance import validate_quic_observer_provenance
from provenance import graph_hash, sha256_file


class RustQuicObserverCopyTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.dependencies = self.root / "original" / "fixture-deps"
        self.donor = self.dependencies / "rust-libp2p"
        self.observed_root = self.root / "observer"
        self.checkout = self.root / "donor-checkout"
        self.checkout.mkdir()
        self.source = b"// fixture prefix\n" * 110 + observer._DECLARATION
        self.patched_source = self.source.removesuffix(observer._DECLARATION) + observer.SOURCE_IMPLEMENTATION
        sources = {
            observer.QUIC_SOURCE: self.source,
            "Cargo.toml": b"[workspace]\n",
            "libp2p/Cargo.toml": b"[package]\nname = 'libp2p'\n",
            "protocols/stream/src/lib.rs": b"// stream fixture\n",
            "misc/multistream-select/src/lib.rs": b"// selection fixture\n",
            "scripts/check.sh": b"#!/bin/sh\nexit 0\n",
        }
        for relative, body in sources.items():
            target = self.donor / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(body)
            target.chmod(0o755 if relative.endswith(".sh") else 0o644)
        other = self.dependencies / "go-libp2p" / "sentinel"
        other.parent.mkdir()
        other.write_bytes(b"original other donor stays untouched\n")
        # A small archive is the unit's immutable Git-object double. Production
        # code always binds the real exported tree to the hard-coded donor pin.
        snapshot, _ = observer._snapshot(self.donor)
        pin = patch.object(observer, "PINNED_GIT_TREE", snapshot["git_tree"])
        pin.start()
        self.addCleanup(pin.stop)
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode="w") as archive:
            for relative in sorted(sources):
                member = tarfile.TarInfo(relative)
                member.size = len(sources[relative])
                member.mode = 0o755 if relative.endswith(".sh") else 0o644
                archive.addfile(member, io.BytesIO(sources[relative]))
        self.archive = buffer.getvalue()

    def prepare(self):
        return observer.prepare_observer_copy(self.dependencies, self.observed_root)

    def verify(self, receipt):
        observer.verify_observer_copy(receipt, self.dependencies, self.observed_root)

    def test_exact_standard_source_full_tree_copy_and_original_archive_binding(self):
        with patch.object(observer.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, stdout=self.archive,
        )) as command:
            observer.verify_original_export(self.dependencies, self.checkout)
        command.assert_called_once_with(
            ["git", "--no-replace-objects", "-C", str(self.checkout),
             "archive", "--format=tar", observer.PINNED_COMMIT],
            check=True, stdout=subprocess.PIPE,
        )
        before = observer.original_provenance(self.dependencies)
        receipt = self.prepare()
        self.verify(receipt)
        self.assertEqual(before, observer.original_provenance(self.dependencies))
        self.assertEqual((self.donor / observer.QUIC_SOURCE).read_bytes(), self.source)
        copied = self.observed_root / "fixture-deps" / "rust-libp2p"
        after = (copied / observer.QUIC_SOURCE).read_bytes()
        self.assertEqual(after, self.patched_source)
        self.assertNotIn(b"pub fn inner", after)
        self.assertNotIn(b"#[cfg(test)]", after)
        self.assertEqual(list((self.observed_root / "fixture-deps").iterdir()), [copied])
        self.assertEqual((self.dependencies / "go-libp2p" / "sentinel").read_bytes(),
                         b"original other donor stays untouched\n")
        self.assertEqual(receipt["commit"], observer.PINNED_COMMIT)
        self.assertEqual(receipt["cargo_feature"], "quic-cause-observer")
        self.assertEqual(receipt["proof_scope"], "instrumented_shutdown_only")
        changed = [name for name in before["files"]
                   if before["files"][name] != receipt["observed"]["files"][name]]
        self.assertEqual(changed, [observer.QUIC_SOURCE])
        self.assertEqual(receipt["original"]["tree_sha256"],
                         graph_hash(self.donor, sorted(before["files"])))
        self.assertEqual(receipt["observed"]["tree_sha256"],
                         graph_hash(copied, sorted(before["files"])))
        self.assertEqual(receipt["patch"]["source_before_sha256"],
                         sha256_file(self.donor / observer.QUIC_SOURCE))
        self.assertEqual(receipt["patch"]["source_after_sha256"],
                         sha256_file(copied / observer.QUIC_SOURCE))
        actual_diff = "".join(difflib.unified_diff(
            self.source.decode().splitlines(keepends=True), after.decode().splitlines(keepends=True),
            fromfile="a/" + observer.QUIC_SOURCE, tofile="b/" + observer.QUIC_SOURCE, n=1,
        ))
        self.assertEqual(actual_diff, observer.SOURCE_PATCH.decode())

    def test_recipe_matches_proposal_implementation_without_donor_unit_module(self):
        proposal = Path(__file__).with_name("rust_quic_source_proposal.patch").read_bytes()
        hunk = proposal.split(b"@@ -111,4 +111,60 @@\n", 1)[1]
        implementation, _ = hunk.split(b"+#[cfg(test)]\n", 1)
        reconstructed = b"".join(
            line[1:] for line in implementation.splitlines(keepends=True)
            if line.startswith((b" ", b"+"))
        )
        self.assertEqual(reconstructed, observer.SOURCE_IMPLEMENTATION + b"\n")

    def test_original_must_match_archive_not_self_declared_receipt(self):
        with patch.object(observer.subprocess, "run", return_value=subprocess.CompletedProcess(
            [], 0, stdout=self.archive,
        )):
            observer.verify_original_export(self.dependencies, self.checkout)
            (self.donor / "Cargo.toml").write_bytes(b"changed original\n")
            # Even a forged tree pin cannot satisfy the independently archived bytes.
            mutated, _ = observer._snapshot(self.donor)
            with patch.object(observer, "PINNED_GIT_TREE", mutated["git_tree"]):
                with self.assertRaisesRegex(ValueError, "differs from the pinned Git archive"):
                    observer.verify_original_export(self.dependencies, self.checkout)

    def test_wrong_pin_or_original_mutation_refuses_copy_before_creation(self):
        with patch.object(observer, "PINNED_GIT_TREE", "0" * 40):
            with self.assertRaises(ValueError):
                self.prepare()
        self.assertFalse(self.observed_root.exists())
        (self.donor / "Cargo.toml").write_bytes(b"mutated\n")
        with self.assertRaises(ValueError):
            self.prepare()
        self.assertFalse(self.observed_root.exists())

    def test_already_patched_source_is_not_reused(self):
        for source in (
            self.patched_source,
            self.source + b"\nimpl ConnectionError { pub fn inner(&self) -> &quinn::ConnectionError { &self.0 } }\n",
        ):
            with self.subTest(source=source[-100:]):
                (self.donor / observer.QUIC_SOURCE).write_bytes(source)
                altered, _ = observer._snapshot(self.donor)
                with patch.object(observer, "PINNED_GIT_TREE", altered["git_tree"]):
                    with self.assertRaises(ValueError):
                        self.prepare()
                self.assertFalse(self.observed_root.exists())

    def test_original_and_observer_symlinks_and_hardlinks_are_rejected(self):
        path = self.donor / "Cargo.toml"
        external = self.root / "external"
        external.write_bytes(path.read_bytes())
        path.unlink()
        path.symlink_to(external)
        with self.assertRaises(ValueError):
            self.prepare()
        path.unlink()
        os.link(external, path)
        with self.assertRaises(ValueError):
            self.prepare()
        path.unlink()
        shutil.copyfile(external, path)
        receipt = self.prepare()
        copied = self.observed_root / "fixture-deps" / "rust-libp2p" / "Cargo.toml"
        copied.unlink()
        copied.symlink_to(path)
        with self.assertRaises(ValueError):
            self.verify(receipt)
        copied.unlink()
        os.link(path, copied)
        with self.assertRaises(ValueError):
            self.verify(receipt)

    def test_symlink_root_overlap_and_existing_destination_are_rejected(self):
        alias = self.root / "alias"
        alias.symlink_to(self.dependencies, target_is_directory=True)
        with self.assertRaises(ValueError):
            observer.prepare_observer_copy(alias, self.observed_root)
        with self.assertRaises(ValueError):
            observer.prepare_observer_copy(self.dependencies, self.dependencies / "observer")
        alias.unlink()
        alias.symlink_to(self.root, target_is_directory=True)
        with self.assertRaises(ValueError):
            observer.prepare_observer_copy(self.dependencies, alias / "observer")
        receipt = self.prepare()
        with self.assertRaises(ValueError):
            self.prepare()
        self.verify(receipt)

    def test_receipt_hash_patch_membership_and_identity_mutations_are_rejected(self):
        receipt = self.prepare()
        for change in (
            lambda value: value.update(commit="0" * 40),
            lambda value: value.update(cargo_feature="default"),
            lambda value: value["original"].update(tree_sha256="0" * 64),
            lambda value: value["observed"].update(tree_sha256="0" * 64),
            lambda value: value["patch"].update(sha256="0" * 64),
            lambda value: value["patch"].update(unified_diff="different patch"),
            lambda value: value["patch"].update(source_after_sha256="0" * 64),
            lambda value: value["original"]["files"].pop("Cargo.toml"),
            lambda value: value.update(binary={"path": "unverified"}),
        ):
            with self.subTest(change=change):
                forged = copy.deepcopy(receipt)
                change(forged)
                with self.assertRaises(ValueError):
                    self.verify(forged)

    def test_observed_wrong_source_other_file_extra_file_and_mode_are_rejected(self):
        receipt = self.prepare()
        copied = self.observed_root / "fixture-deps" / "rust-libp2p"
        target = copied / observer.QUIC_SOURCE
        for wrong in (
            self.patched_source.replace(b"Some(&self.0)", b"None"),
            self.patched_source.replace(b"std::fmt::Display::fmt(&self.0, formatter)", b"write!(formatter, \"changed\")"),
            self.patched_source + b"\n#[cfg(test)]\nmod unregistered_donor_tests {}\n",
            self.source + b"\nimpl ConnectionError { pub fn inner(&self) -> &quinn::ConnectionError { &self.0 } }\n",
        ):
            with self.subTest(source=wrong[-100:]):
                target.write_bytes(wrong)
                with self.assertRaises(ValueError):
                    self.verify(receipt)
                # Updating the declared after-hashes cannot authorize another recipe.
                observed, _ = observer._snapshot(copied)
                forged = observer._receipt(self.dependencies, self.observed_root,
                                           receipt["original"], observed)
                with self.assertRaises(ValueError):
                    self.verify(forged)
        target.write_bytes(self.patched_source)
        original = (copied / "Cargo.toml").read_bytes()
        (copied / "Cargo.toml").write_bytes(b"changed unrelated module\n")
        with self.assertRaises(ValueError):
            self.verify(receipt)
        (copied / "Cargo.toml").write_bytes(original)
        (copied / "extra.rs").write_bytes(b"// extra\n")
        with self.assertRaises(ValueError):
            self.verify(receipt)
        (copied / "extra.rs").unlink()
        target.chmod(0o755)
        with self.assertRaises(ValueError):
            self.verify(receipt)

    def test_archive_symlinks_paths_and_duplicates_are_rejected(self):
        for case in ("symlink", "traversal", "duplicate"):
            with self.subTest(case=case):
                buffer = io.BytesIO()
                with tarfile.open(fileobj=buffer, mode="w") as archive:
                    member = tarfile.TarInfo("../outside" if case == "traversal" else "Cargo.toml")
                    if case == "symlink":
                        member.type = tarfile.SYMTYPE
                        member.linkname = "outside"
                    archive.addfile(member)
                    if case == "duplicate":
                        archive.addfile(member)
                with patch.object(observer.subprocess, "run", return_value=subprocess.CompletedProcess(
                    [], 0, stdout=buffer.getvalue(),
                )):
                    with self.assertRaises(ValueError):
                        observer.verify_original_export(self.dependencies, self.checkout)

    def test_copy_file_and_byte_bounds_are_fail_closed(self):
        for limit in ("FILE_LIMIT", "FILE_BYTE_LIMIT", "TREE_BYTE_LIMIT"):
            with self.subTest(limit=limit), patch.object(observer, limit, 1):
                with self.assertRaises(ValueError):
                    self.prepare()
        self.assertFalse(self.observed_root.exists())

    def test_actual_fixture_source_copy_hashes_are_bound_to_explicit_membership(self):
        fixture = self.root / "fixture"
        fixture.mkdir()
        (fixture / "Cargo.toml").write_bytes(b"[features]\nquic-cause-observer = []\n")
        (fixture / "main.rs").write_bytes(b"fn main() {}\n")
        copied = self.root / "fixture-copy"
        shutil.copytree(fixture, copied)
        names = ["main.rs", "Cargo.toml"]
        before = observer.fixture_source_provenance(fixture, names)
        self.assertEqual(before, observer.fixture_source_provenance(copied, names))
        (copied / "main.rs").write_bytes(b"fn main() { panic!() }\n")
        self.assertNotEqual(before, observer.fixture_source_provenance(copied, names))
        for names in (["../outside"], ["main.rs", "main.rs"], ["target/binary"], [True]):
            with self.assertRaises(ValueError):
                observer.fixture_source_provenance(fixture, names)


class RustQuicObserverAcceptanceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name).resolve()
        self.build = self.root / "build"
        self.source_dir = self.root / "sources"
        self.source = self.source_dir / "rust_fixture"
        self.observer_root = self.build / "quic-observer"
        self.copied = self.observer_root / "rust_fixture"
        self.donors = self.root / "donors"
        sources = {
            "Cargo.toml": b"[features]\nquic-cause-observer = []\n",
            "Cargo.lock": b"version = 4\n",
            "main.rs": b"mod pubsub_scoring;\nfn main() {}\n",
            "pubsub_scoring/observer.rs": b"// current native cause only\n",
            "pubsub_scoring/observer_tests.rs": b"// observer regression source\n",
        }
        for relative, body in sources.items():
            path = self.source / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(body)
        self.copied.parent.mkdir(parents=True)
        shutil.copytree(self.source, self.copied)
        self.original_binary = self.build / "rust_fixture/target/release/forge-libp2p-rust-fixture"
        self.binary = self.copied / "target/release/forge-libp2p-rust-fixture"
        for path, body in ((self.original_binary, b"original Rust fixture\0"),
                           (self.binary, b"instrumented Rust fixture\0")):
            path.parent.mkdir(parents=True)
            path.write_bytes(body)
        cargo = str(self.root / "tools/cargo")
        self.commands = [
            {"command": [cargo, "test", "--frozen", "--features", "quic-cause-observer"],
             "cwd": str(self.copied),
             "environment": {"CARGO_NET_OFFLINE": "true", "RUSTUP_OFFLINE": "true"}},
            {"command": [cargo, "build", "--release", "--frozen", "--features", "quic-cause-observer"],
             "cwd": str(self.copied),
             "environment": {"CARGO_NET_OFFLINE": "true", "RUSTUP_OFFLINE": "true"}},
        ]
        # Full donor recipe validation is covered above. Only those two donor
        # checks are mocked here; fixture hashing and binary checks stay real.
        self.recipe = {"schema": "forge.rust-quic-cause-observer-copy.v1",
                       "donor": "rust-libp2p", "commit": observer.PINNED_COMMIT,
                       "cargo_feature": "quic-cause-observer"}
        self.value = {"tools": {"cargo": {"path": cargo}},
                      "rust_quic_observer": {
                          **self.recipe,
                          "commands": copy.deepcopy(self.commands),
                          "binary": {"path": str(self.binary), "sha256": sha256_file(self.binary)},
                      }}
        self.inputs = {"build_dir": self.build, "source_dir": self.source_dir,
                       "donors_root": self.donors}
        original_check = patch.object(observer, "verify_original_export", return_value=None)
        copy_check = patch.object(observer, "verify_observer_copy", return_value=None)
        self.original_check = original_check.start()
        self.copy_check = copy_check.start()
        self.addCleanup(original_check.stop)
        self.addCleanup(copy_check.stop)

    def validate(self, value=None, original_binary=None):
        paths = {"rust": self.original_binary if original_binary is None else original_binary}
        errors = validate_quic_observer_provenance(
            self.value if value is None else value, self.inputs, paths,
        )
        return errors, paths

    def assert_rejected(self, value=None, message=None, original_binary=None):
        errors, paths = self.validate(value, original_binary)
        self.assertEqual(len(errors), 1)
        if message is not None:
            self.assertIn(message, errors[0])
        self.assertNotIn("rust-quic-observer", paths)

    def test_accepts_matching_original_copy_binary_and_two_exact_feature_commands(self):
        errors, paths = self.validate()
        self.assertEqual(errors, [])
        self.assertEqual(paths, {"rust": self.original_binary, "rust-quic-observer": self.binary})
        self.original_check.assert_called_once_with(
            self.build / "fixture-deps", self.donors / "rust-libp2p",
        )
        self.copy_check.assert_called_once_with(
            self.recipe, self.build / "fixture-deps", self.observer_root,
        )
        names = sorted(path.relative_to(self.source).as_posix()
                       for path in self.source.rglob("*") if path.is_file())
        self.assertEqual(observer.fixture_source_provenance(self.source, names),
                         observer.fixture_source_provenance(self.copied, names))
        self.assertEqual(self.value["rust_quic_observer"]["commands"], self.commands)

    def test_rejects_wrong_cargo_commands_features_order_cwd_and_environment(self):
        for operation in (0, 1):
            for change in ("command", "cargo", "feature", "missing_feature", "all_features",
                           "unfrozen", "cwd", "environment"):
                with self.subTest(operation=operation, change=change):
                    value = copy.deepcopy(self.value)
                    record = value["rust_quic_observer"]["commands"][operation]
                    command = record["command"]
                    if change == "command":
                        command[1] = "check"
                    elif change == "cargo":
                        command[0] = str(self.root / "foreign-cargo")
                    elif change == "feature":
                        command[-1] = "quic-cause-observer,extra"
                    elif change == "missing_feature":
                        del command[-2:]
                    elif change == "all_features":
                        command[-2:] = ["--all-features"]
                    elif change == "unfrozen":
                        command.remove("--frozen")
                    elif change == "cwd":
                        record["cwd"] = str(self.build / "rust_fixture")
                    else:
                        record["environment"]["CARGO_NET_OFFLINE"] = "false"
                    self.assert_rejected(value, "Cargo commands/features changed")
        for commands in (self.commands[:1], list(reversed(self.commands)),
                         self.commands + [self.commands[0]]):
            with self.subTest(commands=commands):
                value = copy.deepcopy(self.value)
                value["rust_quic_observer"]["commands"] = copy.deepcopy(commands)
                self.assert_rejected(value, "Cargo commands/features changed")

    def test_rejects_changed_fixture_source(self):
        (self.copied / "pubsub_scoring/observer.rs").write_bytes(b"// changed observer policy\n")
        self.assert_rejected(message="fixture source differs from locked original source")

    def test_rejects_missing_and_extra_fixture_sources(self):
        missing = self.copied / "pubsub_scoring/observer_tests.rs"
        original = missing.read_bytes()
        missing.unlink()
        self.assert_rejected(message="fixture source differs from locked original source")
        missing.write_bytes(original)
        (self.copied / "extra.rs").write_bytes(b"// unregistered fixture module\n")
        self.assert_rejected(message="fixture source differs from locked original source")

    def test_rejects_wrong_binary_hash_and_changed_actual_binary_bytes(self):
        value = copy.deepcopy(self.value)
        value["rust_quic_observer"]["binary"]["sha256"] = "0" * 64
        self.assert_rejected(value, "observer binary/hash aliases original Rust")
        self.binary.write_bytes(b"mutated instrumented binary\0")
        self.assert_rejected(message="observer binary/hash aliases original Rust")

    def test_rejects_foreign_original_and_missing_binary_paths(self):
        foreign = self.root / "foreign-binary"
        foreign.write_bytes(self.binary.read_bytes())
        for path in (foreign, self.original_binary):
            with self.subTest(path=path):
                value = copy.deepcopy(self.value)
                value["rust_quic_observer"]["binary"] = {
                    "path": str(path), "sha256": sha256_file(path),
                }
                self.assert_rejected(value, "observer binary/hash aliases original Rust")
        self.assert_rejected(message="observer binary/hash aliases original Rust",
                             original_binary=self.binary)
        self.binary.unlink()
        self.assert_rejected(message="observer binary/hash aliases original Rust")

    def test_rejects_missing_receipt_binary_or_commands(self):
        for receipt in (None, "invalid"):
            with self.subTest(receipt=receipt):
                value = copy.deepcopy(self.value)
                value["rust_quic_observer"] = receipt
                self.assert_rejected(value)
        value = copy.deepcopy(self.value)
        del value["rust_quic_observer"]
        self.assert_rejected(value, "lacks explicit Rust QUIC observer provenance")
        for key in ("binary", "commands"):
            with self.subTest(missing=key):
                value = copy.deepcopy(self.value)
                del value["rust_quic_observer"][key]
                self.assert_rejected(value, "invalid observer receipt")
        self.original_check.assert_not_called()
        self.copy_check.assert_not_called()


if __name__ == "__main__":
    unittest.main()
