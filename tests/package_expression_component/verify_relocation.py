#!/usr/bin/env python3
"""Exercise a genuine installed Forge package with inaccessible producer trees."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile
import time


def digest(path):
   result = hashlib.sha256()
   with path.open("rb") as stream:
      for block in iter(lambda: stream.read(1024 * 1024), b""):
         result.update(block)
   return result.hexdigest()


def within(path, root):
   return path == root or root in path.parents


def read_manifest(path, schema):
   value = json.loads(path.read_text())
   if value.get("schema") != schema:
      raise RuntimeError(f"Unexpected installed provenance schema: {path.name}")
   return value


def verify_provenance(directory, producer_worker, configuration):
   backend_path = directory / "build-manifest.json"
   runtime_path = directory / "runtime-manifest.json"
   worker_path = directory / "worker-manifest.json"
   pins_path = directory / "sources.json"
   backend = read_manifest(backend_path, "forge.expression.cel.build.v1")
   runtime = read_manifest(runtime_path, "forge.expression.worker.runtime.v1")
   worker = read_manifest(worker_path, "forge.expression.worker.native.v1")
   pins = read_manifest(pins_path, "forge.expression.cel.sources.v1")
   worker_hash = digest(producer_worker)
   if (worker["binary"]["sha256"] != worker_hash or
       worker["binary"]["name"] != producer_worker.name or
       worker["binary"]["architecture"] != "arm64"):
      raise RuntimeError("Installed worker provenance does not identify the real producer executable")
   if (worker["backend_manifest_sha256"] != digest(backend_path) or
       worker["runtime_manifest_sha256"] != digest(runtime_path) or
       worker["backend_archive_sha256"] != backend["archive_sha256"]):
      raise RuntimeError("Installed worker/backend/runtime provenance chain is inconsistent")
   if (backend["sources"] != pins or runtime["upstream_sources"] != pins["llvm"] or
       worker["llvm_version"] != runtime["llvm_version"] or
       runtime["llvm_version"] != pins["llvm"]["version"] or
       not runtime["single_thread_only"] or not runtime["source_unmodified"]):
      raise RuntimeError("Installed native runtime provenance differs from its source pins")
   if configuration and backend["configuration"] != configuration:
      raise RuntimeError("Installed backend provenance has a different build configuration")
   # These paths identify original producer artifacts. The consumer does not
   # open them: the installed runtime manifest itself is bound above by hash.
   if worker["runtime_archives"] != runtime["archives"]:
      raise RuntimeError("Installed worker records different private runtime archives")
   for filename, expected in (("sources.json", worker["source_hashes"]["cmake/cel/sources.json"]),
                              ("BUILD.bazel", backend["private_package_sha256"]),
                              ("MODULE.bazel.lock", backend["module_lock_sha256"])):
      if digest(directory / filename) != expected:
         raise RuntimeError(f"Installed CEL provenance differs from its qualified source: {filename}")
   if (digest(directory / "BUILD.bazel") != worker["source_hashes"]["cmake/cel/BUILD.bazel"] or
       digest(directory / "MODULE.bazel.lock") != worker["source_hashes"]["cmake/cel/MODULE.bazel.lock"]):
      raise RuntimeError("Installed CEL build definitions differ from the worker's qualified sources")
   if not (directory / "MODULE.bazel").is_file():
      raise RuntimeError("Installed CEL provenance is missing MODULE.bazel")
   notices = {}
   for notice in backend["notices"] + runtime["notices"]:
      relative = Path(notice["repository"]) / notice["file"]
      path = (directory / relative).resolve(strict=True)
      if relative.is_absolute() or ".." in relative.parts or not within(path, directory.resolve()):
         raise RuntimeError("Installed dependency notice is outside the provenance directory")
      if digest(path) != notice["sha256"]:
         raise RuntimeError("Installed dependency notice differs from its qualified manifest")
      if relative.as_posix() in notices and notices[relative.as_posix()] != notice["sha256"]:
         raise RuntimeError("Installed dependency notices have conflicting provenance")
      notices[relative.as_posix()] = notice["sha256"]
   for filename, pin in pins["llvm"]["files"].items():
      if filename.endswith("LICENSE.TXT"):
         relative = "llvm-" + filename.split("/")[0] + "/LICENSE.TXT"
         if notices.get(relative) != pin["sha256"]:
            raise RuntimeError("Installed LLVM license does not match its pinned upstream source")
   return backend, worker_hash, {
      "worker_manifest_sha256": digest(worker_path),
      "backend_manifest_sha256": digest(backend_path),
      "runtime_manifest_sha256": digest(runtime_path),
      "source_pins_sha256": digest(pins_path),
      "verified_notices": len(notices),
   }


def run(command, root, log_name, environment, expected_denial=False):
   started = time.monotonic()
   result = None
   try:
      # Keep partial compiler/configuration output even if a resource monitor
      # interrupts the outer build before the subprocess returns.
      with (root / log_name).open("w") as output:
         result = subprocess.run(command, cwd=root, env=environment, text=True,
                                 stdout=output, stderr=subprocess.STDOUT, timeout=900)
   finally:
      record = {"argv": command, "cwd": str(root), "log": log_name,
                "elapsed_seconds": round(time.monotonic() - started, 6),
                "exit_code": result.returncode if result else None}
      with (root / "commands.jsonl").open("a") as output:
         output.write(json.dumps(record) + "\n")
   if expected_denial:
      diagnostic = (root / log_name).read_text().lower()
      if result.returncode == 0 or not any(message in diagnostic
          for message in ("operation not permitted", "permission denied")):
         raise RuntimeError(f"Sandbox denial was not established; see {root / log_name}")
   elif result.returncode != 0:
      raise RuntimeError(f"Command failed ({result.returncode}); see {root / log_name}")
   return result.returncode


def main():
   parser = argparse.ArgumentParser(description=__doc__)
   parser.add_argument("--source-root", required=True, type=Path)
   parser.add_argument("--build-root", required=True, type=Path)
   parser.add_argument("--toolchain-cache", required=True, type=Path)
   parser.add_argument("--worker", required=True, type=Path)
   parser.add_argument("--generator", required=True)
   parser.add_argument("--cmake", default="cmake")
   parser.add_argument("--configuration", default="Release")
   parser.add_argument("--libdir", default="lib")
   parser.add_argument("--datadir", default="share")
   parser.add_argument("--dependency-prefix", action="append", default=[])
   parser.add_argument("--boost-dir", default="")
   parser.add_argument("--work-root", type=Path)
   args = parser.parse_args()

   if platform.system() != "Darwin" or not Path("/usr/bin/sandbox-exec").is_file():
      raise RuntimeError("This source-unavailable gate requires macOS sandbox-exec")
   source = args.source_root.resolve(strict=True)
   build = args.build_root.resolve(strict=True)
   cache = args.toolchain_cache.resolve(strict=True)
   producer_worker = args.worker.resolve(strict=True)
   if not producer_worker.is_file() or not os.access(producer_worker, os.X_OK):
      raise RuntimeError("The real producer worker executable is required")
   if not (source / "AGENTS.md").is_file() or not (build / "CMakeCache.txt").is_file():
      raise RuntimeError("The real configured producer source and build trees are required")
   for directory in (args.libdir, args.datadir):
      if Path(directory).is_absolute() or ".." in Path(directory).parts:
         raise RuntimeError("Install directories must be relative to the relocated prefix")
   dependencies = [Path(item).resolve(strict=True) for item in args.dependency_prefix]
   dependency_hints = list(dependencies)
   if args.boost_dir:
      dependency_hints.append(Path(args.boost_dir).resolve(strict=True))
   if any(within(path, forbidden) for path in dependency_hints for forbidden in (source, build)):
      raise RuntimeError("Consumer dependency hints must be outside the producer trees")

   if args.work_root:
      # mkdir is deliberately exclusive: never remove or reuse another run's files.
      root = args.work_root.absolute()
      root.mkdir(mode=0o700)
      root = root.resolve(strict=True)
   else:
      root = Path(tempfile.mkdtemp(prefix="forge-expression-installed-")).resolve(strict=True)
   if any(within(root, forbidden) for forbidden in (source, build)):
      raise RuntimeError("The acceptance directory must be outside producer source/build trees")

   report = {"schema": "forge.expression.installed.v1", "status": "RUNNING", "root": str(root),
             "source_root": str(source), "build_root": str(build), "configuration": args.configuration,
             "platform": platform.platform(), "denials": {}}
   report_file = root / "result.json"
   started = time.monotonic()
   print(f"Installed expression acceptance artifacts: {root}", flush=True)
   environment = os.environ.copy()
   # Do not let ambient include paths select headers from a different dependency/toolchain.
   for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
      environment.pop(name, None)
   try:
      original = root / "original"
      relocated = root / "relocated"
      command = [args.cmake, "--install", str(build), "--prefix", str(original), "--component", "dev"]
      if args.configuration:
         command += ["--config", args.configuration]
      run(command, root, "install.log", environment)

      cel_docs = original / args.datadir / "doc/Forge/CEL"
      manifest, worker_hash, provenance = verify_provenance(cel_docs, producer_worker, args.configuration)
      report["producer_worker_sha256"] = worker_hash
      report["provenance"] = provenance
      report["verified_notices"] = provenance["verified_notices"]

      bazel = Path(manifest["command"][0]).resolve(strict=True)
      forbidden_roots = list(dict.fromkeys((source, build, original)))
      profile = "(version 1)\n(allow default)\n"
      for forbidden in forbidden_roots:
         profile += f"(deny file-read* (subpath {json.dumps(str(forbidden))}))\n"
      profile += '(deny process-exec (regex #".*/(bazel|bazelisk)([^/]*)$"))\n'
      profile += f"(deny process-exec (literal {json.dumps(str(bazel))}))\n"
      profile_file = root / "sandbox.sb"
      profile_file.write_text(profile)
      sandbox = ["/usr/bin/sandbox-exec", "-f", str(profile_file)]
      probes = {"source": source / "AGENTS.md", "build": build / "CMakeCache.txt",
                "old_prefix": original / args.libdir / "cmake/Forge/ForgeConfig.cmake"}
      for name, path in probes.items():
         if not path.is_file():
            raise RuntimeError(f"Sandbox probe has no genuine producer file: {path}")
         report["denials"][name] = run(sandbox + ["/bin/cat", str(path)], root,
                                          f"deny-{name}.log", environment, expected_denial=True)
      report["denials"]["bazel"] = run(sandbox + [str(bazel), "--version"], root,
                                         "deny-bazel.log", environment, expected_denial=True)

      original.rename(relocated)
      if original.exists():
         raise RuntimeError("The original install prefix still exists after relocation")
      report["original_prefix_absent"] = True
      consumer_source = root / "consumer-source"
      consumer_source.mkdir()
      for filename in ("CMakeLists.txt", "main.cpp"):
         shutil.copy2(source / "tests/package_expression_component" / filename, consumer_source / filename)
      consumer_cache = root / "toolchain.cmake"
      shutil.copy2(cache, consumer_cache)
      consumer_build = root / "consumer-build"
      prefixes = ";".join([str(relocated)] + [str(path) for path in dependencies])
      configure = [args.cmake, "--fresh", "-S", str(consumer_source), "-B", str(consumer_build),
                   "-G", args.generator, "-C", str(consumer_cache),
                   f"-DCMAKE_PREFIX_PATH={prefixes}",
                   f"-DForge_DIR={relocated / args.libdir / 'cmake/Forge'}",
                   f"-DFORGE_EXPECTED_PREFIX={relocated}",
                   "-DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF", "-DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF",
                   "-DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_LLVM=ON"]
      if args.boost_dir:
         configure += [f"-DBoost_DIR={args.boost_dir}"]
      run(sandbox + configure, root, "configure.log", environment)
      installed_worker = Path((consumer_build / "worker-location.txt").read_text().strip()).resolve(strict=True)
      if not within(installed_worker, relocated) or not os.access(installed_worker, os.X_OK):
         raise RuntimeError("The consumer did not select an executable from the relocated prefix")
      installed_worker_hash = digest(installed_worker)
      if installed_worker_hash != report["producer_worker_sha256"]:
         raise RuntimeError("The installed CEL worker differs from the real producer executable")
      report["installed_worker_sha256"] = installed_worker_hash
      compile_command = [args.cmake, "--build", str(consumer_build), "--parallel", "4"]
      if args.configuration:
         compile_command += ["--config", args.configuration]
      run(sandbox + compile_command, root, "build.log", environment)
      executable = consumer_build / "forge_package_expression_component"
      if not executable.is_file() and args.configuration:
         executable = consumer_build / args.configuration / executable.name
      run(sandbox + [str(executable)], root, "runtime.log", environment)
      report["relocated_prefix"] = str(relocated)
      report["consumer_sha256"] = digest(executable)
      report["status"] = "PASS"
   except KeyboardInterrupt:
      report["status"] = "INTERRUPTED"
      report["error"] = "External interruption before acceptance completed; see resource monitor logs"
      raise
   except Exception as error:
      report["status"] = "FAILED"
      report["error"] = str(error)
      raise
   finally:
      report["total_seconds"] = round(time.monotonic() - started, 6)
      report_file.write_text(json.dumps(report, indent=2) + "\n")
   print(f"Installed expression relocation/source denial/Bazel denial/runtime PASS: {report_file}")


if __name__ == "__main__":
   main()
