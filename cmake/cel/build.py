"""Build the private CEL artifact through its pinned upstream Bazel graph."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess
import tarfile
import urllib.request


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def download(url, expected, destination):
    if destination.exists():
        if digest(destination) != expected:
            raise RuntimeError(f"Pinned download has changed: {destination.name}")
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + ".download")
    with urllib.request.urlopen(url, timeout=120) as source, temporary.open("wb") as target:
        shutil.copyfileobj(source, target)
    if digest(temporary) != expected:
        temporary.unlink()
        raise RuntimeError(f"SHA-256 mismatch for {destination.name}")
    temporary.replace(destination)


def copy_changed(source, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists() or digest(source) != digest(destination):
        shutil.copyfile(source, destination)


def prepare_source(archive, pin, vendor, staging):
    prefix = f"cel-cpp-{pin['commit']}/"
    count = 0
    with tarfile.open(archive, "r:gz") as contents:
        original_names = {member.name[len(prefix):] for member in contents.getmembers()
                          if member.isfile() and member.name.startswith(prefix)}
        for optional in ("clang.bazelrc", "user.bazelrc", "local_tsan.bazelrc"):
            if optional not in original_names and ((staging / optional).exists() or (staging / optional).is_symlink()):
                raise RuntimeError(f"Unpinned optional Bazel configuration in staging: {optional}")
        for member in contents.getmembers():
            if not member.isfile():
                continue
            if not member.name.startswith(prefix):
                raise RuntimeError("Unexpected CEL archive root")
            relative = Path(member.name[len(prefix):])
            if relative.is_absolute() or ".." in relative.parts:
                raise RuntimeError("Invalid CEL archive member")
            original = contents.extractfile(member).read()
            upstream_hash = hashlib.sha256(original).hexdigest()
            supplied = vendor / relative
            if vendor.exists() and (not supplied.is_file() or digest(supplied) != upstream_hash):
                raise RuntimeError(f"Pinned vendor/cel-cpp source differs: {relative}")
            target = staging / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            if not target.exists() or digest(target) != upstream_hash:
                target.write_bytes(original)
            count += 1
    return count


def collect_notices(staging, execution_root, destination):
    roots = [("cel-cpp", staging)]
    external = execution_root / "external"
    if external.is_dir():
        # Bazel owns the resolved repositories. Preserve their notices directly;
        # do not infer or recreate their source/dependency graph.
        roots.extend((entry.name, entry) for entry in sorted(external.iterdir()) if entry.is_dir())
    collected = []
    for name, root in roots:
        for directory, subdirectories, filenames in os.walk(root, followlinks=False):
            subdirectories[:] = sorted(part for part in subdirectories
                if part not in (".git", ".cache", "node_modules") and not part.startswith("bazel-"))
            for filename in sorted(filenames):
                if not filename.upper().startswith(("LICENSE", "NOTICE", "COPYING", "AUTHORS")):
                    continue
                source = Path(directory) / filename
                if source.is_file():
                    relative = source.relative_to(root)
                    target = destination / name / relative
                    copy_changed(source, target)
                    collected.append({"repository": name, "file": relative.as_posix(), "sha256": digest(source)})
    if not any(item["repository"] == "cel-cpp" for item in collected):
        raise RuntimeError("CEL license was not collected")
    if not any(item["repository"].startswith("protobuf+") and item["file"] == "third_party/utf8_range/LICENSE"
               for item in collected):
        raise RuntimeError("The linked protobuf utf8_range license was not collected")
    return collected


def toolchain_evidence(common, command, staging, environment, execution_root, c_driver, cxx_driver):
    query = common + ["aquery", 'mnemonic("CppCompile", //forge_native:backend)',
                      "--output=jsonproto", "--include_artifacts=false"] + command[len(common) + 2:]
    result = json.loads(subprocess.check_output(query, cwd=staging, env=environment, text=True))
    actions = [action["arguments"] for action in result.get("actions", [])]
    if not all(any(any(argument.endswith(name) for argument in argv) for argv in actions)
               for name in ("/backend.cpp", "/arithmetic.cpp")):
        raise RuntimeError("Bazel toolchain evidence is missing a private C++ compilation")
    wrappers = []
    for wrapper_name in sorted({argv[0] for argv in actions}):
        wrapper = execution_root / wrapper_name
        drivers = []
        for line in wrapper.read_text().splitlines():
            if not line.strip().endswith('"$@"'):
                continue
            words = shlex.split(line, comments=True)
            if len(words) == 2 and words[1] == "$@" and Path(words[0]).is_absolute():
                drivers.append(Path(words[0]).resolve())
        if drivers != [c_driver] or c_driver != cxx_driver:
            raise RuntimeError("The resolved Bazel C++ wrapper does not use the selected LLVM driver")
        include_file = wrapper.parent / "builtin_include_directory_paths"
        include_roots = [line for line in include_file.read_text().splitlines() if line.startswith("/")]
        if str(c_driver.parent.parent / "include/c++/v1") not in include_roots:
            raise RuntimeError("Bazel's resolved standard-library headers differ from the LLVM driver")
        wrappers.append({"wrapper": wrapper_name, "wrapper_sha256": digest(wrapper),
                         "driver": str(drivers[0]), "builtin_include_roots": include_roots})
    return {"compile_arguments": actions, "resolved_wrappers": wrappers}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--build-root", type=Path, required=True)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--c-compiler", required=True)
    parser.add_argument("--sdk", required=True)
    parser.add_argument("--deployment-target", required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--bazel", default="")
    parser.add_argument("--repository-cache", default="")
    parser.add_argument("--cxx-flags", default="")
    parser.add_argument("--test", action="store_true")
    parser.add_argument("--prepare-only", action="store_true")
    parser.add_argument("--update-lockfile", action="store_true",
                        help="Explicit qualification only: refresh the staged lock for review")
    args = parser.parse_args()
    if platform.system() != "Darwin" or platform.machine() != "arm64":
        raise RuntimeError("The pinned CEL backend currently supports native macOS ARM64 only")
    root = args.source_root.resolve()
    build = args.build_root.resolve()
    c_driver = Path(args.c_compiler).resolve(strict=True)
    cxx_driver = Path(args.compiler).resolve(strict=True)
    c_version = subprocess.check_output([str(c_driver), "--version"], text=True).splitlines()[0]
    cxx_version = subprocess.check_output([str(cxx_driver), "--version"], text=True).splitlines()[0]
    if c_driver != cxx_driver or c_version != cxx_version or c_version.startswith("Apple"):
        raise RuntimeError("CEL requires a matching native LLVM Clang C/C++ driver pair")
    if not Path(args.sdk).is_absolute() or not Path(args.sdk).is_dir():
        raise RuntimeError("CEL requires an explicit existing SDK directory")
    owner = root / "cmake/cel"
    pins = json.loads((owner / "sources.json").read_text())
    downloads = build.parent / "downloads"
    source_archive = downloads / "cel-cpp.tar.gz"
    download(pins["cel"]["url"], pins["cel"]["sha256"], source_archive)
    bazel = Path(args.bazel).resolve() if args.bazel else downloads / "bazel-8.7.0-darwin-arm64"
    if args.bazel:
        if not bazel.is_file() or digest(bazel) != pins["bazel"]["sha256"]:
            raise RuntimeError("FORGE_CEL_BAZEL_EXECUTABLE must match the pinned SHA-256")
    else:
        download(pins["bazel"]["url"], pins["bazel"]["sha256"], bazel)
        bazel.chmod(0o755)
    staging = build / "source"
    source_count = prepare_source(source_archive, pins["cel"], root / "vendor/cel-cpp", staging)
    qualified_lock = owner / "MODULE.bazel.lock"
    if qualified_lock.is_file():
        copy_changed(qualified_lock, staging / "MODULE.bazel.lock")
    if args.prepare_only:
        print(f"Prepared {source_count} pinned CEL files and Bazel {pins['bazel']['version']}")
        return
    package = staging / "forge_native"
    library = package / "libraries/expression"
    for relative in ("backend.cpp", "arithmetic.cpp"):
        copy_changed(root / "libraries/expression" / relative, library / relative)
    private_headers = sorted((root / "libraries/expression/details").glob("*.hxx"))
    current_headers = {source.name for source in private_headers}
    for stale in (library / "details").glob("*.hxx"):
        if stale.name not in current_headers:
            stale.unlink()
    for source in private_headers:
        copy_changed(source, library / "details" / source.name)
    copy_changed(owner / "BUILD.bazel", package / "BUILD.bazel")
    if args.test:
        for relative in ("expression_arithmetic.cpp", "expression_arithmetic_cases.inc"):
            copy_changed(root / "tests" / relative, package / "tests" / relative)
    environment = os.environ.copy()
    for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH"):
        environment.pop(name, None)
    environment.update(CC=args.c_compiler, CXX=args.compiler, SDKROOT=args.sdk, BAZEL_USE_CPP_ONLY_TOOLCHAIN="1")
    # Use precisely the CMake-selected libc++ alongside the selected compiler.
    runtime = Path(args.compiler).resolve().parent.parent / "lib/c++"
    common = [str(bazel), "--nosystem_rc", "--nohome_rc", f"--output_user_root={build / 'bazel'}", "--host_jvm_args=-Xmx768m"]
    command = common + ["test" if args.test else "build", "//forge_native:arithmetic_test" if args.test else "//forge_native:archive",
        "--jobs=4", "--compilation_mode=" + ("dbg" if args.configuration == "Debug" else "opt"),
        "--copt=-O2", "--host_copt=-O2",
        "--copt=-g0", "--copt=-fno-fast-math", "--host_copt=-fno-fast-math",
        "--copt=-ffp-contract=off", "--host_copt=-ffp-contract=off", "--force_pic",
        "--features=-layering_check", "--host_features=-layering_check",
        "--cxxopt=-std=c++23", "--host_cxxopt=-std=c++23",
        f"--cxxopt=-mmacosx-version-min={args.deployment_target}",
        f"--host_cxxopt=-mmacosx-version-min={args.deployment_target}",
        "--lockfile_mode=" + ("update" if args.update_lockfile else "error"),
        f"--repository_cache={args.repository_cache or build.parent / 'repository-cache'}",
        f"--symlink_prefix={build / 'bazel-'}"]
    for host in (False, True):
        copt = "--host_copt=" if host else "--copt="
        linkopt = "--host_linkopt=" if host else "--linkopt="
        command += [copt + "-isysroot", copt + args.sdk,
            copt + f"-mmacosx-version-min={args.deployment_target}",
            linkopt + "-isysroot", linkopt + args.sdk,
            linkopt + f"-mmacosx-version-min={args.deployment_target}",
            linkopt + f"-L{runtime}", linkopt + f"-Wl,-rpath,{runtime}"]
    # Sanitizer and other CMake toolchain selectors apply to this separate build
    # as well. CMake's Debug vendor policy still selects O2 after configuration.
    flags = shlex.split(args.cxx_flags)
    for flag in flags:
        if flag.startswith(("-O", "-std=")) or flag == "-Wall":
            continue
        command += ["--cxxopt=" + flag, "--host_cxxopt=" + flag]
        if flag.startswith("-fsanitize="):
            command += ["--linkopt=" + flag, "--host_linkopt=" + flag]
    if args.test:
        command.append("--test_output=errors")
    else:
        command.append("--output_groups=default,linkdeps,linkopts")
    subprocess.run(command, cwd=staging, env=environment, check=True)
    if args.test:
        return
    output = build / "bazel-bin/forge_native"
    linkdeps = (output / "archive_linkdeps.txt").read_text().splitlines()
    linkopts = (output / "archive_linkopts.txt").read_text().splitlines()
    if linkdeps:
        raise RuntimeError(f"CEL archive has unbundled libraries: {linkdeps}")
    # Darwin libSystem facilities are supplied by the compiler; CoreFoundation
    # is resolved by the owning CMake imported target. Reject every other need.
    unknown = []
    offset = 0
    while offset < len(linkopts):
        option = linkopts[offset]
        if option == "-framework" and offset + 1 < len(linkopts) and linkopts[offset + 1] == "CoreFoundation":
            offset += 2
            continue
        if option not in ("-pthread", "-lpthread", "-lm", "-ldl", "-Wl,-framework,CoreFoundation"):
            unknown.append(option)
        offset += 1
    if unknown:
        raise RuntimeError(f"Unqualified CEL archive linker requirements: {unknown}")
    archive = build / "libforge_expression_backend.a"
    copy_changed(output / "libarchive.a", archive)
    execution_root = Path(subprocess.check_output(common + ["info", "execution_root"], cwd=staging, env=environment, text=True).strip())
    toolchain = toolchain_evidence(common, command, staging, environment, execution_root, c_driver, cxx_driver)
    notices = collect_notices(staging, execution_root, build / "notices")
    manifest = {"schema": "forge.expression.cel.build.v1", "sources": pins,
        "verified_upstream_files": source_count,
        "module_lock_sha256": digest(staging / "MODULE.bazel.lock"),
        "private_package_sha256": digest(package / "BUILD.bazel"),
        "private_sources": {str(path.relative_to(package)): digest(path) for path in sorted(package.rglob("*")) if path.is_file()},
        "configuration": args.configuration, "compiler": args.compiler,
        "compiler_version": cxx_version, "c_compiler_version": c_version,
        "c_compiler_realpath": str(c_driver), "cxx_compiler_realpath": str(cxx_driver),
        "bazel_toolchain": toolchain,
        "sdk": args.sdk, "deployment_target": args.deployment_target, "architecture": "arm64",
        "command": command, "archive_sha256": digest(archive),
        "linkdeps": linkdeps, "linkopts": linkopts, "notices": notices}
    (build / "build-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"CEL backend: {archive} ({source_count} pinned upstream files, no unbundled libraries)")


if __name__ == "__main__":
    main()
