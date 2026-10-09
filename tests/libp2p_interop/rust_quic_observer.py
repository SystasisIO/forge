"""Isolated, explicitly instrumented Rust QUIC shutdown-observer copy.

This is not unmodified-donor traffic evidence. No build or original-tree write
is performed here; binary and command receipts belong to the coordinator.
"""

import hashlib
import io
import os
import shutil
import stat
import subprocess
import tarfile
from pathlib import Path

from provenance import graph_hash, sha256_file


PINNED_COMMIT = "22fb4c784fc55ad8b15d05fdc9f98d663107d4cb"
PINNED_GIT_TREE = "f403e12b239f2f1141ce528d5c5af50c88632ef3"
CARGO_FEATURE = "quic-cause-observer"
QUIC_SOURCE = "transports/quic/src/lib.rs"
FILE_LIMIT = 4096
FILE_BYTE_LIMIT = 16 * 1024 * 1024
TREE_BYTE_LIMIT = 128 * 1024 * 1024

_DECLARATION = (
    b"/// Error on an established [`Connection`].\n"
    b"#[derive(Debug, thiserror::Error)]\n"
    b"#[error(transparent)]\n"
    b"pub struct ConnectionError(quinn::ConnectionError);\n"
)
SOURCE_IMPLEMENTATION = (
    b"/// Error on an established [`Connection`].\n"
    b"#[derive(Debug)]\n"
    b"pub struct ConnectionError(quinn::ConnectionError);\n"
    b"\nimpl std::fmt::Display for ConnectionError {\n"
    b"    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {\n"
    b"        std::fmt::Display::fmt(&self.0, formatter)\n"
    b"    }\n"
    b"}\n"
    b"\nimpl std::error::Error for ConnectionError {\n"
    b"    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {\n"
    b"        Some(&self.0)\n"
    b"    }\n"
    b"}\n"
)
# Only the implementation hunk of rust_quic_source_proposal.patch is installed.
# Its donor-side unit module is deliberately not part of this copy recipe.
SOURCE_PATCH = (
    b"--- a/transports/quic/src/lib.rs\n"
    b"+++ b/transports/quic/src/lib.rs\n"
    b"@@ -111,4 +111,15 @@\n"
    b" /// Error on an established [`Connection`].\n"
    b"-#[derive(Debug, thiserror::Error)]\n"
    b"-#[error(transparent)]\n"
    b"+#[derive(Debug)]\n"
    b" pub struct ConnectionError(quinn::ConnectionError);\n"
    + b"".join(b"+" + line for line in SOURCE_IMPLEMENTATION.splitlines(keepends=True)[3:])
)


def _safe_path(path: Path) -> Path:
    path = path.absolute()
    if ".." in path.parts:
        raise ValueError("observer paths must not contain parent traversal")
    for component in (*reversed(path.parents), path):
        if component.is_symlink():
            raise ValueError(f"refusing symlink: {component}")
        if component.exists() and component != path and not component.is_dir():
            raise ValueError(f"non-directory path ancestor: {component}")
    return path


def _git_object(kind: bytes, value: bytes) -> bytes:
    # Git SHA-1 establishes equality with the pinned donor tree; receipts also
    # retain SHA-256 for every file and the byte-exact full source graph.
    return hashlib.sha1(kind + b" " + str(len(value)).encode("ascii") + b"\0" + value).digest()


def _snapshot(root: Path) -> tuple[dict, bytes]:
    root = _safe_path(root)
    if not root.is_dir():
        raise ValueError(f"missing donor directory: {root}")
    files = {}
    contents = {}
    total = 0
    entry_count = 0

    def visit(directory: Path) -> bytes:
        nonlocal total, entry_count
        if len(directory.relative_to(root).parts) > 64:
            raise ValueError("donor directory depth bound exceeded")
        entries = []
        for path in directory.iterdir():
            entry_count += 1
            if entry_count > FILE_LIMIT:
                raise ValueError("donor entry bound exceeded")
            metadata = path.lstat()
            if stat.S_ISLNK(metadata.st_mode):
                raise ValueError(f"refusing donor symlink: {path}")
            if stat.S_ISDIR(metadata.st_mode):
                entries.append((os.fsencode(path.name) + b"/", b"40000", visit(path)))
                continue
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1:
                raise ValueError(f"refusing non-independent regular file: {path}")
            relative = path.relative_to(root).as_posix()
            if len(files) >= FILE_LIMIT or metadata.st_size > FILE_BYTE_LIMIT:
                raise ValueError("donor file bound exceeded")
            descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
            with os.fdopen(descriptor, "rb") as value:
                body = value.read(FILE_BYTE_LIMIT + 1)
            total += len(body)
            if len(body) > FILE_BYTE_LIMIT or total > TREE_BYTE_LIMIT:
                raise ValueError("donor byte bound exceeded")
            after = path.lstat()
            if any(getattr(after, field) != getattr(metadata, field) for field in (
                "st_dev", "st_ino", "st_mode", "st_size", "st_mtime_ns", "st_ctime_ns",
            )):
                raise ValueError(f"donor changed while reading: {path}")
            mode = b"100755" if metadata.st_mode & stat.S_IXUSR else b"100644"
            blob = _git_object(b"blob", body)
            files[relative] = {"sha256": hashlib.sha256(body).hexdigest(),
                               "mode": mode.decode("ascii"), "git_blob": blob.hex()}
            contents[relative] = body
            entries.append((os.fsencode(path.name), mode, blob))
        if not entries:
            raise ValueError(f"empty directory is not in the pinned Git tree: {directory}")
        return _git_object(b"tree", b"".join(
            mode + b" " + name.rstrip(b"/") + b"\0" + object_id
            for name, mode, object_id in sorted(entries)
        ))

    tree = visit(root).hex()
    if QUIC_SOURCE not in files:
        raise ValueError("missing QUIC error source")
    graph = hashlib.sha256()
    for relative in sorted(files):
        graph.update(relative.encode("utf-8"))
        graph.update(b"\0")
        graph.update(contents[relative])
        graph.update(b"\0")
    return {"git_tree": tree, "tree_sha256": graph.hexdigest(),
            "files": {relative: files[relative] for relative in sorted(files)}}, contents[QUIC_SOURCE]


def original_provenance(fixture_deps: Path) -> dict:
    """Verify the complete export against the fixed original donor Git tree."""
    snapshot, source = _snapshot(_safe_path(fixture_deps) / "rust-libp2p")
    if snapshot["git_tree"] != PINNED_GIT_TREE:
        raise ValueError("original Rust export does not match the pinned Git tree")
    if source.count(_DECLARATION) != 1 or not source.endswith(_DECLARATION):
        raise ValueError("unexpected or already patched QUIC source")
    if (b"impl ConnectionError" in source
            or b"impl std::error::Error for ConnectionError" in source):
        raise ValueError("QUIC source already has a ConnectionError implementation")
    return snapshot


def verify_original_export(fixture_deps: Path, donor_checkout: Path) -> None:
    """Compare the original export with one immutable pinned Git archive."""
    original = original_provenance(fixture_deps)
    donor_checkout = _safe_path(donor_checkout)
    if not donor_checkout.is_dir():
        raise ValueError("missing original donor checkout")
    archive = subprocess.run(
        ["git", "--no-replace-objects", "-C", str(donor_checkout),
         "archive", "--format=tar", PINNED_COMMIT],
        check=True, stdout=subprocess.PIPE,
    ).stdout
    if len(archive) > TREE_BYTE_LIMIT + FILE_LIMIT * 2048:
        raise ValueError("pinned archive byte bound exceeded")
    files = {}
    names = set()
    total = 0
    with tarfile.open(fileobj=io.BytesIO(archive), mode="r:") as values:
        for member in values:
            relative = Path(member.name)
            if (not member.name or relative.is_absolute() or ".." in relative.parts
                    or relative.as_posix() != member.name.rstrip("/")):
                raise ValueError("unsafe pinned archive path")
            name = relative.as_posix()
            if name in names or len(names) >= FILE_LIMIT:
                raise ValueError("duplicate or excessive pinned archive membership")
            names.add(name)
            if member.isdir():
                continue
            if not member.isfile() or member.size > FILE_BYTE_LIMIT:
                raise ValueError("non-regular or excessive pinned archive file")
            value = values.extractfile(member)
            if value is None:
                raise ValueError("missing pinned archive content")
            with value:
                body = value.read(FILE_BYTE_LIMIT + 1)
            total += len(body)
            if len(body) != member.size or total > TREE_BYTE_LIMIT:
                raise ValueError("pinned archive content bound mismatch")
            files[name] = {"sha256": hashlib.sha256(body).hexdigest(),
                           "mode": "100755" if member.mode & stat.S_IXUSR else "100644",
                           "git_blob": _git_object(b"blob", body).hex()}
    if files != original["files"]:
        raise ValueError("original Rust export differs from the pinned Git archive")
    if original_provenance(fixture_deps) != original:
        raise ValueError("original export changed during archive verification")


def fixture_source_provenance(rust_fixture: Path, relative_files: list[str]) -> dict:
    """Hash the coordinator's exact registered fixture-source set, not target/.

    The coordinator independently checks membership and compares this result
    for its original fixture and actual observer fixture copy.
    """
    root = _safe_path(rust_fixture)
    if (type(relative_files) is not list or not relative_files
            or len(relative_files) > FILE_LIMIT
            or any(type(name) is not str for name in relative_files)
            or len(set(relative_files)) != len(relative_files)):
        raise ValueError("invalid fixture source membership")
    files = {}
    total = 0
    for name in sorted(relative_files):
        relative = Path(name)
        if (relative.is_absolute() or ".." in relative.parts
                or relative.as_posix() != name or not relative.parts
                or relative.parts[0] == "target"):
            raise ValueError("unsafe fixture source path")
        path = _safe_path(root / relative)
        metadata = path.lstat()
        total += metadata.st_size
        if (not stat.S_ISREG(metadata.st_mode) or metadata.st_nlink != 1
                or metadata.st_size > FILE_BYTE_LIMIT or total > TREE_BYTE_LIMIT):
            raise ValueError("invalid or excessive fixture source")
        files[name] = sha256_file(path)
    graph = graph_hash(root, sorted(files))
    if any(sha256_file(_safe_path(root / name)) != value for name, value in files.items()):
        raise ValueError("fixture sources changed while hashing the source copy")
    return {"files": files, "tree_sha256": graph}


def _roots(fixture_deps: Path, observer_root: Path) -> tuple[Path, Path]:
    fixture_deps, observer_root = _safe_path(fixture_deps), _safe_path(observer_root)
    if (observer_root.is_relative_to(fixture_deps)
            or fixture_deps.is_relative_to(observer_root)):
        raise ValueError("observer copy must be isolated from the original export")
    return fixture_deps, observer_root


def _receipt(fixture_deps: Path, observer_root: Path, original: dict, observed: dict) -> dict:
    return {
        "schema": "forge.rust-quic-cause-observer-copy.v1",
        "donor": "rust-libp2p", "commit": PINNED_COMMIT,
        "pinned_git_tree": PINNED_GIT_TREE, "cargo_feature": CARGO_FEATURE,
        "proof_scope": "instrumented_shutdown_only",
        "original_fixture_deps": str(fixture_deps), "observer_root": str(observer_root),
        "copied_donor": "fixture-deps/rust-libp2p",
        "original": original, "observed": observed,
        "patch": {"path": QUIC_SOURCE, "unified_diff": SOURCE_PATCH.decode("ascii"),
                  "sha256": hashlib.sha256(SOURCE_PATCH).hexdigest(),
                  "source_before_sha256": original["files"][QUIC_SOURCE]["sha256"],
                  "source_after_sha256": observed["files"][QUIC_SOURCE]["sha256"]},
    }


def prepare_observer_copy(fixture_deps: Path, observer_root: Path) -> dict:
    """Create a fresh full Rust donor copy with the exact standard-source fix."""
    fixture_deps, observer_root = _roots(fixture_deps, observer_root)
    original = original_provenance(fixture_deps)
    dependencies = observer_root / "fixture-deps"
    if dependencies.exists():
        raise ValueError("observer fixture-deps already exists; refusing reuse or overwrite")
    if observer_root.exists() and (not observer_root.is_dir() or any(observer_root.iterdir())):
        raise ValueError("observer root must be a fresh empty directory")
    observer_root.mkdir(parents=True, exist_ok=True)
    dependencies.mkdir()
    source = fixture_deps / "rust-libp2p"
    destination = dependencies / "rust-libp2p"
    # Copy symlinks as links, never follow them if an input changes during copy;
    # the independent snapshot below rejects any such raced link.
    shutil.copytree(source, destination, symlinks=True)
    copied, before = _snapshot(destination)
    if copied != original:
        raise ValueError("Rust export changed while copying")
    target = destination / QUIC_SOURCE
    temporary = target.with_name("lib.rs.quic-cause-observer")
    with temporary.open("xb") as value:
        value.write(before.removesuffix(_DECLARATION) + SOURCE_IMPLEMENTATION)
    temporary.chmod(target.stat().st_mode & 0o777)
    temporary.replace(target)
    observed, _ = _snapshot(destination)
    receipt = _receipt(fixture_deps, observer_root, original, observed)
    verify_observer_copy(receipt, fixture_deps, observer_root)
    return receipt


def verify_observer_copy(receipt: dict, fixture_deps: Path, observer_root: Path) -> None:
    """Recheck both trees and the exact one-file delta; raise ValueError on mismatch.

    Binary/command receipts are checked separately by the coordinator and must
    be stripped before this call. Every source-proof key must match exactly.
    """
    fixture_deps, observer_root = _roots(fixture_deps, observer_root)
    original = original_provenance(fixture_deps)
    dependencies = _safe_path(observer_root / "fixture-deps")
    if not dependencies.is_dir() or {path.name for path in dependencies.iterdir()} != {"rust-libp2p"}:
        raise ValueError("observer fixture-deps must contain only rust-libp2p")
    observed, source = _snapshot(dependencies / "rust-libp2p")
    original_source = (fixture_deps / "rust-libp2p" / QUIC_SOURCE).read_bytes()
    if source != original_source.removesuffix(_DECLARATION) + SOURCE_IMPLEMENTATION:
        raise ValueError("observer QUIC source is not the exact standard-source patch")
    if observed["files"].keys() != original["files"].keys():
        raise ValueError("observer source file membership changed")
    for relative, facts in original["files"].items():
        actual = observed["files"][relative]
        if ((relative != QUIC_SOURCE and actual != facts)
                or actual["mode"] != facts["mode"]):
            raise ValueError(f"out-of-recipe Rust source change: {relative}")
    if original_provenance(fixture_deps) != original:
        raise ValueError("original export changed during observer verification")
    expected = _receipt(fixture_deps, observer_root, original, observed)
    if type(receipt) is not dict or set(receipt) != set(expected):
        raise ValueError("invalid observer copy receipt fields")
    if any(receipt[key] != value for key, value in expected.items()):
        raise ValueError("observer copy receipt does not match the exact source recipe")
