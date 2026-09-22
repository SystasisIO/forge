#!/usr/bin/env python3
"""Run and validate the one canonical Stage 6 promotion invocation."""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from typing import Optional

from check_stage6_acceptance import sha256_file, validate


PROMOTION_DIRECTORY_PREFIX = "stage6-promotion-"
CANONICAL_ACCEPTANCE_MANIFEST = Path("tests/libp2p_interop/p2p_donor_capabilities.json")


def write_receipt(path: Path, receipt: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n")


def create_invocation_directory(base_directory: Path) -> Path:
    """Allocate an isolated artifact root so concurrent promotions cannot collide."""
    base_directory.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=PROMOTION_DIRECTORY_PREFIX, dir=base_directory))


def forced_live_environment(inherited: Optional[dict[str, str]] = None) -> dict[str, str]:
    environment = dict(os.environ if inherited is None else inherited)
    environment["FORGE_ENABLE_LIBP2P_INTEROP"] = "1"
    return environment


def promotion_status(returncode: int, errors: list[str]) -> str:
    return "FAILED" if returncode != 0 or errors else "PASS"


def resolve_canonical_acceptance_manifest(root: Path, value: str) -> Path:
    """Promotion owns one source-tree manifest; arbitrary artifact manifests are not authority."""
    manifest = Path(value).resolve()
    expected = (root / CANONICAL_ACCEPTANCE_MANIFEST).resolve()
    if manifest != expected:
        raise ValueError(f"acceptance manifest must resolve exactly to {expected}")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", required=True)
    parser.add_argument("--forge-fixture", required=True)
    parser.add_argument("--source-dir", required=True)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--forge-root", required=True)
    parser.add_argument("--donors-root", required=True)
    parser.add_argument("--acceptance-manifest", required=True)
    parser.add_argument("--expected-head", required=True)
    parser.add_argument("--suite", choices=("stage6", "autonat", "mdns"), default="stage6")
    args = parser.parse_args()

    root = Path(args.forge_root).resolve()
    try:
        manifest_path = resolve_canonical_acceptance_manifest(root, args.acceptance_manifest)
    except ValueError as error:
        print(f"FAILED: {error}", file=sys.stderr)
        return 2
    build_base = Path(args.build_dir).resolve()
    invocation_directory = create_invocation_directory(build_base)
    artifact_path = invocation_directory / (f"{args.suite}-artifacts.json" if args.suite != "stage6" else "interop-artifacts.json")
    receipt_path = invocation_directory / f"{args.suite}-promotion-receipt.json"
    runner_argv = [
        str(Path(sys.executable).resolve()),
        str(Path(args.runner).resolve()),
        "--enabled", "1",
        "--forge-fixture", str(Path(args.forge_fixture).resolve()),
        "--source-dir", str(Path(args.source_dir).resolve()),
        "--build-dir", str(invocation_directory),
        "--forge-root", str(root),
        "--donors-root", str(Path(args.donors_root).resolve()),
        "--acceptance-manifest", str(manifest_path),
    ]
    if args.suite != "stage6":
        runner_argv += ["--suite", args.suite]
    started = time.time()
    result = subprocess.run(runner_argv, cwd=root, env=forced_live_environment(), check=False)
    finished = time.time()
    receipt = {
        "schema_version": 2,
        "runner_argv": runner_argv,
        "started_at_unix": started,
        "finished_at_unix": finished,
        "returncode": result.returncode,
        "invocation_directory": str(invocation_directory),
        "artifact_path": str(artifact_path),
        "artifact_sha256": sha256_file(artifact_path) if artifact_path.is_file() else None,
    }
    write_receipt(receipt_path, receipt)

    errors, has_limitations = validate(
        root, manifest_path, artifact_path, args.expected_head, receipt, expected_suite=args.suite
    )
    print(f"{args.suite} promotion evidence: {invocation_directory}", file=sys.stderr)
    if promotion_status(result.returncode, errors) == "FAILED":
        if result.returncode != 0:
            print(f"FAILED: canonical runner exited with {result.returncode}; receipt={receipt_path}", file=sys.stderr)
        for error in errors:
            print(f"FAILED: {error}", file=sys.stderr)
        return result.returncode or 1
    if has_limitations:
        print("PASS_WITH_DOCUMENTED_LIMITATIONS: canonical runner executed and was validated in this promotion")
    else:
        scope = ("full Stage 6 including AutoNAT41 and mDNS38" if args.suite == "stage6" else
                 "focused mDNS38 only, not full Stage 6 or production support" if args.suite == "mdns" else
                 "focused AutoNAT41 only, not full Stage 6")
        print(f"PASS: {scope}; canonical runner executed and was validated in this promotion")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
