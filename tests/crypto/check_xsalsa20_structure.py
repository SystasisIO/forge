#!/usr/bin/env python3

import argparse
import hashlib
import platform
import re
import shutil
import subprocess
from pathlib import Path


ARCHIVE_SHA256 = "adbdd8f16149e81ac6078a03aca6fc03b592b89ef7b5ed83841c086191be3349"
TREE_MANIFEST_SHA256 = "b7a7f9f72927a2bcd47ca29822c7c35902dbe4c128bfefd73eb67028d69338bc"
UPSTREAM_FILE_COUNT = 678
TREE_ENTRY = re.compile(r"^([0-9a-f]{64})  (.+)$")


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_tree_manifest(path: Path, errors: list[str]) -> dict[str, str]:
    if not path.is_file():
        errors.append("missing complete libsodium tree manifest: vendor/libsodium/TREE.SHA256")
        return {}
    if sha256(path) != TREE_MANIFEST_SHA256:
        errors.append("libsodium TREE.SHA256 does not match the pinned complete-tree manifest hash")

    expected: dict[str, str] = {}
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        match = TREE_ENTRY.fullmatch(line)
        if match is None:
            errors.append(f"invalid libsodium tree manifest entry at line {line_number}")
            continue
        digest, relative = match.groups()
        relative_path = Path(relative)
        if relative_path.is_absolute() or ".." in relative_path.parts or relative in expected:
            errors.append(f"invalid or duplicate libsodium tree manifest path at line {line_number}: {relative}")
            continue
        expected[relative] = digest

    if len(expected) != UPSTREAM_FILE_COUNT:
        errors.append(f"libsodium tree manifest has {len(expected)} entries; expected {UPSTREAM_FILE_COUNT}")
    return expected


def verify_complete_tree(upstream: Path, tree_manifest: Path, errors: list[str]) -> None:
    expected = read_tree_manifest(tree_manifest, errors)
    if not upstream.is_dir():
        errors.append("missing vendored libsodium upstream tree")
        return

    actual = {
        path.relative_to(upstream).as_posix(): sha256(path)
        for path in upstream.rglob("*")
        if path.is_file()
    }
    missing = sorted(set(expected) - set(actual))
    extra = sorted(set(actual) - set(expected))
    changed = sorted(relative for relative in set(expected) & set(actual) if expected[relative] != actual[relative])
    if missing:
        errors.append("vendored libsodium tree is missing files: " + ", ".join(missing))
    if extra:
        errors.append("vendored libsodium tree has unexpected files: " + ", ".join(extra))
    if changed:
        errors.append("vendored libsodium tree has changed files: " + ", ".join(changed))


def normalized_symbol(symbol: str) -> str:
    if platform.system() == "Darwin" and symbol.startswith("_"):
        return symbol[1:]
    return symbol


def verify_archive_symbols(archive: Path, errors: list[str]) -> None:
    if not archive.is_file():
        errors.append(f"missing XSalsa20 archive for symbol inspection: {archive}")
        return
    nm = shutil.which("nm")
    if nm is None:
        errors.append("nm is required to inspect XSalsa20 archive symbols")
        return

    result = subprocess.run([nm, "-g", "-P", str(archive)], capture_output=True, text=True, check=False)
    if result.returncode != 0:
        errors.append(f"nm -g -P failed for XSalsa20 archive: {result.stderr.strip()}")
        return

    forbidden = []
    for line in result.stdout.splitlines():
        fields = line.split()
        if not fields or fields[0].endswith(":"):
            continue
        symbol = normalized_symbol(fields[0])
        if symbol.startswith(("crypto_core_", "crypto_stream_")) or symbol in {"randombytes_buf", "sodium_memzero"}:
            forbidden.append(symbol)
    if forbidden:
        errors.append("XSalsa20 archive exposes unprefixed libsodium symbols: " + ", ".join(sorted(set(forbidden))))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--archive", required=True, type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    errors: list[str] = []

    upstream = source / "vendor" / "libsodium" / "upstream"
    manifest_path = source / "vendor" / "libsodium" / "MANIFEST.md"
    manifest = manifest_path.read_text(encoding="utf-8") if manifest_path.is_file() else ""
    required_manifest_terms = (ARCHIVE_SHA256, TREE_MANIFEST_SHA256, "678-file full manifest", "forge_xsalsa20_private_")
    for term in required_manifest_terms:
        if term not in manifest:
            errors.append(f"libsodium manifest omits required provenance or prefix: {term}")
    verify_complete_tree(upstream, source / "vendor" / "libsodium" / "TREE.SHA256", errors)

    module = (source / "libraries" / "crypto" / "symmetric" / "include" / "forge" / "crypto" / "symmetric" / "xsalsa20.cppm").read_text()
    forbidden_public_terms = ("sodium", "crypto_stream_", "randombytes", "libp2p", "pnet")
    for term in forbidden_public_terms:
        if term in module:
            errors.append(f"public XSalsa20 module leaks private backend or P2P term: {term}")

    salsa_module = (source / "libraries" / "crypto" / "symmetric" / "include" / "forge" / "crypto" / "symmetric" / "salsa20.cppm").read_text()
    for term in forbidden_public_terms:
        if term in salsa_module:
            errors.append(f"public Salsa20 module leaks private backend or P2P term: {term}")

    library = source / "libraries" / "crypto" / "symmetric"
    required_pairs = (
        (library / "include" / "forge" / "crypto" / "symmetric" / "xsalsa20.cppm", library / "xsalsa20.cpp"),
        (library / "include" / "forge" / "crypto" / "symmetric" / "salsa20.cppm", library / "salsa20.cpp"),
        (library / "details" / "counter_state.hxx", library / "counter_state.cpp"),
        (library / "details" / "stream_impl.hxx", library / "stream_impl.cpp"),
    )
    for header, implementation in required_pairs:
        if not header.is_file() or not implementation.is_file():
            errors.append(f"missing create-library pair: {header.name} / {implementation.name}")

    cmake = (library / "CMakeLists.txt").read_text()
    for source_name in (
        "counter_state.cpp",
        "stream_impl.cpp",
        "xsalsa20.cpp",
        "salsa20.cpp",
        "$<TARGET_OBJECTS:forge_crypto_symmetric_xsalsa20_vendor>",
    ):
        if source_name not in cmake:
            errors.append(f"symmetric CMake source list omits {source_name}")

    root_cmake = (source / "CMakeLists.txt").read_text()
    required_vendor_terms = (
        "forge_crypto_symmetric_xsalsa20_vendor OBJECT",
        "C_VISIBILITY_PRESET hidden",
        "CONFIGURED=1",
        "SODIUM_STATIC=1",
        "crypto_core_salsa20=forge_xsalsa20_private_crypto_core_salsa20",
        "crypto_stream_xsalsa20=forge_xsalsa20_private_crypto_stream_xsalsa20",
        "randombytes_buf=forge_xsalsa20_private_randombytes_buf",
        "sodium_memzero=forge_xsalsa20_private_sodium_memzero",
        "forge_apply_vendored_implementation_policy(${_forge_vendor_target})",
    )
    for term in required_vendor_terms:
        if term not in root_cmake:
            errors.append(f"XSalsa20 private vendor target omits required policy: {term}")

    package_consumer = (source / "tests" / "package_crypto_symmetric_component" / "main.cpp").read_text()
    required_consumer_terms = (
        "forge.crypto.symmetric.xsalsa20",
        'extern "C" void sodium_memzero(',
        'extern "C" void randombytes_buf(',
        'extern "C" int crypto_stream_xsalsa20(',
        "forge.crypto.symmetric.salsa20",
        'extern "C" int crypto_stream_salsa20(',
    )
    for term in required_consumer_terms:
        if term not in package_consumer:
            errors.append(f"crypto_symmetric package consumer omits coexistence probe: {term}")

    verify_archive_symbols(args.archive.resolve(), errors)
    if errors:
        raise SystemExit("\n".join(errors))
    print("XSalsa20 full-tree provenance, private API boundary and archive symbol isolation are intact")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
