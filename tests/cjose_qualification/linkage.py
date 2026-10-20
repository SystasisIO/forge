"""Check actual installed dependency resolution, not just link command strings."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import subprocess
import sys


def output(*args):
    env = {k: v for k, v in os.environ.items() if k not in ("LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH", "DYLD_INSERT_LIBRARIES")}
    return subprocess.check_output(args, text=True, stderr=subprocess.DEVNULL, env=env)


def mac_paths(binary):
    text = output("otool", "-l", str(binary))
    return re.findall(r"cmd LC_RPATH\n\s*cmdsize \d+\n\s*path (.*?) \(offset", text)


def mac_tree(binary):
    root_rpaths = mac_paths(binary)
    pending, visited, found = [binary], set(), []
    while pending:
        image = pending.pop().resolve()
        if image in visited:
            continue
        visited.add(image)
        rpaths = mac_paths(image) + root_rpaths
        for line in output("otool", "-L", str(image)).splitlines()[1:]:
            name = line.strip().split(" (", 1)[0]
            if name.startswith(("/usr/lib/", "/System/")):
                continue
            if name.startswith("@rpath/"):
                candidates = []
                for path in rpaths:
                    path = path.replace("@loader_path", str(image.parent)).replace("@executable_path", str(binary.parent))
                    candidates.append(pathlib.Path(path) / name[len("@rpath/"):])
                matches = [p for p in candidates if p.is_file()]
                if not matches:
                    raise RuntimeError("unresolved_rpath")
                path = matches[0]
            else:
                name = name.replace("@loader_path", str(image.parent)).replace("@executable_path", str(binary.parent))
                path = pathlib.Path(name)
            if not path.is_file():
                raise RuntimeError("unresolved_dependency")
            if path.resolve() == image:
                continue
            found.append(path.resolve())
            pending.append(path)
    return set(found)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    parser.add_argument("--deps", required=True, type=pathlib.Path)
    parser.add_argument("--linkage", required=True, choices=("shared", "static"))
    parser.add_argument("--evidence", required=True, type=pathlib.Path)
    args = parser.parse_args()
    if sys.platform == "darwin":
        paths = mac_tree(args.binary)
    else:
        listing = output("ldd", "-r", str(args.binary))
        if "not found" in listing or "undefined symbol:" in listing:
            raise RuntimeError("unresolved_dependency")
        paths = {pathlib.Path(p).resolve() for p in re.findall(r"=> (/\S+)", listing)}
    libraries = {}
    for name in ("libcjose", "libjansson", "libcrypto", "libssl"):
        libraries[name] = sorted({p for p in paths if p.name.startswith(name + ".")})
    if len(libraries["libcjose"]) != (1 if args.linkage == "shared" else 0):
        raise RuntimeError("cjose_copy_count")
    if args.linkage == "shared" and not libraries["libcjose"][0].is_relative_to(args.prefix.resolve()):
        raise RuntimeError("wrong_cjose")
    if len(libraries["libcrypto"]) != 1 or libraries["libssl"]:
        raise RuntimeError("openssl_copy_count")
    if not libraries["libcrypto"][0].is_relative_to(pathlib.Path(os.environ["OPENSSL_ROOT_DIR"]).resolve()):
        raise RuntimeError("wrong_openssl")
    if len(libraries["libjansson"]) != 1 or not libraries["libjansson"][0].is_relative_to(args.deps.resolve()):
        raise RuntimeError("wrong_jansson")
    args.evidence.write_text(json.dumps({"status": "PASS", "linkage": args.linkage,
        "libraries": {name: [{"file": p.name, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()} for p in values]
                      for name, values in libraries.items()}}, indent=2) + "\n")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print("installed linkage verification failed")
        raise SystemExit(1)
