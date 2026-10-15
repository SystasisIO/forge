"""CI-only backend driver. Raw output is local; only allowlisted evidence uploads."""

import argparse
import hashlib
import json
import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIXTURE = ROOT / "tests/cjose_qualification"
PINS = json.loads((FIXTURE / "pins.json").read_text())
ARGS = argparse.ArgumentParser()
ARGS.add_argument("phase", choices=("prepare", "baseline", "candidate", "legacy-abi", "consumers", "sanitizers", "evidence"))
ARGS.add_argument("--work", type=pathlib.Path, required=True)
OPTIONS = ARGS.parse_args()
WORK = OPTIONS.work
RAW = WORK / "raw"
EVIDENCE = WORK / "evidence"
RAW.mkdir(parents=True, exist_ok=True)
EVIDENCE.mkdir(parents=True, exist_ok=True)


def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n")


def run(label, command, *, cwd=ROOT, timeout=600, env=None, required=True):
    started = time.monotonic()
    record = {"label": label, "status": "RUNNING", "command": [str(x).replace(str(WORK), "$WORK").replace(str(ROOT), "$ROOT") for x in command]}
    save(EVIDENCE / (label + ".json"), record)
    with (RAW / (label + ".log")).open("wb") as output:
        try:
            result = subprocess.run([str(x) for x in command], cwd=cwd, env=env, stdout=output, stderr=subprocess.STDOUT, timeout=timeout, check=False)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 124
    record.update(exit=code, elapsed_seconds=round(time.monotonic() - started, 6), status="PASS" if code == 0 else "FAILED")
    save(EVIDENCE / (label + ".json"), record)
    print(f"{label}: exit={code}", flush=True)
    if required and code != 0:
        raise RuntimeError(label)
    return code


def capture(command):
    return subprocess.check_output([str(x) for x in command], text=True, stderr=subprocess.DEVNULL).strip()


def fetch(name, url, sha):
    if not re.fullmatch(r"[0-9a-f]{40}", sha):
        raise RuntimeError("non_immutable_ref")
    source = WORK / (name + "-src")
    run(name + "-init", ["git", "init", "--quiet", source])
    run(name + "-fetch", ["git", "-C", source, "fetch", "--quiet", "--depth=1", url, sha])
    run(name + "-checkout", ["git", "-C", source, "checkout", "--quiet", "--detach", "FETCH_HEAD"])
    if capture(["git", "-C", source, "rev-parse", "HEAD"]) != sha:
        raise RuntimeError("source_sha_mismatch")
    return source


def cmake_options(prefix, sanitize=False):
    flags = "-O1 -g -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer" if sanitize else "-O2 -g"
    linker = macos_linker_flag()
    options = ["-G", "Ninja", "-DCMAKE_BUILD_TYPE=RelWithDebInfo", f"-DCMAKE_C_COMPILER={os.environ['CC']}",
            f"-DCMAKE_PREFIX_PATH={WORK / 'deps'}", f"-DOPENSSL_ROOT_DIR={os.environ['OPENSSL_ROOT_DIR']}",
            f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib", f"-DCMAKE_C_FLAGS={flags}",
            f"-DCMAKE_SHARED_LINKER_FLAGS={linker + ' ' if linker else ''}-Wl,-rpath,{WORK / 'deps/lib'} -Wl,-rpath,{os.environ['OPENSSL_ROOT_DIR']}/lib",
            f"-DCMAKE_BUILD_RPATH={prefix / 'lib'};{WORK / 'deps/lib'};{os.environ['OPENSSL_ROOT_DIR']}/lib",
            f"-DCMAKE_INSTALL_RPATH={WORK / 'deps/lib'};{os.environ['OPENSSL_ROOT_DIR']}/lib"]
    if linker:
        options.extend([f"-DCMAKE_EXE_LINKER_FLAGS={linker}", f"-DCMAKE_MODULE_LINKER_FLAGS={linker}",
                        f"-DCMAKE_OSX_SYSROOT={os.environ['SDKROOT']}"])
    return options


def macos_linker_flag():
    return f"-fuse-ld={os.environ['CJOSE_QUALIFICATION_LINKER']}" if sys.platform == "darwin" else ""


def macos_toolchain():
    if sys.platform != "darwin":
        return None
    evidence = WORK / "evidence/macos-toolchain.json"
    manifest = {"status": "FAILED", "stage": "version_check", "expected_version": PINS["macos_llvm_lld_version"]}
    save(evidence, manifest)
    cc, linker = pathlib.Path(os.environ["CC"]), pathlib.Path(os.environ["CJOSE_QUALIFICATION_LINKER"])
    compiler_version = re.search(r"clang version (\d+\.\d+\.\d+)", capture([cc, "--version"]))
    linker_version = re.search(r"LLD (\d+\.\d+\.\d+)", capture([linker, "--version"]))
    if not compiler_version or not linker_version or compiler_version[1] != linker_version[1] or compiler_version[1] != PINS["macos_llvm_lld_version"]:
        raise RuntimeError("macos_toolchain_version_mismatch")
    manifest.update(compiler_version=compiler_version[1], linker_version=linker_version[1], stage="driver_selection")
    save(evidence, manifest)
    selected = None
    default = None
    # -### prints the driver plan without compiling, linking or creating output.
    for label, extra in (("macos-default-linker-plan", []), ("macos-selected-linker-plan", [macos_linker_flag()])):
        run(label, [cc, "-###", "-x", "c", "-", "-o", WORK / "unused-toolchain-output",
                    "-O1", "-g", "-fsanitize=fuzzer-no-link,address,undefined", "-fno-omit-frame-pointer", *extra])
        actual = None
        for line in (RAW / (label + ".log")).read_text(errors="replace").splitlines():
            try:
                words = shlex.split(line)
            except ValueError:
                continue
            if words and pathlib.Path(words[0]).name in ("ld", "ld64.lld"):
                actual = pathlib.Path(words[0])
        if extra:
            selected = actual
        else:
            default = actual
    if selected is None or selected.resolve() != linker.resolve():
        raise RuntimeError("macos_linker_selection_not_proved")
    manifest.update(status="PASS", stage="selected", compiler=str(cc), linker=str(linker),
                    compiler_sha256=hashlib.sha256(cc.read_bytes()).hexdigest(),
                    linker_sha256=hashlib.sha256(linker.read_bytes()).hexdigest(),
                    driver_selected_linker=str(selected), driver_default_linker=str(default) if default else None,
                    sdk_version=capture(["xcrun", "--show-sdk-version"]), sdk_root=os.environ["SDKROOT"],
                    qualification="selection_only_runtime_gates_still_required")
    save(evidence, manifest)
    return manifest


def backend(name, source, sanitize=False):
    build, prefix = WORK / (name + "-build"), WORK / (name + "-installed")
    test_warning_policy = []
    if sanitize and sys.platform == "darwin":
        test_warning_policy.append(f"-DCMAKE_PROJECT_cjose_INCLUDE={FIXTURE / 'macos_upstream_test_warnings.cmake'}")
    run(name + "-configure", ["cmake", "-S", source, "-B", build, *cmake_options(prefix, sanitize),
        *test_warning_policy,
        "-DCJOSE_BUILD_SHARED=ON", "-DCJOSE_BUILD_STATIC=ON", "-DCJOSE_BUILD_TESTS=ON", "-DCJOSE_MACOS_DYLIB=ON"])
    if test_warning_policy and "QUALIFICATION_UPSTREAM_TEST_DEPRECATION_WARNING_ONLY" not in (RAW / (name + "-configure.log")).read_text(errors="replace"):
        raise RuntimeError("upstream_test_warning_policy_not_applied")
    run(name + "-build", ["cmake", "--build", build, "-j4"])
    if not (build / "check_cjose").is_file():
        raise RuntimeError("missing_upstream_tests")
    # Installing is independent of test success, allowing consumer diagnostics.
    run(name + "-install", ["cmake", "--install", build])
    suite(name + "-tests", build / "check_cjose", source / "test")


def suite(name, binary, directory):
    code = run(name, [binary], cwd=directory, timeout=360, required=False)
    text = (RAW / (name + ".log")).read_text(errors="replace")
    summaries = re.findall(r"Checks:\s*(\d+),\s*Failures:\s*(\d+),\s*Errors:\s*(\d+)", text)
    cases = re.findall(r"(?:^|\n)[^\n]*?:\d+:([PFE]):([A-Za-z0-9_]+):([A-Za-z0-9_]+):\d+:", text)
    ids = [case + ":" + test for _, case, test in cases]
    failure_ids = [case + ":" + test for status, case, test in cases if status != "P"]
    count, failures, errors = map(int, summaries[-1]) if summaries else (0, 0, 0)
    save(EVIDENCE / (name + "-summary.json"), {"exit": code, "checks": count, "failures": failures,
         "errors": errors, "test_ids": sorted(set(ids)), "failure_ids": sorted(set(failure_ids)),
         "status": "PASS" if code == 0 and count > 0 and failures == errors == 0 else "FAILED"})
    return code == 0 and count > 0 and failures == errors == 0


def prepare():
    toolchain = macos_toolchain()
    # Source resolution never follows a branch, tag, or mutable fork HEAD.
    entry = capture(["git", "ls-tree", "HEAD", "vendor/cjose"]).split()
    if len(entry) != 4 or entry[:2] != ["160000", "commit"]:
        raise RuntimeError("missing_candidate_gitlink")
    candidate_sha = entry[2]
    fetch("upstream", PINS["upstream_url"], PINS["upstream_sha"])
    fetch("historical", PINS["upstream_url"], PINS["historical_sha"])
    fetch("candidate", "https://github.com/SystasisIO/cjose.git", candidate_sha)
    jansson = fetch("jansson", PINS["jansson_url"], PINS["jansson_sha"])
    # A shallow fetch of the base into the fork allows an actual ancestry check.
    run("candidate-base-fetch", ["git", "-C", WORK / "candidate-src", "fetch", "--quiet", "--deepen=1000",
        "https://github.com/SystasisIO/cjose.git", candidate_sha])
    run("candidate-ancestry", ["git", "-C", WORK / "candidate-src", "merge-base", "--is-ancestor", PINS["upstream_sha"], candidate_sha])
    run("jansson-configure", ["cmake", "-S", jansson, "-B", WORK / "jansson-build", *cmake_options(WORK / "deps"),
        "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", "-DJANSSON_BUILD_SHARED_LIBS=ON", "-DJANSSON_BUILD_DOCS=OFF", "-DJANSSON_EXAMPLES=OFF"])
    run("jansson-build", ["cmake", "--build", WORK / "jansson-build", "-j4"])
    run("jansson-install", ["cmake", "--install", WORK / "jansson-build"])
    original = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in (WORK / "upstream-src/test").glob("check_*") if p.is_file()}
    save(EVIDENCE / "original-test-corpus.json", original)
    manifest = {"forge_sha": os.environ["GITHUB_SHA"], "candidate_sha": candidate_sha, **PINS,
        "runner_os": os.environ["RUNNER_OS"], "runner_image": os.environ.get("ImageVersion", "unavailable"),
        "compiler": capture([os.environ["CC"], "--version"]).splitlines()[0],
        "macos_toolchain": toolchain,
        "cmake": capture(["cmake", "--version"]).splitlines()[0], "check": capture(["pkg-config", "--modversion", "check"]),
        "openssl": capture([pathlib.Path(os.environ["OPENSSL_ROOT_DIR"]) / "bin/openssl", "version"]),
        "python_packages": capture([sys.executable, "-m", "pip", "freeze"]).splitlines(),
        "macos_leak_sanitizer": "UNSUPPORTED" if sys.platform == "darwin" else "not_applicable",
        "bounds": {"header": 1024, "payload": 65536, "token": 100000, "kid": 128, "signature": 64,
                   "fuzz_runs": 20000, "fuzz_seconds": 60, "fuzz_rss_mb": 1024, "process_seconds": 15}}
    save(EVIDENCE / "provenance.json", manifest)


def candidate():
    backend("candidate", WORK / "candidate-src")
    build = WORK / "original-build"
    run("original-configure", ["cmake", "-S", FIXTURE / "original_tests", "-B", build,
        *cmake_options(WORK / "original-installed"), f"-DCANDIDATE_SOURCE={WORK / 'candidate-src'}",
        f"-DORIGINAL_SOURCE={WORK / 'upstream-src'}", "-DCJOSE_BUILD_SHARED=ON", "-DCJOSE_BUILD_STATIC=ON", "-DCJOSE_MACOS_DYLIB=ON"])
    run("original-build", ["cmake", "--build", build, "--target", "check_original", "-j4"])
    suite("candidate-original-tests", build / "check_original", WORK / "upstream-src/test")


def historical():
    # Historical lane is diagnostic, not the acceptance base. Never rewrite it.
    source = WORK / "historical-src"
    build = source
    run("historical-autoreconf", ["autoreconf", "-fi"], cwd=source)
    build.mkdir(exist_ok=True)
    env = dict(os.environ, PKG_CONFIG_PATH=f"{WORK / 'deps/lib/pkgconfig'}:{os.environ.get('PKG_CONFIG_PATH', '')}")
    linker = macos_linker_flag()
    run("historical-configure", [source / "configure", f"--prefix={WORK / 'historical-installed'}",
        f"--with-jansson={WORK / 'deps'}", f"--with-openssl={os.environ['OPENSSL_ROOT_DIR']}",
        "--disable-doxygen-doc", f"LDFLAGS={linker + ' ' if linker else ''}-Wl,-rpath,{WORK / 'deps/lib'} -Wl,-rpath,{os.environ['OPENSSL_ROOT_DIR']}/lib"], cwd=build, env=env)
    run("historical-build", ["make", "-j4", "all", "check"], cwd=build, env=env, required=False)
    run("historical-install", ["make", "-j4", "install"], cwd=build, env=env)
    if not (build / "test/check_cjose").is_file():
        raise RuntimeError("missing_historical_tests")
    suite("historical-tests", build / "test/check_cjose", source / "test")


def legacy_abi():
    result = {"status": "FAILED", "reason": "prerequisites", "runs": [],
              "historical_sha": PINS["historical_sha"], "compiled_against": "0.6.2.8"}
    target = EVIDENCE / "legacy-abi-result.json"
    save(target, result)
    prefix, build = WORK / "historical-installed", WORK / "legacy-abi-build"
    library_name = "libcjose.dylib" if sys.platform == "darwin" else "libcjose.so"
    if not (prefix / "include/cjose/version.h").is_file() or not (prefix / "lib" / library_name).is_file():
        result["reason"] = "missing_historical_install"
        save(target, result)
        raise RuntimeError("missing_historical_install")
    result["reason"] = "configure"
    save(target, result)
    run("legacy-abi-configure", ["cmake", "-S", FIXTURE / "legacy_abi", "-B", build,
        *cmake_options(prefix), f"-DHISTORICAL_PREFIX={prefix}"])
    # The only compile/link step: both later executions use this exact binary.
    result["reason"] = "compile_and_link"
    save(target, result)
    run("legacy-abi-build", ["cmake", "--build", build, "--target", "cjose_legacy_consumer", "-j4"])
    binary = build / "cjose_legacy_consumer"
    binary_hash = hashlib.sha256(binary.read_bytes()).hexdigest()
    result.update(reason="execution", binary_sha256=binary_hash, compile_count=1)
    save(target, result)
    passed = True
    for name, version in (("historical", "0.6.2.8"), ("candidate", "0.8.1")):
        selected = WORK / (name + "-installed")
        library = selected / "lib" / library_name
        if not library.is_file():
            result["runs"].append({"name": name, "status": "FAILED", "reason": "missing_shared_library"})
            passed = False
            continue
        # Never mutate os.environ or the runner/global loader environment.
        env = {key: value for key, value in os.environ.items() if not key.startswith(("LD_", "DYLD_"))}
        variable = "DYLD_LIBRARY_PATH" if sys.platform == "darwin" else "LD_LIBRARY_PATH"
        env[variable] = os.pathsep.join(map(str, (selected / "lib", WORK / "deps/lib", pathlib.Path(os.environ["OPENSSL_ROOT_DIR"]) / "lib")))
        label = "legacy-abi-" + name
        code = run(label, [binary, library.resolve(), version], env=env, timeout=15, required=False)
        log = (RAW / (label + ".log")).read_text(errors="replace")
        expected = {"status": "PASS", "header_version": "0.6.2.8", "runtime_version": version,
                    "loaded_expected_library": True, "legacy_constants_checked": 33, "rfc_import_verify_export": True}
        # Loader diagnostics stay raw/local; upload only a fixed-schema result.
        try:
            reported = json.loads(log)
        except (ValueError, TypeError):
            reported = None
        unchanged = hashlib.sha256(binary.read_bytes()).hexdigest() == binary_hash
        ok = code == 0 and reported == expected and unchanged
        item = {"name": name, "status": "PASS" if ok else "FAILED", "exit": code,
                "expected_runtime_version": version, "loaded_expected_library": bool(ok),
                "binary_unchanged": unchanged, "binary_sha256": binary_hash,
                "library_file": library.resolve().name, "library_sha256": hashlib.sha256(library.read_bytes()).hexdigest()}
        if isinstance(reported, dict) and reported.get("stage") in (
            "arguments", "historical_headers", "loaded_library", "runtime_version", "legacy_constants",
            "legacy_header_api", "rfc_import_verify_export"):
            item["failed_stage"] = reported["stage"]
        result["runs"].append(item)
        passed = passed and ok
        save(target, result)
    result.update(status="PASS" if passed and len(result["runs"]) == 2 else "FAILED", reason="completed")
    save(target, result)
    if result["status"] != "PASS":
        raise RuntimeError("legacy_abi_not_proved")


def feature_detection():
    all_passed = True
    for name, expected, fallback in (("candidate", True, False), ("upstream", False, False), ("candidate", True, True)):
        prefix = WORK / (name + "-installed")
        label = name + "-feature-detection" + ("-fallback" if fallback else "")
        code = run(label, ["cmake", "-S", FIXTURE / "feature_detection", "-B", WORK / (label + "-build"),
            *cmake_options(prefix), f"-Dcjose_DIR={prefix / 'lib/cmake/cjose'}",
            f"-DEXPECT_EXTENSION={'ON' if expected else 'OFF'}",
            f"-DEXPECT_JANSSON_FALLBACK={'ON' if fallback else 'OFF'}",
            f"-DCMAKE_DISABLE_FIND_PACKAGE_jansson={'TRUE' if fallback else 'FALSE'}",
            f"-DQUALIFICATION_DEPENDENCY_PREFIX={WORK / 'deps'}",
            f"-DFORGE_CMAKE_MODULES={ROOT / 'cmake'}"], required=False)
        log = (RAW / (label + ".log")).read_text(errors="replace")
        # Both cases configure successfully after a genuine dependency control.
        # The upstream case proves a defined false extension result, not a
        # missing package/compiler/Jansson or an unrelated configure failure.
        cache = WORK / (label + "-build/CMakeCache.txt")
        cache_text = cache.read_text() if cache.exists() else ""
        control = re.search(r"(?m)^FORGE_CJOSE_HAS_QUALIFIED_DEPENDENCIES:[^=]+=(.*)$", cache_text)
        extension = re.search(r"(?m)^FORGE_CJOSE_HAS_EXTERNAL_SIGNING:[^=]+=(.*)$", cache_text)
        truthy = {"1", "ON", "TRUE", "YES"}
        control_passed = control is not None and control[1].strip().upper() in truthy
        extension_passed = extension is not None and extension[1].strip().upper() in truthy
        passed = (code == 0 and "QUALIFICATION_FEATURE_RESULT_PASS" in log and control_passed and
                  extension is not None and extension_passed == expected)
        save(EVIDENCE / (label + "-result.json"), {"status": "PASS" if passed else "FAILED",
             "expected_configure_success": True, "expected_extension": expected, "configure_exit": code,
             "jansson_fallback": fallback,
             "dependency_control": control_passed, "extension_probe_defined": extension is not None,
             "extension_available": extension_passed, "pass_marker": "QUALIFICATION_FEATURE_RESULT_PASS" in log})
        all_passed = all_passed and passed
    if not all_passed:
        raise RuntimeError("feature_detection")


def consumers(sanitize=False):
    name = "sanitized" if sanitize else "candidate"
    if sanitize:
        backend(name, WORK / "candidate-src", True)
    else:
        feature_detection()
    for linkage in ("shared", "static"):
        label = name + "-" + linkage
        build, prefix = WORK / (label + "-consumer"), WORK / (name + "-installed")
        run(label + "-configure", ["cmake", "-S", FIXTURE, "-B", build, *cmake_options(prefix, sanitize),
            f"-Dcjose_DIR={prefix / 'lib/cmake/cjose'}", f"-DCJOSE_QUALIFICATION_LINKAGE={linkage}",
            f"-DCJOSE_QUALIFICATION_SANITIZERS={'ON' if sanitize else 'OFF'}",
            f"-DCJOSE_QUALIFICATION_FUZZ={'ON' if sanitize and linkage == 'static' else 'OFF'}"])
        run(label + "-build", ["cmake", "--build", build, "-j4"])
        run(label + "-qualify", [sys.executable, FIXTURE / "run.py", "--probe", build / "cjose_public_probe",
            "--work", WORK / (label + "-cases"), "--evidence", EVIDENCE / (label + "-cases.json")], timeout=360)
        run(label + "-linkage", [sys.executable, FIXTURE / "linkage.py", "--binary", build / "cjose_public_probe",
            "--prefix", prefix, "--deps", WORK / "deps", "--linkage", linkage,
            "--evidence", EVIDENCE / (label + "-linkage-tree.json")])
        if sanitize and linkage == "static":
            corpus = WORK / (label + "-cases/corpus")
            fuzz_code = run("bounded-fuzz", [build / "cjose_profile_fuzz", corpus, "-runs=20000", "-max_total_time=60",
                "-max_len=100001", "-rss_limit_mb=1024", "-timeout=2", f"-artifact_prefix={RAW}/"], timeout=90, required=False)
            log = (RAW / "bounded-fuzz.log").read_text(errors="replace")
            final = re.search(r"#(\d+)\s+DONE\s+cov: (\d+) ft: (\d+).*?rss: (\d+)Mb", log)
            save(EVIDENCE / "bounded-fuzz-metrics.json", {"status": "PASS" if final and fuzz_code == 0 else "FAILED" if fuzz_code else "HARNESS_ERROR",
                 "runs": int(final[1]) if final else None, "coverage_edges": int(final[2]) if final else None,
                 "features": int(final[3]) if final else None, "peak_rss_mb": int(final[4]) if final else None})
            # These are explicitly synthetic/public test inputs, not runtime
            # credentials or logs. The fuzzer never receives a private key.
            destination = EVIDENCE / "synthetic-fuzz-corpus"
            destination.mkdir(exist_ok=True)
            manifest = []
            for path in sorted(corpus.iterdir()):
                if not path.is_file() or not re.fullmatch(r"[A-Za-z0-9_-]+", path.name):
                    raise RuntimeError("invalid_corpus_file")
                value = path.read_bytes()
                if len(value) > 100001:
                    raise RuntimeError("unbounded_corpus_file")
                digest = hashlib.sha256(value).hexdigest()
                (destination / digest).write_bytes(value)
                manifest.append({"sha256": digest, "size": len(value)})
            save(EVIDENCE / "synthetic-fuzz-corpus-manifest.json", {"status": "PASS" if manifest else "HARNESS_ERROR",
                 "data_class": "synthetic_public_test_inputs_only", "files": manifest})
            if fuzz_code:
                raise RuntimeError("bounded_fuzz_failed")


def evidence():
    required = ["upstream-tests-summary", "candidate-tests-summary", "candidate-original-tests-summary",
        "candidate-feature-detection-result", "upstream-feature-detection-result", "candidate-feature-detection-fallback-result", "candidate-shared-qualify", "candidate-static-qualify",
        "sanitized-tests-summary", "sanitized-shared-qualify", "sanitized-static-qualify", "bounded-fuzz",
        "candidate-shared-linkage", "candidate-static-linkage", "sanitized-shared-linkage", "sanitized-static-linkage", "bounded-fuzz-metrics",
        "synthetic-fuzz-corpus-manifest", "legacy-abi-result"]
    if sys.platform == "darwin":
        required.append("macos-toolchain")
    gates = {}
    for name in required:
        path = EVIDENCE / (name + ".json")
        gates[name] = json.loads(path.read_text()).get("status", "HARNESS_ERROR") if path.exists() else "NOT_RUN"
    base_file, candidate_file = EVIDENCE / "upstream-tests-summary.json", EVIDENCE / "candidate-original-tests-summary.json"
    if base_file.exists() and candidate_file.exists():
        base, patched = json.loads(base_file.read_text()), json.loads(candidate_file.read_text())
        save(EVIDENCE / "q1-comparison.json", {"baseline_checks": base["checks"], "candidate_checks": patched["checks"],
            "baseline_failures": base["failure_ids"], "candidate_failures": patched["failure_ids"],
            "new_failures": sorted(set(patched["failure_ids"]) - set(base["failure_ids"])),
            "same_check_count": base["checks"] == patched["checks"],
            "same_test_ids": base["test_ids"] == patched["test_ids"]})
        if not base["test_ids"] or base["checks"] != patched["checks"] or base["test_ids"] != patched["test_ids"]:
            gates["same-original-tests"] = "HARNESS_ERROR"
    run("select-static-diagnostics", [sys.executable, ROOT / ".github/scripts/cjose-qualification-diagnostics.py",
        "--work", WORK, "--root", ROOT])
    historical_path = EVIDENCE / "historical-tests-summary.json"
    save(EVIDENCE / "summary.json", {"required": gates, "Q7": "COORDINATOR_OWNED",
        "historical_diagnostic": json.loads(historical_path.read_text()) if historical_path.exists() else {"status": "NOT_RUN"},
        "status": "PASS" if all(x == "PASS" for x in gates.values()) else "INCOMPLETE_OR_FAILED"})
    return all(x == "PASS" for x in gates.values())


def main():
    if OPTIONS.phase == "prepare":
        prepare()
    elif OPTIONS.phase == "baseline":
        # Both states execute even if one test suite fails; evidence still fails.
        failed = False
        try:
            backend("upstream", WORK / "upstream-src")
        except (RuntimeError, subprocess.SubprocessError, OSError):
            failed = True
        try:
            historical()
        except (RuntimeError, subprocess.SubprocessError, OSError):
            save(EVIDENCE / "historical-diagnostic.json", {"status": "INCOMPLETE_OR_FAILED", "acceptance_base": False})
        if failed:
            return 1
    elif OPTIONS.phase == "candidate":
        candidate()
    elif OPTIONS.phase == "legacy-abi":
        legacy_abi()
    elif OPTIONS.phase in ("consumers", "sanitizers"):
        consumers(OPTIONS.phase == "sanitizers")
    else:
        return 0 if evidence() else 1
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, subprocess.SubprocessError, OSError):
        # Never forward third-party exception text, commands, token or key bytes.
        print("qualification phase failed; inspect sanitized evidence", file=sys.stderr)
        sys.exit(1)
