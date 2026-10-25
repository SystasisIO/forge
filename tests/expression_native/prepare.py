#!/usr/bin/env python3
"""Prepare and verify pinned research-only dependencies under root vendor/."""

import argparse
import hashlib
import json
import shutil
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path


def verified_download(url, checksum, path):
    if not path.exists():
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_suffix(path.suffix + ".download")
        total = 0
        with urllib.request.urlopen(url, timeout=30) as response, temporary.open("wb") as output:
            while block := response.read(1024 * 1024):
                total += len(block)
                if total > 64 * 1024 * 1024:
                    raise ValueError("dependency archive exceeds research download cap")
                output.write(block)
        if hashlib.sha256(temporary.read_bytes()).hexdigest() != checksum:
            raise ValueError("download checksum mismatch")
        temporary.rename(path)
    if hashlib.sha256(path.read_bytes()).hexdigest() != checksum:
        raise ValueError("cached dependency checksum mismatch")


def prepare(root, build):
    manifest = json.loads(Path(__file__).with_name("sources.json").read_text())
    vendor = root / "vendor"
    source = manifest["cel"]
    archive = build / "downloads/cel-cpp.tar.gz"
    verified_download(source["url"], source["sha256"], archive)
    destination = vendor / source["directory"]
    with tarfile.open(archive) as contents:
        prefix = "cel-cpp-" + source["commit"]
        members = contents.getmembers()
        for member in members:
            parts = Path(member.name).parts
            if not parts or parts[0] != prefix or ".." in parts or member.issym() or member.islnk():
                raise ValueError("unexpected source archive member")
        if not destination.exists():
            with tempfile.TemporaryDirectory(prefix="expression-native-", dir=vendor) as directory:
                contents.extractall(directory, filter="data")
                shutil.move(str(Path(directory) / prefix), destination)
        checked = 0
        for member in members:
            if not member.isfile():
                continue
            target = destination / Path(member.name).relative_to(prefix)
            if not target.is_file():
                raise ValueError("pinned CEL source file is missing")
            expected = hashlib.sha256(contents.extractfile(member).read()).hexdigest()
            if hashlib.sha256(target.read_bytes()).hexdigest() != expected:
                raise ValueError("pinned CEL source file has changed: " + str(target.relative_to(root)))
            checked += 1
    exprtk = manifest["exprtk"]
    for filename, digest in exprtk["files"].items():
        url = f"https://raw.githubusercontent.com/ArashPartow/exprtk/{exprtk['commit']}/{filename}"
        verified_download(url, digest, vendor / exprtk["directory"] / filename)
    print(json.dumps({"verified_cel_files": checked, "cel_archive_sha256": source["sha256"],
                      "exprtk_files": exprtk["files"]}, sort_keys=True))


if __name__ == "__main__":
    if sys.version_info < (3, 12):
        raise SystemExit("prepare.py requires Python 3.12+ for tarfile data filtering")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-root", type=Path)
    arguments = parser.parse_args()
    repository = Path(__file__).resolve().parents[2]
    prepare(repository, arguments.build_root or repository / "build/expression_native")
