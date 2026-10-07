#!/usr/bin/env python3
import io
import json
import os
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
    main as inventory_main,
    private_mdns_attribution_errors,
    private_profile_source_errors,
    registered_runner_acceptance_pairs,
    registered_runner_pair_errors,
)
from runner import (
    GO_EXECUTION_POLICY,
    SUPPORTED_FORGE_BUILD_PROFILES,
    forge_fixture_requirements,
    go_execution_environment,
    prepare_go_fixture,
    prepare_rust_fixture,
    require_dht_provider_evidence,
    require_hidden_dht_find_peer_evidence,
    require_local_topology_evidence,
    require_supported_forge_build_profile,
    require_toolchain,
    run_dial,
)
from test_provider_evidence import valid_hidden_find_peer_result, valid_result
from promote_stage6_acceptance import (
    CANONICAL_ACCEPTANCE_MANIFEST,
    PROMOTION_DIRECTORY_PREFIX,
    PROMOTION_SCOPES,
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
    def test_go_environment_clamps_graph_overrides_without_mutating_parent(self) -> None:
        inherited = {
            "PATH": "/tools", "OTHER": "preserved",
            "GOFLAGS": "-modfile=/external/alternate.mod -overlay=/external/overlay.json",
            "GOENV": "/external/go-env", "GOWORK": "/external/go.work",
            "GOTOOLCHAIN": "auto", "GOPROXY": "https://untrusted.invalid", "GOSUMDB": "sum.example",
        }
        before = dict(inherited)
        ambient = dict(os.environ)
        effective = go_execution_environment(inherited)
        self.assertEqual({key: effective[key] for key in GO_EXECUTION_POLICY}, {
            "GOTOOLCHAIN": "local", "GOPROXY": "off", "GOSUMDB": "off",
            "GOFLAGS": "", "GOENV": "off", "GOWORK": "off",
        })
        self.assertEqual(effective["PATH"], "/tools")
        self.assertEqual(effective["OTHER"], "preserved")
        self.assertEqual(inherited, before)
        self.assertEqual(dict(os.environ), ambient)

    def test_prepare_go_fixture_uses_effective_policy_for_verify_tests_and_build(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source/go_fixture"
            source.mkdir(parents=True)
            canonical = "module fixture\n\ngo 1.26.3\n"
            (source / "go.mod").write_text(canonical)
            alternate = root / "alternate.mod"
            alternate.write_text("module different-fixture\n\ngo 1.26.3\n")
            ambient = dict(os.environ)
            for label, overrides in (
                ("control", {}),
                ("alternate", {"GOFLAGS": f"-modfile={alternate}",
                               "GOENV": str(root / "go-env"), "GOWORK": str(root / "go.work"),
                               "GOPROXY": "https://untrusted.invalid", "GOSUMDB": "sum.example"}),
            ):
                with self.subTest(environment=label):
                    inherited = {"PATH": "/tools", "OTHER": "preserved", **overrides}
                    before = dict(inherited)
                    calls = []
                    def fake_run(command, cwd=None, env=None):
                        calls.append((command, cwd, dict(env)))
                    with patch("runner.run", side_effect=fake_run):
                        binary, records = prepare_go_fixture(root / "source", root / label, "/tools/go", inherited)
                    expected = [
                        ["/tools/go", "mod", "verify"],
                        ["/tools/go", "test", "-mod=readonly", "-count=1", "-timeout=60s", "."],
                        ["/tools/go", "build", "-mod=readonly", "-trimpath", "-o", str(binary), "."],
                    ]
                    self.assertEqual([call[0] for call in calls], expected)
                    self.assertEqual([record["command"] for record in records], expected)
                    for record, (_, cwd, effective) in zip(records, calls):
                        self.assertEqual(cwd, binary.parent)
                        self.assertEqual(record["cwd"], str(cwd))
                        self.assertEqual(record["environment"], GO_EXECUTION_POLICY)
                        self.assertEqual(record["environment"], {key: effective[key] for key in GO_EXECUTION_POLICY})
                        self.assertEqual(effective["OTHER"], "preserved")
                    self.assertEqual((binary.parent / "go.mod").read_text(), canonical)
                    self.assertEqual(inherited, before)
            self.assertEqual(dict(os.environ), ambient)

    def test_toolchain_version_check_uses_the_same_clamped_go_environment(self) -> None:
        inherited = {"GOFLAGS": "-modfile=/external/alternate.mod", "GOENV": "/external/go-env",
                     "GOWORK": "/external/go.work", "PATH": "/tools"}
        ambient = dict(os.environ)
        calls = {}
        def tool(name, command, pattern, version, cwd=None, env=None):
            calls[name] = dict(env)
            return {"path": f"/tools/{name}", "version": version, "version_output": "synthetic unit tool"}
        lock = {"toolchains": {"go": {"version": "1.26.3"},
                              "rust": {"rustc_version": "1.95.0", "cargo_version": "1.95.0"}}}
        with patch("runner.go_execution_environment", return_value=go_execution_environment(inherited)) as policy, \
                patch("runner.tool_identity", side_effect=tool):
            _, go_environment, _ = require_toolchain(lock, Path("/unit/source"))
        policy.assert_called_once_with()
        self.assertEqual(calls["go"], go_environment)
        self.assertEqual({key: go_environment[key] for key in GO_EXECUTION_POLICY}, GO_EXECUTION_POLICY)
        self.assertEqual(dict(os.environ), ambient)

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

    def test_promotion_scopes_match_all_suites_without_an_autonat_fallback(self) -> None:
        from check_stage6_acceptance import ACCEPTANCE_SUITES

        self.assertEqual(set(PROMOTION_SCOPES), set(ACCEPTANCE_SUITES))
        expected = {"private-profile": "PSK protocol30", "inline-muxer": "inline-muxer16",
                    "path": "DCUtR12", "coordinated": "TCP reuse8"}
        for suite, label in expected.items():
            with self.subTest(suite=suite):
                self.assertIn(label, PROMOTION_SCOPES[suite])
                self.assertNotIn("AutoNAT", PROMOTION_SCOPES[suite])
                self.assertIn("not full Stage 6 or production support", PROMOTION_SCOPES[suite])


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


class InventoryPolicyTest(unittest.TestCase):
    source_dir = Path(__file__).resolve().parent
    source_root = source_dir.parents[1]

    def manifest(self, name: str) -> dict:
        return json.loads((self.source_dir / name).read_text())

    def runner_fixture(self, include_pr9: bool = True, pr9_map: Optional[dict] = None,
                       include_private: bool = True, include_path: bool = True) -> str:
        from runner import (
            AUTONAT_ACCEPTANCE_SCENARIOS,
            AUTORELAY_ACCEPTANCE_SCENARIOS,
            CURRENT_ACCEPTANCE_SCENARIOS,
            LIVE_SCENARIO_PROFILES,
            MDNS_ACCEPTANCE_SCENARIOS,
        )

        maps = {
            "LIVE_SCENARIO_PROFILES": LIVE_SCENARIO_PROFILES,
            "CURRENT_ACCEPTANCE_SCENARIOS": CURRENT_ACCEPTANCE_SCENARIOS,
            "AUTONAT_ACCEPTANCE_SCENARIOS": AUTONAT_ACCEPTANCE_SCENARIOS,
            "MDNS_ACCEPTANCE_SCENARIOS": MDNS_ACCEPTANCE_SCENARIOS,
        }
        if include_pr9:
            maps["AUTORELAY_ACCEPTANCE_SCENARIOS"] = (
                AUTORELAY_ACCEPTANCE_SCENARIOS if pr9_map is None else pr9_map
            )
        source = "\n".join(f"{name} = {value!r}" for name, value in maps.items())
        if include_private:
            source += '''
from private_profile_cases import case_specs as private_profile_specs, run_case as private_profile_case
from private_profile_evidence import PRIVATE_CONTRACTS, INLINE_CONTRACTS, validate_private_profile

@owned_case
def run_private_profile_case(spec, binaries, root):
    record = private_profile_case(spec, binaries, root)
    errors = validate_private_profile(record["result"], record, record["listener_result"])
    if errors:
        raise RuntimeError(errors)
    return record
'''
        if include_path:
            source += '''
from path_cases import run_suite as run_path_suite
from coordinated_cases import run_suite as run_coordinated_suite
'''
        source += '''
from pubsub_cases import case_specs as pubsub_specs, run_case as run_pubsub_case
'''
        source += "\ndef main():\n    pass\n"
        if include_private:
            source += '''
    for suite, names in (("private-profile", PRIVATE_CONTRACTS), ("inline-muxer", INLINE_CONTRACTS)):
        for spec in private_profile_specs(suite):
            run_private_profile_case(spec, binaries, root)
'''
        if include_path:
            source += '''
    for artifact in run_path_suite(binaries, root, command_attempt=command_attempt):
        artifacts.append(artifact)
    for artifact in run_coordinated_suite(binaries, root, pnet_key_file=pnet_key_file,
            pnet_fingerprint=pnet_fingerprint, command_attempt=command_attempt):
        artifacts.append(artifact)
'''
        source += '''
    for spec in pubsub_specs():
        artifact = run_pubsub_case(spec, binaries, root, key=pnet_key_file,
            fingerprint=pnet_fingerprint, command_attempt=command_attempt)
        artifacts.append(artifact)
'''
        return source

    def check(self, capabilities: Optional[dict] = None, inventory: Optional[dict] = None,
              donor_cases: Optional[dict] = None, runner_source: Optional[str] = None) -> tuple[int, str, str]:
        classified = self.manifest("p2p_feature_inventory.json")
        surfaces = {
            owner["path"]: classified["public_surface_snapshots"][key]
            for key, owner in classified["owners"].items()
            if owner["kind"] in {"library", "plugin"}
        }

        def surface_snapshot(root: Path, owner: dict) -> tuple:
            snapshot = surfaces[owner["path"]]
            return snapshot["modules"], snapshot["headers"], snapshot["sha256"], []

        replacements = {}
        if capabilities is not None:
            replacements[self.source_dir / "p2p_donor_capabilities.json"] = json.dumps(capabilities)
        if inventory is not None:
            replacements[self.source_dir / "p2p_feature_inventory.json"] = json.dumps(inventory)
        if donor_cases is not None:
            replacements[self.source_dir / "donor_cases.json"] = json.dumps(donor_cases)
        if runner_source is not None:
            replacements[self.source_dir / "runner.py"] = runner_source
        original_read_text = Path.read_text

        def read_text(path: Path, *args, **kwargs) -> str:
            if path in replacements:
                return replacements[path]
            return original_read_text(path, *args, **kwargs)

        argv = [
            "check_p2p_feature_inventory.py",
            str(self.source_root),
            str(self.source_dir / "p2p_feature_inventory.json"),
            str(self.source_dir / "p2p_donor_capabilities.json"),
            str(self.source_dir / "donor_cases.json"),
            str(self.source_root / "donors"),
        ]
        stdout, stderr = io.StringIO(), io.StringIO()
        # Pin classified declarations for policy tests while production source may evolve.
        # The standalone checker verifies actual source hashes and donor pins without mocks.
        with (
            patch.object(sys, "argv", argv),
            patch.object(Path, "read_text", read_text),
            patch("check_p2p_feature_inventory.donor_checkout_head_errors", return_value=[]),
            patch("check_p2p_feature_inventory.donor_case_source_errors", return_value=[]),
            patch("check_p2p_feature_inventory.donor_source_object_errors", return_value=[]),
            patch("check_p2p_feature_inventory.public_surface_snapshot", side_effect=surface_snapshot),
            patch.object(sys, "stdout", stdout),
            patch.object(sys, "stderr", stderr),
        ):
            status = inventory_main()
        return status, stdout.getvalue(), stderr.getvalue()

    def test_current_inventory_is_valid_but_not_live_execution_evidence(self) -> None:
        states = {
            feature["id"]: feature["state"]
            for feature in self.manifest("p2p_feature_inventory.json")["features"]
        }
        status, stdout, stderr = self.check()
        self.assertEqual((status, stderr), (0, ""))
        self.assertIn("source-only; no live interop execution verdict", stdout)
        capabilities = self.manifest("p2p_donor_capabilities.json")
        self.assertEqual(
            [entry["ordinal"] for entry in capabilities["stage_6_pr_registry"]],
            [0, 1, 2, 3, 4, 5, 6, 7, 9, 10, 11, 12],
        )
        self.assertEqual(
            {feature["id"]: feature["state"]
             for feature in self.manifest("p2p_feature_inventory.json")["features"]},
            states,
        )

    def test_private_inline_registration_matches_actual_case_ids_without_a_live_verdict(self) -> None:
        from private_profile_acceptance import SCENARIOS
        from private_profile_cases import case_specs

        registry = self.manifest("p2p_donor_capabilities.json")["interop_acceptance_registry"]["capabilities"]
        actual = {(case.runner_id, case.contract) for suite in ("private-profile", "inline-muxer")
                  for case in case_specs(suite)}
        self.assertEqual(len(SCENARIOS), 16)
        registered = set()
        for name, owner in SCENARIOS.items():
            scenario = next(value for value in registry[owner]["scenarios"] if value["id"] == name)
            self.assertEqual(scenario["registration"], "registered")
            self.assertEqual(scenario["source_case_id"], name)
            self.assertEqual(scenario["expected_status"], "limited" if name.startswith("inline_muxer_rust_") else "passed")
            registered.add((scenario["runner_scenario_id"], name))
        self.assertEqual(registered, actual)
        states = {value["id"]: value["state"] for value in self.manifest("p2p_feature_inventory.json")["features"]}
        self.assertEqual(states["state.hole_punch_attempt"], "partial")
        self.assertEqual(states["transport.tcp_yamux"], "unverified")

    def test_path_registration_is_checked_through_actual_inventory_main(self) -> None:
        status, _, stderr = self.check(runner_source=self.runner_fixture(include_path=False))
        self.assertEqual(status, 1)
        self.assertIn("current scenario is not registered by runner.py", stderr)
        capabilities = self.manifest("p2p_donor_capabilities.json")
        scenario = capabilities["interop_acceptance_registry"]["capabilities"]["connections.coordinated_dial_port_reuse"]["scenarios"][-1]
        scenario["requires_capabilities"] = []
        status, _, stderr = self.check(capabilities=capabilities)
        self.assertEqual(status, 1)
        self.assertIn("path registration must match its exact staged native case contract", stderr)

    def test_path_literal_map_cannot_replace_actual_suite_dispatch(self) -> None:
        from check_p2p_feature_inventory import PATH_REGISTRATION_SCENARIOS
        from runner import CURRENT_ACCEPTANCE_SCENARIOS

        counterfeit = {**CURRENT_ACCEPTANCE_SCENARIOS,
                       **{value[4]: (name,) for name, value in PATH_REGISTRATION_SCENARIOS.items()}}
        source = self.runner_fixture(include_path=False).replace(
            f"CURRENT_ACCEPTANCE_SCENARIOS = {CURRENT_ACCEPTANCE_SCENARIOS!r}",
            f"CURRENT_ACCEPTANCE_SCENARIOS = {counterfeit!r}")
        status, _, stderr = self.check(runner_source=source)
        self.assertEqual(status, 1)
        self.assertIn("current scenario is not registered by runner.py", stderr)

    def test_private_inline_source_contract_rejects_profile_and_case_drift(self) -> None:
        capabilities = self.manifest("p2p_donor_capabilities.json")
        owner = "transport.tcp_yamux"
        capability = next(value for value in capabilities["capabilities"] if value["id"] == owner)
        scenario = next(value for value in capabilities["interop_acceptance_registry"]["capabilities"][owner]["scenarios"]
                        if value["id"] == "tcp_yamux_private_pnet")
        self.assertEqual(private_profile_source_errors(self.source_root, owner, capability, scenario), [])
        for field, value in (
            ("source_case_id", "interop.live_private_network_pnet"), ("profile", "native"),
            ("transport_stack", ["tcp", "yamux"]), ("requires_capabilities", []),
            ("required_directions", ["forge_to_go", "go_to_forge"]), ("required_directions", None),
            ("required_directions", ["forge_to_go", {"role": "listener"}]),
            ("expected_status", "limited"), ("registration", "planned"),
        ):
            with self.subTest(field=field, value=value):
                self.assertTrue(private_profile_source_errors(
                    self.source_root, owner, capability, {**scenario, field: value}))
        self.assertTrue(private_profile_source_errors(self.source_root, "protocol.ping", capability, scenario))
        self.assertTrue(private_profile_source_errors(self.source_root, owner, {**capability, "donor_sources": []}, scenario))
        with patch.object(Path, "is_file", return_value=False):
            self.assertTrue(private_profile_source_errors(self.source_root, owner, capability, scenario))

    def test_private_inline_manifest_source_drift_is_rejected(self) -> None:
        capabilities = self.manifest("p2p_donor_capabilities.json")
        scenario = capabilities["interop_acceptance_registry"]["capabilities"]["discovery.rendezvous"]["scenarios"][-1]
        self.assertEqual(scenario["id"], "rendezvous_rust_private_tcp_yamux_pnet")
        scenario["source_case_id"] = "rendezvous_unregistered"
        status, _, stderr = self.check(capabilities=capabilities)
        self.assertEqual(status, 1)
        self.assertIn("private/inline registration must match its exact source case contract", stderr)

    def test_private_inline_literal_registration_cannot_replace_actual_runner_wiring(self) -> None:
        status, _, stderr = self.check(runner_source=self.runner_fixture(include_private=False))
        self.assertEqual(status, 1)
        self.assertIn("current scenario is not registered by runner.py", stderr)
        source = self.runner_fixture()
        for old, new in (
            ("record = private_profile_case(spec", "record = unrelated_adapter(spec"),
            ("errors = validate_private_profile(", "errors = unrelated_validator("),
            ("@owned_case", "@unowned_case"),
            ("for spec in private_profile_specs(suite):", "for spec in unrelated_specs(suite):"),
        ):
            with self.subTest(old=old):
                status, _, stderr = self.check(runner_source=source.replace(old, new))
                self.assertEqual(status, 1)
                self.assertIn("private profile runner", stderr)

    def test_removed_public_hole_punch_helper_is_owned_by_private_node_path_manager(self) -> None:
        inventory = self.manifest("p2p_feature_inventory.json")
        feature = next(value for value in inventory["features"] if value["id"] == "state.hole_punch_attempt")
        self.assertEqual(feature["owner"], "net.p2p.node")
        self.assertEqual(feature["public_components"], [])
        self.assertIn("libraries/net/p2p/path_manager.cpp", feature["evidence"]["source_paths"])
        self.assertIn("test_forge_p2p_path_management", feature["evidence"]["tests"])
        self.assertNotIn("state.hole_punch_attempt",
                         inventory["public_surface_snapshots"]["net.p2p.node"]["module_features"]["forge.net.p2p.hole_punch"])

    def test_upnp_cannot_return_to_the_initial_profile(self) -> None:
        capabilities = self.manifest("p2p_donor_capabilities.json")
        upnp = next(value for value in capabilities["capabilities"] if value["id"] == "nat.upnp_mapping")
        upnp.update(support_requirement="optional", default_activation="opt_in", decision="stage_6",
                    planned_branch="forge-p2p-nat-mapping-v1")
        status, _, stderr = self.check(capabilities=capabilities)
        self.assertEqual(status, 1)
        self.assertIn("nat.upnp_mapping: semantic classification differs", stderr)

    def test_future_upnp_cannot_keep_a_staged_branch(self) -> None:
        capabilities = self.manifest("p2p_donor_capabilities.json")
        upnp = next(value for value in capabilities["capabilities"] if value["id"] == "nat.upnp_mapping")
        upnp["planned_branch"] = "forge-p2p-nat-mapping-v1"
        status, _, stderr = self.check(capabilities=capabilities)
        self.assertEqual(status, 1)
        self.assertIn("nat.upnp_mapping: planned_branch is only valid for a staged decision", stderr)

    def test_registry_rejects_restored_pr8_and_extra_entries(self) -> None:
        for position in (8, 12):
            capabilities = self.manifest("p2p_donor_capabilities.json")
            capabilities["stage_6_pr_registry"].insert(position, {
                "ordinal": 8,
                "branch": "forge-p2p-nat-mapping-v1",
                "dependencies": [],
                "allowed_capability_owners": ["nat.upnp_mapping"],
            })
            with self.subTest(position=position):
                status, _, stderr = self.check(capabilities=capabilities)
                self.assertEqual(status, 1)
                self.assertIn("exact approved branch set and order", stderr)

    def test_registry_rejects_renumbering_and_noninteger_ordinals(self) -> None:
        for index, ordinal in ((8, 8), (1, True), (8, 9.0)):
            capabilities = self.manifest("p2p_donor_capabilities.json")
            capabilities["stage_6_pr_registry"][index]["ordinal"] = ordinal
            with self.subTest(index=index, ordinal=ordinal):
                status, _, stderr = self.check(capabilities=capabilities)
                self.assertEqual(status, 1)
                self.assertIn(f"entry {index} has invalid ordinal or branch", stderr)

    def test_autorelay_cannot_depend_on_deferred_upnp(self) -> None:
        capabilities = self.manifest("p2p_donor_capabilities.json")
        autorelay = capabilities["stage_6_pr_registry"][8]
        self.assertEqual(autorelay["branch"], "forge-p2p-autorelay-v1")
        autorelay["dependencies"].append("forge-p2p-nat-mapping-v1")
        status, _, stderr = self.check(capabilities=capabilities)
        self.assertEqual(status, 1)
        self.assertIn("forge-p2p-autorelay-v1 dependencies differ from baseline", stderr)

    def test_stale_plugin_hashes_are_rejected_not_silently_refreshed(self) -> None:
        for owner in ("node", "resolver", "pubsub", "diagnostics"):
            inventory = self.manifest("p2p_feature_inventory.json")
            key = f"plugin.p2p.{owner}"
            inventory["public_surface_snapshots"][key]["sha256"] = "0" * 64
            with self.subTest(owner=key):
                status, _, stderr = self.check(inventory=inventory)
                self.assertEqual(status, 1)
                self.assertIn(f"public surface {key}: declarations changed", stderr)

    def test_plugin_module_coverage_cannot_be_omitted(self) -> None:
        inventory = self.manifest("p2p_feature_inventory.json")
        inventory["public_surface_snapshots"]["plugin.p2p.node"]["module_features"].pop(
            "forge.plugins.net.p2p.node.api"
        )
        status, _, stderr = self.check(inventory=inventory)
        self.assertEqual(status, 1)
        self.assertIn("public surface plugin.p2p.node: every module needs exact feature classification", stderr)

    def test_pr9_native_profiles_match_the_canonical_suite(self) -> None:
        from autorelay_acceptance import ROLE_DIRECTIONS, SCENARIOS
        from stage6_evidence_contract import AUTORELAY_NATIVE_DIRECTIONS, AUTORELAY_NATIVE_PROFILES

        self.assertEqual(len(AUTORELAY_NATIVE_PROFILES), 6)
        self.assertEqual(AUTORELAY_NATIVE_PROFILES, SCENARIOS)
        self.assertEqual(AUTORELAY_NATIVE_DIRECTIONS, ROLE_DIRECTIONS)
        pairs = registered_runner_acceptance_pairs(self.source_dir / "runner.py")
        for name, (_, _, _, profile) in SCENARIOS.items():
            self.assertIn((f"{profile}/{name}", name), pairs)

    def test_pr9_directions_must_follow_the_observed_forge_role(self) -> None:
        from stage6_evidence_contract import AUTORELAY_NATIVE_DIRECTIONS, AUTORELAY_NATIVE_PROFILES

        for name, (owner, role, _, _) in AUTORELAY_NATIVE_PROFILES.items():
            for directions in (
                sorted(AUTORELAY_NATIVE_DIRECTIONS["service" if role == "lifecycle" else "lifecycle"]),
                ["forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"],
            ):
                capabilities = self.manifest("p2p_donor_capabilities.json")
                scenario = next(value for value in capabilities["interop_acceptance_registry"]["capabilities"][owner]["scenarios"]
                                if value["id"] == name)
                scenario["required_directions"] = directions
                with self.subTest(name=name, directions=directions):
                    status, _, stderr = self.check(capabilities=capabilities)
                    self.assertEqual(status, 1)
                    self.assertIn("PR9 native evidence profile must match its exact staged owner, role directions", stderr)

    def test_pr9_rejects_mismatched_native_evidence_profiles(self) -> None:
        from stage6_evidence_contract import AUTORELAY_NATIVE_PROFILES

        for name, (owner, _, transport, _) in AUTORELAY_NATIVE_PROFILES.items():
            mutations = {
                "profile": "private_network",
                "transport_stack": ["tcp", "yamux"] if transport == "quic" else ["quic"],
                "runner_scenario_id": f"quic_base/{name}",
                "source_case_id": "relayv2.reserve_connect_status",
                "requires_capabilities": ["security.private_network_psk"],
                "expected_status": "limited",
                "activation": "opt_in",
            }
            for field, replacement in mutations.items():
                capabilities = self.manifest("p2p_donor_capabilities.json")
                scenario = next(value for value in capabilities["interop_acceptance_registry"]["capabilities"][owner]["scenarios"]
                                if value["id"] == name)
                scenario[field] = replacement
                with self.subTest(name=name, field=field):
                    status, _, stderr = self.check(capabilities=capabilities)
                    self.assertEqual(status, 1)
                    self.assertIn("PR9 native evidence profile must match its exact", stderr)

    def test_pr9_registration_requires_all_six_profiles(self) -> None:
        from stage6_evidence_contract import AUTORELAY_NATIVE_PROFILES, evidence_contract_for

        for name, (owner, _, _, _) in AUTORELAY_NATIVE_PROFILES.items():
            capabilities = self.manifest("p2p_donor_capabilities.json")
            registry = capabilities["interop_acceptance_registry"]
            registry["capabilities"][owner]["scenarios"] = [
                value for value in registry["capabilities"][owner]["scenarios"] if value["id"] != name
            ]
            registry["evidence_contracts"].remove(evidence_contract_for(name))
            with self.subTest(name=name):
                status, _, stderr = self.check(capabilities=capabilities)
                self.assertEqual(status, 1)
                self.assertIn("acceptance scenario ids differ from Stage 6 baseline", stderr)
                self.assertIn("executable validator registry must match registered evidence contracts exactly", stderr)

    def test_pr9_registration_cannot_promote_support_decisions(self) -> None:
        for owner in ("relay.autorelay_lifecycle", "relay.circuit_v2_service"):
            capabilities = self.manifest("p2p_donor_capabilities.json")
            capability = next(value for value in capabilities["capabilities"] if value["id"] == owner)
            capability["decision"] = "current"
            capability.pop("planned_branch")
            with self.subTest(owner=owner):
                status, _, stderr = self.check(capabilities=capabilities)
                self.assertEqual(status, 1)
                self.assertIn("PR9 native evidence profile must match its exact staged owner", stderr)

    def test_pr9_host_local_exception_requires_exact_observable_contracts(self) -> None:
        capabilities = self.manifest("p2p_donor_capabilities.json")
        scenario = capabilities["interop_acceptance_registry"]["capabilities"]["relay.autorelay_lifecycle"]["scenarios"][0]
        scenario["source_case_id"] = "interop.live_ping_identify_relay"
        status, _, stderr = self.check(capabilities=capabilities)
        self.assertEqual(status, 1)
        self.assertIn("relay.autorelay_lifecycle: host-local orchestration cannot claim bilateral interop", stderr)

    def test_pr9_donor_cases_must_register_each_actual_native_pair(self) -> None:
        from stage6_evidence_contract import AUTORELAY_NATIVE_PROFILES, AUTORELAY_NATIVE_SOURCE_CASES

        for name, (_, role, _, _) in AUTORELAY_NATIVE_PROFILES.items():
            donors = self.manifest("donor_cases.json")
            case = next(value for value in donors["cases"] if value["id"] == AUTORELAY_NATIVE_SOURCE_CASES[role])
            case["forge_live_scenario"] = [value for value in case["forge_live_scenario"] if value["scenario"] != name]
            with self.subTest(name=name):
                status, _, stderr = self.check(donor_cases=donors)
                self.assertEqual(status, 1)
                self.assertIn("donor case does not register its runner scenario", stderr)

    def test_pr9_runner_registration_is_a_closed_six_pair_map(self) -> None:
        from stage6_evidence_contract import AUTORELAY_NATIVE_PROFILES

        mapping = {f"{value[3]}/{name}": (name,) for name, value in AUTORELAY_NATIVE_PROFILES.items()}
        for name, (_, _, _, profile) in AUTORELAY_NATIVE_PROFILES.items():
            reduced = dict(mapping)
            reduced.pop(f"{profile}/{name}")
            with self.subTest(name=name):
                status, _, stderr = self.check(runner_source=self.runner_fixture(pr9_map=reduced))
                self.assertEqual(status, 1)
                self.assertIn("AutoRelay registration must cover all 6 exact native role/transport scenarios", stderr)
        expanded = {**mapping, "quic_stage6/autorelay_unproved": ("autorelay_unproved",)}
        status, _, stderr = self.check(runner_source=self.runner_fixture(pr9_map=expanded))
        self.assertEqual(status, 1)
        self.assertIn("AutoRelay registration must cover all 6 exact native role/transport scenarios", stderr)

    def test_pr9_canonical_profile_drift_is_rejected(self) -> None:
        from stage6_evidence_contract import AUTORELAY_NATIVE_PROFILES

        drifted = dict(AUTORELAY_NATIVE_PROFILES)
        drifted["autorelay_lifecycle"] = ("relay.autorelay_lifecycle", "service", "quic", "quic_stage6")
        with patch("check_p2p_feature_inventory.AUTORELAY_SCENARIOS", drifted):
            status, _, stderr = self.check()
        self.assertEqual(status, 1)
        self.assertIn("PR9 native profiles differ from the canonical AutoRelay suite", stderr)

    def test_old_planned_pr9_synthetic_inventory_fixture_remains_valid(self) -> None:
        from stage6_evidence_contract import AUTORELAY_NATIVE_PROFILES, evidence_contract_for

        capabilities = self.manifest("p2p_donor_capabilities.json")
        lifecycle = next(value for value in capabilities["capabilities"] if value["id"] == "relay.autorelay_lifecycle")
        lifecycle["interop_applicability"] = "not_applicable"
        registry = capabilities["interop_acceptance_registry"]
        registry["capabilities"].pop("relay.autorelay_lifecycle")
        service = registry["capabilities"]["relay.circuit_v2_service"]["scenarios"][0]
        service["registration"] = "planned"
        service.pop("source_case_id")
        service["required_directions"] = ["forge_to_go", "go_to_forge", "forge_to_rust", "rust_to_forge"]
        registry["capabilities"]["relay.circuit_v2_service"]["scenarios"] = [service]
        registry["evidence_contracts"] = [
            contract for contract in registry["evidence_contracts"]
            if contract not in {evidence_contract_for(name) for name in AUTORELAY_NATIVE_PROFILES}
        ] + [evidence_contract_for("relay_v2_service")]
        status, stdout, stderr = self.check(capabilities=capabilities, runner_source=self.runner_fixture(include_pr9=False))
        self.assertEqual((status, stderr), (0, ""))
        self.assertIn("source-only; no live interop execution verdict", stdout)


if __name__ == "__main__":
    unittest.main()
