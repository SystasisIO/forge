#!/usr/bin/env python3
import json
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Optional
from unittest.mock import patch

sys.dont_write_bytecode = True

from provenance import (
    FIXTURE_DONOR_DIRECTORIES,
    donor_checkout_head_errors,
    donor_revision_schema_errors,
    donor_source_object_errors,
    fixture_donor_revision_bindings,
    worktree_identity,
)
from check_p2p_feature_inventory import (
    donor_case_source_errors,
    private_mdns_attribution_errors,
    registered_runner_acceptance_pairs,
    registered_runner_pair_errors,
)
from runner import (
    SUPPORTED_FORGE_BUILD_PROFILES,
    forge_fixture_requirements,
    prepare_rust_fixture,
    require_dht_provider_evidence,
    require_hidden_dht_find_peer_evidence,
    require_local_topology_evidence,
    require_supported_forge_build_profile,
    run_dial,
)
from test_provider_evidence import valid_hidden_find_peer_result, valid_result
from promote_stage6_acceptance import (
    CANONICAL_ACCEPTANCE_MANIFEST,
    PROMOTION_DIRECTORY_PREFIX,
    create_invocation_directory,
    forced_live_environment,
    promotion_status,
    resolve_canonical_acceptance_manifest,
)


def git(root: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(root), *args], text=True).strip()


def initialize_repository(root: Path) -> None:
    root.mkdir()
    git(root, "init")
    git(root, "config", "user.name", "Forge provenance test")
    git(root, "config", "user.email", "forge-provenance@example.invalid")


def commit(root: Path, message: str) -> str:
    git(root, "add", ".")
    git(root, "commit", "-m", message)
    return git(root, "rev-parse", "HEAD")


def add_submodule(root: Path, source: Path, destination: str) -> None:
    git(root, "-c", "protocol.file.allow=always", "submodule", "add", str(source), destination)


class WorktreeFingerprintTest(unittest.TestCase):
    def make_uninitialized_gitlink(self, temporary: Path) -> Path:
        source = temporary / "source"
        initialize_repository(source)
        (source / "source.txt").write_text("source\n")
        source_head = commit(source, "source")

        root = temporary / "root"
        initialize_repository(root)
        (root / "root.txt").write_text("root\n")
        commit(root, "root")
        git(root, "update-index", "--add", "--cacheinfo", f"160000,{source_head},vendor/empty")
        git(root, "commit", "-m", "indexed gitlink")
        (root / "vendor" / "empty").mkdir(parents=True)
        return root

    def make_nested_worktree(self, temporary: Path) -> tuple[Path, str, str, str]:
        leaf = temporary / "leaf"
        initialize_repository(leaf)
        (leaf / "leaf.txt").write_text("first\n")
        leaf_first = commit(leaf, "leaf first")
        (leaf / "leaf.txt").write_text("second\n")
        leaf_second = commit(leaf, "leaf second")
        git(leaf, "checkout", leaf_first)

        module = temporary / "module"
        initialize_repository(module)
        add_submodule(module, leaf, "nested")
        (module / "module.txt").write_text("first\n")
        module_first = commit(module, "module first")
        (module / "module.txt").write_text("second\n")
        module_second = commit(module, "module second")
        git(module, "checkout", module_first)

        root = temporary / "root"
        initialize_repository(root)
        add_submodule(root, module, "module")
        commit(root, "root")
        git(root, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive")
        return root, module_first, module_second, leaf_second

    def restore_clean_submodules(self, root: Path) -> None:
        git(root, "-c", "protocol.file.allow=always", "submodule", "update", "--init", "--recursive", "--force")
        git(root / "module", "clean", "-fd")
        git(root / "module" / "nested", "clean", "-fd")

    def test_fingerprint_tracks_direct_and_nested_submodule_state(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root, module_first, module_second, leaf_second = self.make_nested_worktree(Path(directory))
            clean = worktree_identity(root)
            self.assertFalse(clean.dirty)

            git(root / "module", "checkout", module_second)
            different_checkout = worktree_identity(root)
            self.assertNotEqual(different_checkout.fingerprint, clean.fingerprint)
            self.assertTrue(different_checkout.dirty)
            self.restore_clean_submodules(root)
            self.assertEqual(worktree_identity(root), clean)
            self.assertEqual(git(root / "module", "rev-parse", "HEAD"), module_first)

            (root / "module" / "module.txt").write_text("dirty\n")
            dirty_tracked = worktree_identity(root)
            self.assertNotEqual(dirty_tracked.fingerprint, clean.fingerprint)
            self.assertTrue(dirty_tracked.dirty)
            self.restore_clean_submodules(root)
            self.assertEqual(worktree_identity(root), clean)

            module_file = root / "module" / "module.txt"
            module_file.write_text("dirty before chmod\n")
            dirty_before_chmod = worktree_identity(root)
            module_file.chmod(module_file.stat().st_mode ^ stat.S_IXUSR)
            dirty_after_chmod = worktree_identity(root)
            self.assertTrue(dirty_before_chmod.dirty)
            self.assertTrue(dirty_after_chmod.dirty)
            self.assertNotEqual(dirty_after_chmod.fingerprint, dirty_before_chmod.fingerprint)
            self.restore_clean_submodules(root)
            self.assertEqual(worktree_identity(root), clean)

            (root / "module" / "untracked.txt").write_text("untracked\n")
            dirty_untracked = worktree_identity(root)
            self.assertNotEqual(dirty_untracked.fingerprint, clean.fingerprint)
            self.assertTrue(dirty_untracked.dirty)
            self.restore_clean_submodules(root)
            self.assertEqual(worktree_identity(root), clean)

            git(root / "module" / "nested", "checkout", leaf_second)
            dirty_nested = worktree_identity(root)
            self.assertNotEqual(dirty_nested.fingerprint, clean.fingerprint)
            self.assertTrue(dirty_nested.dirty)
            self.restore_clean_submodules(root)
            self.assertEqual(worktree_identity(root), clean)

    def test_empty_uninitialized_gitlink_is_clean_and_deterministic(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.make_uninitialized_gitlink(Path(directory))
            first = worktree_identity(root)
            second = worktree_identity(root)
            self.assertFalse(first.dirty)
            self.assertEqual(first, second)

    def test_nonempty_uninitialized_gitlink_is_dirty(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = self.make_uninitialized_gitlink(Path(directory))
            clean = worktree_identity(root)
            (root / "vendor" / "empty" / "unexpected.txt").write_text("not a repository\n")
            invalid = worktree_identity(root)
            self.assertTrue(invalid.dirty)
            self.assertNotEqual(invalid.fingerprint, clean.fingerprint)

    def test_initialized_nested_submodule_remains_clean(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root, _, _, _ = self.make_nested_worktree(Path(directory))
            self.assertTrue((root / "module" / ".git").is_file())
            self.assertTrue((root / "module" / "nested" / ".git").is_file())
            self.assertFalse(worktree_identity(root).dirty)


class DonorCheckoutPinTest(unittest.TestCase):
    def test_donor_checkout_helper_accepts_matching_and_rejects_stale_head(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            donors_root = Path(directory) / "donors"
            donors_root.mkdir()
            donor = donors_root / "pinned-donor"
            initialize_repository(donor)
            (donor / "source.txt").write_text("first\n")
            pinned_head = commit(donor, "first")

            self.assertEqual(
                donor_checkout_head_errors(donors_root, {"pinned-donor": pinned_head}), []
            )

            (donor / "source.txt").write_text("stale\n")
            commit(donor, "second")
            self.assertEqual(
                donor_checkout_head_errors(donors_root, {"pinned-donor": pinned_head}),
                ["donor pinned-donor: checkout does not match pinned revision"],
            )

    def test_donor_revision_schema_is_fail_closed(self) -> None:
        full_revision = "a" * 40
        invalid_cases = (
            (None, "donor_revisions must be a non-empty object"),
            ({}, "donor_revisions must be a non-empty object"),
            ({"": full_revision}, "invalid donor repository name ''"),
            ({"../escape": full_revision}, "invalid donor repository name '../escape'"),
            ({"pinned-donor": True}, "donor pinned-donor: revision must be a full lowercase commit SHA"),
            ({"pinned-donor": 1}, "donor pinned-donor: revision must be a full lowercase commit SHA"),
            ({"pinned-donor": "a" * 39}, "donor pinned-donor: revision must be a full lowercase commit SHA"),
            ({"pinned-donor": "A" * 40}, "donor pinned-donor: revision must be a full lowercase commit SHA"),
        )
        for revisions, expected_error in invalid_cases:
            with self.subTest(revisions=revisions):
                self.assertEqual(donor_revision_schema_errors(revisions), [expected_error])
                self.assertEqual(
                    donor_checkout_head_errors(Path("/does-not-matter"), revisions),
                    [expected_error],
                )

    def test_fixture_donor_binding_rejects_an_alternate_commit_with_the_same_tree(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            donors_root = Path(directory) / "donors"
            donors_root.mkdir()
            donor = donors_root / "pinned-donor"
            initialize_repository(donor)
            (donor / "source.txt").write_text("same package source\n")
            canonical_commit = commit(donor, "canonical")
            canonical_tree = git(donor, "rev-parse", "HEAD^{tree}")
            git(donor, "commit", "--allow-empty", "-m", "alternate metadata only")
            alternate_commit = git(donor, "rev-parse", "HEAD")
            self.assertNotEqual(alternate_commit, canonical_commit)
            self.assertEqual(git(donor, "rev-parse", "HEAD^{tree}"), canonical_tree)

            canonical_revisions = {
                directory: "a" * 40 for directory in FIXTURE_DONOR_DIRECTORIES.values()
            }
            canonical_revisions["go-libp2p"] = canonical_commit
            fixture_donors = [
                {
                    "name": name,
                    "directory": directory,
                    "commit": (
                        alternate_commit if directory == "go-libp2p" else canonical_revisions[directory]
                    ),
                    "tree": canonical_tree,
                }
                for name, directory in FIXTURE_DONOR_DIRECTORIES.items()
            ]
            bindings, errors = fixture_donor_revision_bindings(
                fixture_donors, canonical_revisions
            )
            self.assertNotIn("go-libp2p", bindings)
            self.assertEqual(
                errors,
                [
                    "fixture lock donor go-libp2p: "
                    "commit does not match canonical donor_cases revision"
                ],
            )

    def test_fixture_donor_registry_rejects_swapped_names_and_directories(self) -> None:
        canonical_revisions = {
            directory: f"{index:040x}"
            for index, directory in enumerate(FIXTURE_DONOR_DIRECTORIES.values(), start=1)
        }
        fixture_donors = [
            {
                "name": name,
                "directory": directory,
                "commit": canonical_revisions[directory],
                "tree": f"{index:040x}",
            }
            for index, (name, directory) in enumerate(FIXTURE_DONOR_DIRECTORIES.items(), start=11)
        ]
        bindings, errors = fixture_donor_revision_bindings(fixture_donors, canonical_revisions)
        self.assertEqual(bindings, canonical_revisions)
        self.assertEqual(errors, [])

        swapped = [dict(donor) for donor in fixture_donors]
        swapped[0]["directory"], swapped[1]["directory"] = (
            swapped[1]["directory"], swapped[0]["directory"]
        )
        _, errors = fixture_donor_revision_bindings(swapped, canonical_revisions)
        self.assertIn(
            "fixture lock donor go-libp2p: directory must be go-libp2p", errors
        )
        self.assertIn(
            "fixture lock donor rust-libp2p: directory must be rust-libp2p", errors
        )

        substituted = [dict(donor) for donor in fixture_donors]
        substituted[0]["directory"] = "boxo"
        substituted[0]["commit"] = "f" * 40
        _, errors = fixture_donor_revision_bindings(
            substituted, canonical_revisions | {"boxo": "f" * 40}
        )
        self.assertIn(
            "fixture lock donor go-libp2p: directory must be go-libp2p", errors
        )
        self.assertIn(
            "fixture lock donor directories do not match the canonical fixture donor registry", errors
        )

    def test_donor_source_must_exist_as_a_blob_at_the_pinned_revision(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            donors_root = Path(directory) / "donors"
            donors_root.mkdir()
            donor = donors_root / "pinned-donor"
            initialize_repository(donor)
            (donor / "tracked.txt").write_text("tracked\n")
            pinned_commit = commit(donor, "pinned")
            revisions = {"pinned-donor": pinned_commit}
            self.assertEqual(
                donor_source_object_errors(
                    donors_root, revisions, "donors/pinned-donor/tracked.txt"
                ),
                [],
            )

            (donor / "untracked.txt").write_text("mutable checkout file\n")
            self.assertEqual(
                donor_source_object_errors(
                    donors_root, revisions, "donors/pinned-donor/untracked.txt"
                ),
                [
                    "donor source is absent from pinned revision: "
                    "donors/pinned-donor/untracked.txt"
                ],
            )

    def test_inventory_donor_case_sources_reject_an_untracked_pinned_path(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "source"
            root.mkdir()
            donors_root = Path(directory) / "donors"
            donors_root.mkdir()
            donor = donors_root / "go-libp2p"
            initialize_repository(donor)
            (donor / "tracked.txt").write_text("tracked\n")
            revision = commit(donor, "pinned")
            cases = [{
                "id": "registered-case",
                "mapping_state": "registered",
                "donor_file": ["donors/go-libp2p/tracked.txt"],
            }]
            self.assertEqual(
                donor_case_source_errors(root, cases, {"go-libp2p": revision}, donors_root),
                [],
            )

            (donor / "untracked.txt").write_text("mutable checkout path\n")
            cases[0]["donor_file"] = ["donors/go-libp2p/untracked.txt"]
            errors = donor_case_source_errors(root, cases, {"go-libp2p": revision}, donors_root)
            self.assertEqual(errors, [
                "donor case registered-case: donor source is absent from pinned revision: "
                "donors/go-libp2p/untracked.txt"
            ])
            self.assertEqual(promotion_status(0, errors), "FAILED")

    def test_private_mdns_requires_spec_and_fingerprint_attribution(self) -> None:
        root = Path(__file__).parents[2]
        source = root / "tests/libp2p_interop"
        original = json.loads((source / "p2p_donor_capabilities.json").read_text())
        expected = (
            "donor capability discovery.mdns_private_fingerprinted: must attribute the namespace "
            "to the libp2p specification and fingerprint computation to pinned Rust pnet"
        )
        for mutation in ("none", "forge_origin", "missing_spec_donor", "missing_fingerprint_donor", "invalid_sources"):
            with self.subTest(mutation=mutation):
                document = json.loads(json.dumps(original))
                entry = next(item for item in document["capabilities"]
                             if item["id"] == "discovery.mdns_private_fingerprinted")
                if mutation == "forge_origin":
                    entry["origin"] = "forge_extension"
                elif mutation == "missing_spec_donor":
                    entry["donor_sources"].remove("donors/libp2p-specs/discovery/mdns.md")
                elif mutation == "missing_fingerprint_donor":
                    entry["donor_sources"].remove("donors/rust-libp2p/transports/pnet/src/lib.rs")
                elif mutation == "invalid_sources":
                    entry["donor_sources"] = None
                self.assertEqual(private_mdns_attribution_errors(entry),
                                 [] if mutation == "none" else [expected])

    def test_matching_invalid_revision_maps_block_inventory_and_donor_gates(self) -> None:
        root = Path(__file__).parents[2]
        donor_source = json.loads((root / "tests/libp2p_interop/donor_cases.json").read_text())
        capability_source = json.loads(
            (root / "tests/libp2p_interop/p2p_donor_capabilities.json").read_text()
        )
        original_revisions = donor_source["donor_revisions"]
        cases = (
            ("numeric", 1),
            ("short", "deadbeef"),
        )

        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            donor_path = temporary / "donor_cases.json"
            capability_path = temporary / "p2p_donor_capabilities.json"
            for name, invalid_revision in cases:
                with self.subTest(revision=name):
                    invalid_revisions = {
                        repository: invalid_revision for repository in original_revisions
                    }
                    donor_source["donor_revisions"] = invalid_revisions
                    capability_source["donor_revisions"] = invalid_revisions
                    donor_path.write_text(json.dumps(donor_source))
                    capability_path.write_text(json.dumps(capability_source))

                    matrix = subprocess.run(
                        [
                            sys.executable,
                            str(root / "tests/libp2p_interop/check_donor_matrix.py"),
                            str(root),
                            str(donor_path),
                        ],
                        capture_output=True,
                        text=True,
                    )
                    inventory = subprocess.run(
                        [
                            sys.executable,
                            str(root / "tests/libp2p_interop/check_p2p_feature_inventory.py"),
                            str(root),
                            str(root / "tests/libp2p_interop/p2p_feature_inventory.json"),
                            str(capability_path),
                            str(donor_path),
                        ],
                        capture_output=True,
                        text=True,
                    )
                    expected_error = (
                        "donor go-libp2p: revision must be a full lowercase commit SHA"
                    )
                    self.assertNotEqual(matrix.returncode, 0, matrix.stderr)
                    self.assertIn(expected_error, matrix.stderr)
                    self.assertNotEqual(inventory.returncode, 0, inventory.stderr)
                    self.assertIn(expected_error, inventory.stderr)
                    self.assertEqual(
                        promotion_status(0, donor_checkout_head_errors(temporary, invalid_revisions)),
                        "FAILED",
                    )


class InteropCMakeConfigurationTest(unittest.TestCase):
    def test_multi_config_artifacts_are_configuration_scoped(self) -> None:
        source = (Path(__file__).parents[1] / "CMakeLists.txt").read_text()
        self.assertIn(
            "set(FORGE_INTEROP_ARTIFACT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/libp2p_interop)", source
        )
        self.assertIn(
            "set(FORGE_INTEROP_ARTIFACT_DIRECTORY ${FORGE_INTEROP_ARTIFACT_DIRECTORY}/$<CONFIG>)", source
        )
        self.assertIn(
            "set(FORGE_INTEROP_BUILD_INFO_HEADER ${FORGE_INTEROP_BUILD_INFO_DIRECTORY}/forge_interop_build_info.hxx)",
            source,
        )
        self.assertIn(
            "set(FORGE_INTEROP_BUILD_INFO_STAMP ${FORGE_INTEROP_BUILD_INFO_DIRECTORY}/forge_interop_build_info.json)",
            source,
        )
        self.assertEqual(source.count("--build-dir ${FORGE_INTEROP_ARTIFACT_DIRECTORY}"), 3)
        self.assertIn(
            "set(\n      FORGE_P2P_STAGE6_PROMOTION_DIRECTORY\n      ${FORGE_P2P_STAGE6_PROMOTION_DIRECTORY}/$<CONFIG>\n   )",
            source,
        )
        self.assertIn("add_dependencies(forge_interop_fixture forge_interop_fixture_build_info)", source)
        self.assertNotIn("OBJECT_DEPENDS", source)


class InteropRunnerResultTest(unittest.TestCase):
    def test_prepare_rust_fixture_records_and_runs_frozen_tests_before_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_dir = root / "source"
            rust_fixture = source_dir / "rust_fixture"
            rust_fixture.mkdir(parents=True)
            (rust_fixture / "Cargo.toml").write_text("[package]\nname = 'fixture'\n")
            calls: list[tuple[list[str], Optional[Path], Optional[dict[str, str]]]] = []

            def fake_run(command: list[str], cwd: Optional[Path] = None,
                         env: Optional[dict[str, str]] = None) -> None:
                calls.append((command, cwd, env))

            with patch("runner.run", side_effect=fake_run):
                binary, commands = prepare_rust_fixture(
                    source_dir, root / "build", "/tools/cargo", {"PATH": "/tools"}
                )

            expected_commands = [
                ["/tools/cargo", "test", "--frozen"],
                ["/tools/cargo", "build", "--release", "--frozen"],
            ]
            self.assertEqual([record["command"] for record in commands], expected_commands)
            self.assertEqual([command for command, _, _ in calls], expected_commands)
            self.assertEqual(binary, root / "build/rust_fixture/target/release/forge-libp2p-rust-fixture")
            self.assertTrue(all(record["environment"]["CARGO_NET_OFFLINE"] == "true" for record in commands))
            self.assertTrue(all(record["environment"]["RUSTUP_OFFLINE"] == "true" for record in commands))

    def test_prepare_rust_fixture_refreshes_source_without_following_links_or_discarding_target(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source_dir = root / "source"
            rust_fixture = source_dir / "rust_fixture"
            (rust_fixture / "src").mkdir(parents=True)
            (rust_fixture / "Cargo.toml").write_text("[package]\nname = 'fixture'\n")
            (rust_fixture / "src/main.rs").write_text("fn main() {}\n")
            work = root / "build/rust_fixture"
            (work / "src").mkdir(parents=True)
            (work / "src/stale_module.rs").write_text("stale\n")
            (work / "target").mkdir()
            (work / "target/cache-sentinel").write_text("keep\n")
            outside = root / "outside"
            outside.mkdir()
            (outside / "sentinel").write_text("do not remove\n")
            (work / "stale-link").symlink_to(outside, target_is_directory=True)

            with patch("runner.run"):
                prepare_rust_fixture(source_dir, root / "build", "/tools/cargo", {"PATH": "/tools"})

            self.assertFalse((work / "src/stale_module.rs").exists())
            self.assertEqual((work / "src/main.rs").read_text(), "fn main() {}\n")
            self.assertEqual((work / "target/cache-sentinel").read_text(), "keep\n")
            self.assertFalse((work / "stale-link").exists())
            self.assertEqual((outside / "sentinel").read_text(), "do not remove\n")

    def test_registered_acceptance_pairs_require_tcp_identify(self) -> None:
        runner_pairs = registered_runner_acceptance_pairs(Path(__file__).with_name("runner.py"))
        identify_pair = ("tcp_noise/identify", "identify_native_tcp_yamux")
        self.assertIn(identify_pair, runner_pairs)

        omitted_pair_errors = registered_runner_pair_errors(runner_pairs - {identify_pair}, runner_pairs)

        self.assertTrue(
            any(
                "missing" in error
                and "tcp_noise/identify -> identify_native_tcp_yamux" in error
                for error in omitted_pair_errors
            )
        )

    def test_run_dial_rejects_non_ok_fixture_result(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            fixture = root / "non_ok_fixture.py"
            fixture.write_text(
                f"#!{sys.executable}\n"
                "import json\n"
                "import sys\n"
                "from pathlib import Path\n"
                "result_file = Path(sys.argv[sys.argv.index('--result-file') + 1])\n"
                "result_file.write_text(json.dumps({'status': 'failed'}) + '\\n')\n"
            )
            fixture.chmod(fixture.stat().st_mode | stat.S_IXUSR)
            with self.assertRaisesRegex(RuntimeError, "did not report status=ok"):
                run_dial(
                    fixture,
                    "fixture",
                    "ping",
                    "peer",
                    "/ip4/127.0.0.1/tcp/1",
                    root,
                )


class Stage6PromotionHelperTest(unittest.TestCase):
    def test_promotion_accepts_only_the_source_tree_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            canonical = root / CANONICAL_ACCEPTANCE_MANIFEST
            canonical.parent.mkdir(parents=True)
            canonical.write_text("{}\n")
            self.assertEqual(resolve_canonical_acceptance_manifest(root, str(canonical)), canonical.resolve())
            external = root / "minimal-manifest.json"
            external.write_text("{}\n")
            with self.assertRaisesRegex(ValueError, "must resolve exactly"):
                resolve_canonical_acceptance_manifest(root, str(external))

    def test_invocation_directories_are_unique_children_of_the_configured_base(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory) / "promotion-base"
            first = create_invocation_directory(base)
            second = create_invocation_directory(base)
            self.assertEqual(first.parent, base)
            self.assertEqual(second.parent, base)
            self.assertNotEqual(first, second)
            self.assertTrue(first.name.startswith(PROMOTION_DIRECTORY_PREFIX))
            self.assertTrue(second.name.startswith(PROMOTION_DIRECTORY_PREFIX))

    def test_promotion_forces_live_environment_and_checker_errors_fail(self) -> None:
        inherited = {"FORGE_ENABLE_LIBP2P_INTEROP": "0", "OTHER": "value"}
        environment = forced_live_environment(inherited)
        self.assertEqual(environment["FORGE_ENABLE_LIBP2P_INTEROP"], "1")
        self.assertEqual(environment["OTHER"], "value")
        self.assertEqual(promotion_status(0, ["artifact is missing"]), "FAILED")
        self.assertEqual(promotion_status(1, []), "FAILED")


class InteropFixtureContractTest(unittest.TestCase):
    def fixture_lock(self, build_profiles: object = None) -> dict:
        return {
            "schema_version": 2,
            "toolchains": {
                "forge_fixture": {
                    "compiler_id": "Clang",
                    "compiler_version": "22.1.8",
                    "build_profiles": list(SUPPORTED_FORGE_BUILD_PROFILES)
                    if build_profiles is None else build_profiles,
                },
            },
        }

    def test_forge_fixture_accepts_each_locked_build_profile(self) -> None:
        requirements = forge_fixture_requirements(self.fixture_lock())
        for profile in SUPPORTED_FORGE_BUILD_PROFILES:
            require_supported_forge_build_profile(profile, requirements)

    def test_forge_fixture_rejects_unsupported_or_malformed_build_profiles(self) -> None:
        requirements = forge_fixture_requirements(self.fixture_lock())
        with self.assertRaises(RuntimeError):
            require_supported_forge_build_profile("Experimental", requirements)
        with self.assertRaises(RuntimeError):
            require_supported_forge_build_profile(None, requirements)

        malformed_profiles = (
            "default",
            list(SUPPORTED_FORGE_BUILD_PROFILES[:-1]),
            [*SUPPORTED_FORGE_BUILD_PROFILES, "Experimental"],
        )
        for profiles in malformed_profiles:
            with self.subTest(profiles=profiles), self.assertRaises(RuntimeError):
                forge_fixture_requirements(self.fixture_lock(profiles))

    def test_dht_provider_evidence_requires_correlated_provider_query(self) -> None:
        require_dht_provider_evidence(valid_result("forge"), "forge", "listener-peer")
        invalid = valid_result("forge")
        invalid["network_proof"]["api_lookup"]["returned_provider_peer"] = "wrong-provider"
        with self.assertRaises(RuntimeError):
            require_dht_provider_evidence(invalid, "forge", "listener-peer")

    def test_hidden_dht_find_peer_evidence_requires_decoded_seed_reply(self) -> None:
        require_hidden_dht_find_peer_evidence(valid_hidden_find_peer_result(), "seed-peer", "target-peer")
        invalid = valid_hidden_find_peer_result()
        invalid["find_peer_proof"]["wire_confirmation"]["returned_target_peer"] = "wrong-target"
        with self.assertRaises(RuntimeError):
            require_hidden_dht_find_peer_evidence(invalid, "seed-peer", "target-peer")

    def test_local_topology_evidence_is_fail_closed(self) -> None:
        require_local_topology_evidence(
            {"status": "ok", "relay_echo": True, "relay_bytes": 1}, "relay_echo_topology"
        )
        valid_dcutr = {
            "status": "ok",
            "hole_punch_status": 3,
            "relay_echo": True,
            "source_hole_punch_successes": 1,
            "relay_bytes": 1,
        }
        require_local_topology_evidence(valid_dcutr, "dcutr_relay_topology")

        invalid = (
            {**valid_dcutr, "status": "failed"},
            {**valid_dcutr, "hole_punch_status": 4},
            {**valid_dcutr, "hole_punch_status": 3.0},
            {**valid_dcutr, "hole_punch_status": True},
            {**valid_dcutr, "relay_echo": False},
            {**valid_dcutr, "source_hole_punch_successes": 0},
            {**valid_dcutr, "source_hole_punch_successes": True},
            {**valid_dcutr, "source_hole_punch_successes": 1.0},
            {**valid_dcutr, "relay_bytes": 0},
            {**valid_dcutr, "relay_bytes": True},
            {**valid_dcutr, "relay_bytes": 1.0},
        )
        for result in invalid:
            with self.subTest(result=result), self.assertRaises(RuntimeError):
                require_local_topology_evidence(result, "dcutr_relay_topology")

        for result in (
            {"status": "ok", "relay_echo": True, "relay_bytes": True},
            {"status": "ok", "relay_echo": True, "relay_bytes": 1.0},
        ):
            with self.subTest(result=result), self.assertRaises(RuntimeError):
                require_local_topology_evidence(result, "relay_echo_topology")

        with self.assertRaises(RuntimeError):
            require_local_topology_evidence({"status": "ok"}, "unknown_topology")


if __name__ == "__main__":
    unittest.main()
