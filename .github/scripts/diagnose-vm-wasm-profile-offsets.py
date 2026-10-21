#!/usr/bin/env python3
"""Finite CI67 profile diagnostic. No production repair or alternate workload."""
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import signal
import stat
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zipfile

HEAD = "0504095b77e306429192793ffbae1393ef23f82c"
TREE = "31c53e7c4b3dd4ad8ce3fdb75620e051ae95dd85"
PARENT = "72a9e5aeaba390389aa35bbdc0fe7fc02fc968ab"
WAMR = "da5892640f9168df5a097d096e6a3924192d6f86"
KEY = "07a659677fa4c4d84a4856fd75acd714df78dcd67ff839ad67f6edbf913d82aa"
OUTER = "034aad5d1de2337ff9f7a966f2230e53f13095a21f0f967604cd2ed03c867fd4"
INNER = "c6955fb6e6ca2b1d249f7ad784c839e8478d82c7ef6cd5db35f7b1e701e804df"
PRE = "aafa189cb65cca0cb9a5c858d0d2aabc98c06f191fc01026e6def2562db10f47"
POST = "e20260d9e09bdec5594130c2cbe067038e0fd979718ace2a2bb5ceb3a378c437"
PATCH_SHA = "9a222c48eaca8ab7e8629add54077640b44e6e2db10d360d8168a583a9d46bbc"
TEST_FILE = "tests/vm_wasm_execution/profile_public_tests.cpp"
TEST = "test_forge_vm_wasm_profile_public"
PROBE = ".github/probes/vm-wasm-profile-offsets-diagnostic"
CANCEL_SIGNAL = 0
RECIPE_FILES = {
    ".github/scripts/setup-vm-wasm-toolchain.sh", ".github/scripts/check-vm-wasm-toolchain.py",
    ".github/scripts/check-llvm-cxx-abi.py", ".github/workflows/vm-wasm.yml",
    *{".github/probes/llvm-cxx-abi/" + n for n in (
        "CMakeLists.txt", "bootstrap_abi.cpp", "bootstrap_abi.hxx", "bootstrap_abi_main.cpp",
        "intel-llvm-z3.patch", "main.cpp", "test_toolchain.py")},
}


def need(ok, message):
    if not ok:
        raise RuntimeError("HARNESS_ERROR: " + message)


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def save(path, value):
    with Path(path).open("x") as stream:
        json.dump(value, stream, indent=2)
        stream.write("\n")


def git(*args):
    return subprocess.check_output(["git", "--no-replace-objects", "-c", "core.fsmonitor=false",
                                    "-c", "core.untrackedCache=false", "-C", str(source), *args])


def source_check(label, dirty):
    need(git("rev-parse", "HEAD").decode().strip() == HEAD, "runtime HEAD")
    raw = git("cat-file", "commit", HEAD)
    headers = raw.split(b"\n\n", 1)[0].splitlines()
    need(hashlib.sha1(b"commit " + str(len(raw)).encode() + b"\0" + raw).hexdigest() == HEAD,
         "raw commit object digest")
    need([v[5:].decode() for v in headers if v.startswith(b"tree ")] == [TREE] and
         [v[7:].decode() for v in headers if v.startswith(b"parent ")] == [PARENT], "tree/ordered parent")
    (evidence / (label + ".commit")).write_bytes(raw)
    status = git("status", "--porcelain=v1", "--untracked-files=all").decode()
    need(status == (" M " + TEST_FILE + "\n" if dirty else ""), "exact source dirty boundary")
    need(not git("diff", "--cached", "--name-only"), "staged runtime source")
    need(sha(source / TEST_FILE) == (POST if dirty else PRE), "test pre/postimage")
    submodules = git("submodule", "status", "--recursive").decode()
    need(not any(v.startswith(("-", "+", "U")) for v in submodules.splitlines()), "vendor pin state")
    need(subprocess.check_output(["git", "-C", str(source / "vendor/wamr"), "rev-parse", "HEAD"]).decode().strip()
         == WAMR, "WAMR pin")
    git("submodule", "foreach", "--quiet", "--recursive",
        'test -z "$(git -c core.fsmonitor=false status --porcelain --untracked-files=all)"')
    diff = git("diff", "--no-ext-diff", "--no-textconv", "--binary")
    (evidence / (label + ".diff")).write_bytes(diff)
    record = {"head": HEAD, "tree": TREE, "parents": [PARENT], "status": status,
              "test_sha256": sha(source / TEST_FILE), "submodules": submodules,
              "dirty_test_diagnostic_not_clean_head": dirty,
              "diff_sha256": hashlib.sha256(diff).hexdigest()}
    save(evidence / (label + ".json"), record)
    return record


def clean_env():
    forbidden = {"CTEST_TESTS_TO_RUN", "CTEST_TEST_INCLUDE_FILE", "CFLAGS", "CPPFLAGS", "C_INCLUDE_PATH",
                 "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH", "LIBRARY_PATH", "COMPILER_PATH", "GCC_EXEC_PREFIX",
                 "CMAKE_TOOLCHAIN_FILE"}
    for name, value in os.environ.items():
        need(not value or not (name.startswith(("BOOST_TEST_", "GTEST_", "CATCH_", "LD_")) or name in
             forbidden), "ambient test/compiler/loader control: " + name)
    os.environ.pop("CPATH", None)


def qualified_env():
    llvm = root / "llvm"
    need(os.environ["LLVM_PREFIX"] == str(llvm) and os.environ["CC"] == str(llvm / "bin/clang") and
         os.environ["CXX"] == str(llvm / "bin/clang++") and os.environ["CXXFLAGS"] == "-stdlib=libc++",
         "qualified consumer compiler/flags")
    lib = Path((evidence / "toolchain/llvm-abi/libcxx-path.txt").read_text().strip())
    need(lib.is_absolute() and lib.resolve().is_relative_to(llvm.resolve()) and lib.is_file(), "qualified libc++")
    need(os.environ["LDFLAGS"] == f"-stdlib=libc++ -L{lib.parent} -Wl,-rpath,{lib.parent}", "qualified link flags")
    abi = json.loads((evidence / "toolchain/llvm-abi/result.json").read_text())
    headers = json.loads((evidence / "toolchain/header-result.json").read_text())
    need(abi["state"] == headers["state"] == "READY" and abi["LLVM"] == headers["clang"] == "22.1.8" and
         abi["RTTI"] == "YES" and headers["libcxx"] == 220108, "real ABI/header preflight receipts")


def group_alive(pid):
    try:
        os.killpg(pid, 0)
        return True
    except ProcessLookupError:
        return False


def capture(name, argv, timeout=7200, image=None, env=None):
    """One owned group; even an exited leader does not retire live descendants."""
    record = {"argv": [str(v) for v in argv], "cwd": str(source), "timeout": timeout}
    need(not CANCEL_SIGNAL, "command refused after cancellation")
    log = evidence / (name + ".log")
    process = None
    try:
        with log.open("xb") as output:
            process = subprocess.Popen(record["argv"], cwd=source, env=env, stdout=output,
                                       stderr=subprocess.STDOUT, start_new_session=True)
            record["pid"] = record["owned_pgid"] = process.pid
            record["owned_pgids"] = [process.pid]
            deadline = time.monotonic() + timeout
            # Observe only direct CTest children until the exact image is found; never rerun it.
            while process.poll() is None:
                if CANCEL_SIGNAL or time.monotonic() >= deadline:
                    record["cancel_signal"] = CANCEL_SIGNAL
                    record["timed_out"] = not CANCEL_SIGNAL
                    break
                if image and "runtime_child" not in record:
                    children = Path(f"/proc/{process.pid}/task/{process.pid}/children")
                    try:
                        for child in children.read_text().split():
                            proc = Path("/proc") / child
                            if Path(os.readlink(proc / "exe")) == image.resolve():
                                pgid, sid = os.getpgid(int(child)), os.getsid(int(child))
                                need(pgid in (process.pid, int(child)) and sid in (process.pid, int(child)),
                                     "unexpected direct-child group/session ownership")
                                record["runtime_child"] = {"pid": int(child), "exe": str(image.resolve()),
                                    "cmdline": (proc / "cmdline").read_bytes().decode().split("\0")[:-1],
                                    "stat": (proc / "stat").read_text(), "pgid": pgid, "sid": sid}
                                if pgid not in record["owned_pgids"]:
                                    record["owned_pgids"].insert(0, pgid)
                                break
                    except (FileNotFoundError, ProcessLookupError):
                        pass
                try:
                    process.wait(timeout=min(0.05 if image else 0.2, max(0.001, deadline - time.monotonic())))
                except subprocess.TimeoutExpired:
                    pass
    finally:
        if process:
            for pgid in record["owned_pgids"]:
                if group_alive(pgid):
                    record["group_cleanup_required"] = True
                    for sig in (signal.SIGTERM, signal.SIGKILL):
                        try:
                            os.killpg(pgid, sig)
                        except ProcessLookupError:
                            break
                        until = time.monotonic() + 10
                        while group_alive(pgid) and time.monotonic() < until:
                            process.poll()
                            time.sleep(0.1)
                        if not group_alive(pgid):
                            break
            record["exit"] = process.wait(timeout=10)
            record["groups_gone"] = {str(pgid): not group_alive(pgid) for pgid in record["owned_pgids"]}
            record["group_gone"] = all(record["groups_gone"].values())
            record.setdefault("cancel_signal", CANCEL_SIGNAL)
        record["log_sha256"] = sha(log) if log.exists() else None
        save(evidence / (name + ".json"), record)
    need(record.get("group_gone") and not record.get("timed_out") and not record.get("cancel_signal") and
         not record.get("group_cleanup_required"), "command timeout/retained process group: " + name)
    return record


def checked(name, argv, timeout=7200, env=None):
    result = capture(name, argv, timeout, env=env)
    need(result["exit"] == 0, "command failed: " + name)


def trust():
    base = "https://api.github.com/repos/SystasisIO/forge"

    class NoRedirect(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, *args, **kwargs):
            return None

    api = urllib.request.build_opener(NoRedirect)

    def request(path):
        return urllib.request.Request(base + path, headers={"Authorization": "Bearer " + os.environ["GH_TOKEN"],
            "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"})

    def metadata(path, name):
        with api.open(request(path), timeout=60) as response:
            data = response.read(4 * 1024 * 1024 + 1)
        need(len(data) <= 4 * 1024 * 1024, "API metadata bound")
        (evidence / name).write_bytes(data)
        return json.loads(data)

    run = metadata("/actions/runs/39507960049", "trusted-run.json")
    need(all(run.get(k) == v for k, v in {"id": 39507960049, "head_sha": HEAD, "workflow_id": 312612190,
         "path": ".github/workflows/vm-wasm.yml", "event": "workflow_dispatch", "run_attempt": 1,
         "status": "completed", "conclusion": "failure"}.items()), "trusted failed-run identity")
    for field in ("repository", "head_repository"):
        need(run[field]["id"] == 1215380442 and run[field]["full_name"] == "SystasisIO/forge", "repository")
    commit = metadata("/git/commits/" + HEAD, "trusted-commit.json")
    need(commit["sha"] == HEAD and commit["tree"]["sha"] == TREE and
         [p["sha"] for p in commit["parents"]] == [PARENT], "API raw050 tree/parent")
    job = metadata("/actions/jobs/118921561978", "trusted-producer-job.json")
    need(all(job.get(k) == v for k, v in {"id": 118921561978, "run_id": 39507960049, "head_sha": HEAD,
         "name": "ubuntu-24.04 / dedicated LLVM22.1.8 libc++ source bootstrap", "status": "completed",
         "conclusion": "success"}.items()), "successful producer identity")
    for name in ("Explicit source build and relocated ABI qualification (-j2, one linker)",
                 "Publish only the qualified immutable toolchain"):
        need(any(s["name"] == name and s["status"] == "completed" and s["conclusion"] == "success"
                 for s in job["steps"]), "producer qualification step")
    artifact = metadata("/actions/artifacts/12310936206", "trusted-artifact.json")
    need(artifact["id"] == 12310936206 and not artifact["expired"] and artifact["size_in_bytes"] == 281295387 and
         artifact["name"] == "llvm22.1.8-linux-x86-" + KEY and artifact["digest"] == "sha256:" + OUTER,
         "artifact identity/size/digest")
    need(all(artifact["workflow_run"].get(k) == v for k, v in {"id": 39507960049, "head_sha": HEAD,
         "repository_id": 1215380442, "head_repository_id": 1215380442}.items()), "artifact run binding")

    try:
        api.open(request("/actions/artifacts/12310936206/zip"), timeout=60)
        raise RuntimeError("HARNESS_ERROR: expected signed download redirect")
    except urllib.error.HTTPError as response:
        need(response.code == 302, "artifact API redirect status")
        location = response.headers["Location"]
    url = urllib.parse.urlsplit(location)
    need(url.scheme == "https" and url.hostname and not url.username and not url.password and
         url.port in (None, 443) and url.hostname.endswith((".blob.core.windows.net", ".actions.githubusercontent.com")),
         "signed artifact host/port")
    save(evidence / "signed-download-origin.json", {"host": url.hostname, "port": url.port or 443,
         "authenticated_API_redirects": "REFUSED", "signed_download_redirects": "REFUSED"})
    archive = root / "trusted-artifact.zip"
    try:
        # No GitHub Authorization header is forwarded or persisted with this signed URL.
        with urllib.request.build_opener(NoRedirect).open(location, timeout=120) as response, archive.open("xb") as output:
            count = 0
            while block := response.read(1024 * 1024):
                count += len(block)
                need(count <= 281295387, "outer archive bound")
                output.write(block)
    except urllib.error.URLError:
        raise RuntimeError("HARNESS_ERROR: signed artifact download failed") from None
    need(count == 281295387 and sha(archive) == OUTER, "outer SHA/size")
    bundle = root / "bundle"
    bundle.mkdir()
    with zipfile.ZipFile(archive) as zipped:
        entries = zipped.infolist()
        need(len(entries) == 3 and {e.filename for e in entries} ==
             {"toolchain.tar.gz", "recipe.json", "archive.sha256"}, "exact ZIP member set")
        for entry in entries:
            need(not entry.is_dir() and not stat.S_ISLNK(entry.external_attr >> 16), "ZIP entry type")
            need(entry.file_size <= (1024 ** 3 if entry.filename == "toolchain.tar.gz" else 4 * 1024 ** 2),
                 "ZIP member bound")
            with zipped.open(entry) as src, (bundle / entry.filename).open("xb") as dst:
                shutil.copyfileobj(src, dst, 1024 * 1024)
    need(sha(bundle / "toolchain.tar.gz") == INNER and
         (bundle / "archive.sha256").read_text().strip() == INNER, "inner SHA/checksum record")
    recipe = json.loads((bundle / "recipe.json").read_text())
    need(recipe["key"] == KEY and set(recipe["recipe_files"]) == RECIPE_FILES, "exact eleven recipe inputs")
    unsigned = {k: v for k, v in recipe.items() if k != "key"}
    need(hashlib.sha256(json.dumps(unsigned, sort_keys=True).encode()).hexdigest() == KEY, "recipe content key")
    for name, digest in recipe["recipe_files"].items():
        path = PurePosixPath(name)
        need(not path.is_absolute() and ".." not in path.parts and sha(source / path) == digest, "recipe source: " + name)
    (evidence / "recipe.json").write_bytes((bundle / "recipe.json").read_bytes())
    save(evidence / "trusted-digests.json", {"outer": OUTER, "inner": INNER, "key": KEY,
         "run_conclusion": "failure", "producer_conclusion": "success"})


def dependencies():
    brew = shutil.which("brew")
    if not brew:
        installer = root / "homebrew-install.sh"
        with urllib.request.urlopen("https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh", timeout=120) as r:
            data = r.read(4 * 1024 * 1024 + 1)
        need(len(data) <= 4 * 1024 * 1024, "Brew installer bound")
        installer.write_bytes(data)
        save(evidence / "brew-installer.json", {"sha256": sha(installer)})
        checked("brew-bootstrap", ["/bin/bash", installer], env=dict(os.environ, NONINTERACTIVE="1"))
        brew = "/home/linuxbrew/.linuxbrew/bin/brew"
    packages = ["boost", "cmake", "gmp", "libngtcp2", "ninja", "openssl@3"]
    checked("brew-install", [brew, "install", *packages])
    checked("brew-info", [brew, "info", "--json=v2", *packages], 120)
    prefix = lambda name: subprocess.check_output([brew, "--prefix", name], timeout=60).decode().strip()
    brew_root = subprocess.check_output([brew, "--prefix"], timeout=60).decode().strip()
    checked("glaze", ["bash", source / ".github/scripts/install-glaze.sh", Path(os.environ["RUNNER_TEMP"]) / "forge-glaze"])
    with open(os.environ["GITHUB_PATH"], "a") as stream:
        stream.write(str(Path(brew).parent) + "\n" + os.environ["LLVM_PREFIX"] + "/bin\n")
    with open(os.environ["GITHUB_ENV"], "a") as stream:
        stream.write("OPENSSL_PREFIX=" + prefix("openssl@3") + "\nCMAKE_PREFIX_PATH=" + ";".join([
            os.environ["RUNNER_TEMP"] + "/forge-glaze", brew_root, prefix("boost"), prefix("libngtcp2"), prefix("openssl@3")]) + "\n")


def instrument():
    patch = transport / PROBE / "profile-diagnostic.patch"
    need(sha(patch) == PATCH_SHA, "frozen patch digest")
    checked("patch-check", ["git", "apply", "--check", patch], 120)
    checked("patch-apply", ["git", "apply", patch], 120)
    need(sha(source / TEST_FILE) == POST, "exact instrumented postimage")


def configure():
    need(not build.exists(), "fresh build destination")
    llvm = Path(os.environ["LLVM_PREFIX"])
    need(llvm == root / "llvm", "fresh qualified consumer prefix, not Stage A or an ambient LLVM")
    need(subprocess.check_output(["cmake", "--version"]).decode().splitlines()[0] == "cmake version 3.31.6", "CMake")
    need(subprocess.check_output([llvm / "bin/llvm-config", "--version"]).decode().strip() == "22.1.8", "LLVM version")
    need(subprocess.check_output([llvm / "bin/llvm-config", "--has-rtti"]).decode().strip() == "YES", "LLVM RTTI")
    need(os.environ["CC"] == str(llvm / "bin/clang") and os.environ["CXX"] == str(llvm / "bin/clang++"), "compilers")
    ssl = os.environ["OPENSSL_PREFIX"]
    checked("configure", ["cmake", "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
        "-DCMAKE_C_COMPILER=" + os.environ["CC"], "-DCMAKE_CXX_COMPILER=" + os.environ["CXX"],
        "-DCMAKE_PREFIX_PATH=" + os.environ["CMAKE_PREFIX_PATH"], "-DOPENSSL_ROOT_DIR=" + ssl,
        "-DOPENSSL_INCLUDE_DIR=" + ssl + "/include", "-DOPENSSL_SSL_LIBRARY=" + ssl + "/lib/libssl.so.3",
        "-DOPENSSL_CRYPTO_LIBRARY=" + ssl + "/lib/libcrypto.so.3", "-DFORGE_ENABLE_MODULES=ON",
        "-DFORGE_ENABLE_ROCKSDB=OFF", "-DFORGE_ENABLE_TUI=OFF", "-DFORGE_ENABLE_TOOLING=OFF",
        "-DFORGE_ENABLE_VM_WASM_LLVM_JIT=ON", "-DFORGE_ENABLE_VM_WASM_OPTIMIZING=ON",
        "-DFORGE_ENABLE_WASM_COMPILER=ON", "-DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON",
        "-DFORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=OFF", "-DBUILD_TESTING=ON",
        "-DFORGE_WAMR_COMPILER_TEST_AS_BYTES=1099511627776", "-DLLVM_DIR=" + str(llvm / "lib/cmake/llvm"),
        "-DClang_DIR=" + str(llvm / "lib/cmake/clang")])


def provider_inventory(paths):
    records = []
    for path in sorted(paths):
        target = path.resolve(strict=True)
        need(target.is_file(), "resolved provider is not a file: " + str(path))
        records.append({"ldd_path": str(path), "resolved_path": str(target),
                        "bytes": target.stat().st_size, "sha256": sha(target)})
    return records


def profile():
    registry = json.loads(subprocess.check_output(["ctest", "--test-dir", build, "-R", "^" + TEST + "$", "--show-only=json-v1"], timeout=120))
    save(evidence / "profile-registry.json", registry)
    need(len(registry["tests"]) == 1, "one canonical CTest")
    entry = registry["tests"][0]
    image = build / "tests/vm_wasm_execution" / TEST
    props = {p["name"]: p["value"] for p in entry["properties"]}
    need(entry["name"] == TEST and entry["command"] == [str(image), "--catch_system_errors=no"] and
         props["TIMEOUT"] == 120 and props["RUN_SERIAL"] is True, "canonical argv/limits")
    need(not {"DISABLED", "WILL_FAIL", "SKIP_RETURN_CODE", "SKIP_REGULAR_EXPRESSION", "ENVIRONMENT"} & props.keys(), "test masking/environment")
    before = sha(image)
    checked("profile-elf", ["readelf", "-W", "-h", "-d", image], 120)
    checked("profile-ldd", ["ldd", image], 120)
    paths = set()
    for line in (evidence / "profile-ldd.log").read_text().splitlines():
        match = re.fullmatch(r"\s*(?:\S+\s+=>\s+)?(/.+?)\s+\(0x[0-9a-fA-F]+\)\s*", line)
        if match:
            paths.add(Path(match[1]))
        else:
            need(re.fullmatch(r"\s*linux-vdso\S*\s+\(0x[0-9a-fA-F]+\)\s*", line) is not None,
                 "unresolved/unrecognized ldd provider row: " + line)
    need(paths, "no absolute ldd provider images")
    providers_before = provider_inventory(paths)
    save(evidence / "profile-providers-before.json", providers_before)
    try:
        result = capture("profile-ctest", ["ctest", "--test-dir", build, "-R", "^" + TEST + "$",
            "--output-on-failure", "--no-tests=error", "-j", "1", "--timeout", "120",
            "--output-log", evidence / "profile-output.log", "--output-junit", evidence / "profile-ctest.xml"], 140, image)
    finally:
        providers_after = provider_inventory(paths)
        save(evidence / "profile-providers-after.json", {"providers": providers_after,
             "unchanged": providers_before == providers_after})
        need(providers_before == providers_after, "selected ldd provider images changed")
    need(before == sha(image), "profile executable changed")
    need(result.get("runtime_child", {}).get("cmdline") == entry["command"], "actual canonical child PID/image")
    save(evidence / "profile-image.json", {"path": str(image), "sha256_before": before, "sha256_after": sha(image),
         "runtime_child": result["runtime_child"], "ctest_exit": result["exit"],
         "provider_count": len(providers_before), "provider_images_unchanged": True})
    shutil.copyfile(build / "Testing/Temporary/LastTest.log", evidence / "profile-LastTest.log")
    text = (evidence / "profile-LastTest.log").read_text()
    need(re.findall(r"Running (\d+) test cases", text) == ["10"], "ten unfiltered Boost cases")
    cases = list(ET.parse(evidence / "profile-ctest.xml").getroot().iter("testcase"))
    need(len(cases) == 1 and cases[0].get("name") == TEST and cases[0].find("skipped") is None, "one unskipped JUnit case")
    return result["exit"]


def diagnostic_records():
    log = evidence / "profile-LastTest.log"
    if not log.exists():
        save(evidence / "diagnostic-records.json", {"status": "NOT_OBSERVED_NO_COMPLETED_PROFILE_LOG"})
        return
    populations = {"first.mix": 0, "first.other": 1, "second.mix": 0, "shared.mix": 0,
                   "initial.mix": 0, "retained.mix": 0, "post_cancel.other": 1}
    blocks, current = [], None
    for line in log.read_text().splitlines():
        if not line.startswith("PROFILE_DIAG_V1 "):
            continue
        kind, payload = line[len("PROFILE_DIAG_V1 "):].split(" ", 1)
        if kind == "begin":
            match = re.fullmatch(r"case=(?P<case>.+?) population=(?P<population>\S+) function=(?P<function>\d+) "
                r"count=(?P<count>\d+) capacity=(?P<capacity>\d+) pinned_pc=(?P<pinned_pc>\d+) "
                r"wasm_bytes=(?P<wasm_bytes>\d+) code_section=(?P<code_section>\d+)", payload)
            need(match is not None, "frozen diagnostic begin grammar")
            fields = match.groupdict()
            need(current is None and fields["population"] in populations, "diagnostic block start")
            current = {"begin": fields, "samples": [], "ranges": []}
        else:
            fields = dict(token.split("=", 1) for token in payload.split())
            need(current is not None, "diagnostic record outside block")
            if kind == "sample":
                current["samples"].append({k: int(v) for k, v in fields.items()})
            elif kind == "control_range":
                current["ranges"].append({k: int(v) for k, v in fields.items()})
            elif kind != "end":
                need(kind not in current, "duplicate diagnostic record")
                current[kind] = fields
            else:
                begin = current["begin"]
                pop, count = begin["population"], int(begin["count"])
                need(fields["population"] == pop and int(begin["function"]) == populations[pop] and
                     int(begin["capacity"]) == 8192 and 0 <= count <= 8192, "population/function/count")
                wasm = bytes.fromhex(current["fixture"]["wasm_hex"])
                need(len(wasm) == int(begin["wasm_bytes"]), "actual fixture bytes")
                samples = current["samples"]
                need(sum(r["count"] for r in samples) == count and all(r["executed"] == 1 and r["count"] > 0
                     for r in samples), "existing sample histogram frequencies")
                summary = current["sample_summary"]
                known = [r for r in samples if r["offset"] != 4294967295]
                mapped = sum(r["count"] for r in known)
                need(int(summary["count"]) == count and int(summary["mapped"]) == mapped and
                     int(summary["unknown"]) == count - mapped and
                     int(summary["distinct_pcs"]) == len({r["pc"] for r in samples}) and
                     int(summary["distinct_offsets"]) == len({r["offset"] for r in known}), "sample summary")
                control = current["control_summary"]
                need(int(control["executed"]) == 0 and int(control["queries"]) <= 65536 and
                     int(control.get("truncated", "0")) == 0, "bounded complete control output")
                if known:
                    window = {k: int(v) for k, v in current["control_window"].items()}
                    need(window["executed"] == 0 and window["query_budget"] == 65536 and
                         window["anchor_pc"] in {r["pc"] for r in known} and
                         0 <= window["begin"] <= window["anchor_pc"] <= window["end_inclusive"] < 2 ** 64 and
                         int(control["queries"]) == window["end_inclusive"] - window["begin"] + 1,
                         "control window around a real sampled PC")
                    cursor = window["begin"]
                    need(len(current["ranges"]) == int(control["ranges"]) == int(control["emitted"]) <= 256,
                         "complete control ranges")
                    for row in current["ranges"]:
                        need(row["executed"] == 0 and row["begin"] == cursor and row["end_inclusive"] >= cursor,
                             "lookup-only contiguous inclusive range")
                        cursor = row["end_inclusive"] + 1
                    need(cursor == window["end_inclusive"] + 1, "control window coverage")
                else:
                    need(int(control["queries"]) == 0, "no invented query anchor")
                (evidence / ("fixture-" + pop + ".wasm")).write_bytes(wasm)
                current["fixture_sha256"] = hashlib.sha256(wasm).hexdigest()
                blocks.append(current)
                current = None
    need(current is None and len({b["begin"]["population"] for b in blocks}) == len(blocks), "complete unique blocks")
    reached = {b["begin"]["population"] for b in blocks}
    need({"first.mix", "shared.mix", "initial.mix", "retained.mix", "post_cancel.other"} <= reached,
         "missing causal population; native diagnosis incomplete")
    save(evidence / "diagnostic-records.json", {"status": "COMPLETE_OBSERVATIONS_NOT_CAUSAL_VERDICT",
         "blocks": blocks, "boundary": "Queried ranges are not executed samples or a complete DWARF map."})


def collect():
    diagnostic_complete = True
    try:
        instrumented = evidence / "instrument-receipt.json"
        if instrumented.exists():
            proof = json.loads(instrumented.read_text()).get("source_after", {})
            need(proof.get("test_sha256") == POST and proof.get("dirty_test_diagnostic_not_clean_head") is True,
                 "instrumentation completion/source proof")
        else:
            need(not any((evidence / (p + "-receipt.json")).exists() for p in ("configure", "build", "profile")),
                 "later phase without instrumentation proof")
        source_check("collect-final", instrumented.exists())
    except Exception as error:
        save(evidence / "collect-source-incomplete.json", {"status": "HOLD_SOURCE_NOT_VERIFIED", "error": str(error)})
        diagnostic_complete = False
    try:
        diagnostic_records()
    except Exception as error:
        save(evidence / "diagnostic-incomplete.json", {"status": "HOLD_NOT_PRODUCT_RECLASSIFICATION", "error": str(error)})
        diagnostic_complete = False
    upload = root / "upload"
    upload.mkdir()
    selected = set(evidence.rglob("*"))
    selected.update(build / n for n in ("CMakeCache.txt", "CMakeFiles/CMakeConfigureLog.yaml", "Testing/Temporary/LastTest.log"))
    selected.update(transport / n for n in (
        ".github/scripts/diagnose-vm-wasm-profile-offsets.py", ".github/workflows/diagnose-vm-wasm-profile-offsets.yml",
        PROBE + "/profile-diagnostic.patch", PROBE + "/README.md"))
    records, copied, incomplete = [], 0, False
    for path in sorted(selected):
        if not path.is_file() or path.is_symlink():
            continue
        relative = Path("run") / path.relative_to(root) if path.is_relative_to(root) else Path("transport") / path.relative_to(transport)
        size = path.stat().st_size
        member_complete = size <= 64 * 1024 ** 2 and copied + size <= 512 * 1024 ** 2
        records.append({"path": str(path), "bytes": size, "sha256": sha(path), "upload_path": str(relative), "complete": member_complete})
        if member_complete:
            target = upload / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            copied += size
        else:
            incomplete = True
    save(upload / "evidence-freeze.json", {"records": records, "incomplete": incomplete,
         "diagnostic_complete": diagnostic_complete,
         "runtime_head": HEAD, "tree": TREE, "wamr": WAMR, "test_preimage": PRE, "test_postimage": POST,
         "transport_head": os.environ["GITHUB_SHA"], "boundary": "One dirty-test diagnostic; not clean-head or CI67 acceptance."})
    need(not incomplete and diagnostic_complete, "diagnostic/upload incomplete; raw files retained")


def main():
    global root, source, transport, evidence, build
    need(len(sys.argv) == 2 and sys.argv[1] in {"trust", "dependencies", "instrument", "configure", "build", "profile", "collect"}, "fixed phase")
    phase = sys.argv[1]
    need(os.environ["RUNNER_OS"] == "Linux" and os.uname().machine == "x86_64", "Linux x86_64")
    need(os.environ["GITHUB_REPOSITORY"] == "SystasisIO/forge" and os.environ["GITHUB_EVENT_NAME"] == "push" and
         os.environ["GITHUB_REF"] == "refs/heads/vm-wasm-profile-offsets-diagnostic-v1", "dedicated diagnostic ref")
    event = json.loads(Path(os.environ["GITHUB_EVENT_PATH"]).read_text())
    need(event.get("deleted") is False and event.get("after") == os.environ["GITHUB_SHA"], "non-deletion transport push")
    transport = Path(os.environ["GITHUB_WORKSPACE"])
    need(subprocess.check_output(["git", "-C", str(transport), "rev-parse", "HEAD"]).decode().strip()
         == os.environ["GITHUB_SHA"], "transport HEAD")
    subprocess.run(["git", "-C", str(transport), "diff", "--exit-code", "--quiet"], check=True)
    subprocess.run(["git", "-C", str(transport), "diff", "--cached", "--exit-code", "--quiet"], check=True)
    source = Path(os.environ["DIAG_SOURCE"])
    root = Path(os.environ["DIAG_ROOT"])
    need(source == transport / "baseline-source" and root == Path(os.environ["RUNNER_TEMP"]) /
         ("vm-wasm-profile-offsets-" + os.environ["GITHUB_RUN_ID"] + "-" + os.environ["GITHUB_RUN_ATTEMPT"]), "owned roots")
    evidence, build = root / "evidence", root / "build"
    if phase == "trust":
        root.mkdir()
        evidence.mkdir()
    elif phase == "collect":
        evidence.mkdir(parents=True, exist_ok=True)
        collect()
        return 0
    clean_env()
    need(os.environ["CMAKE_BUILD_PARALLEL_LEVEL"] == "2" and os.environ["CTEST_PARALLEL_LEVEL"] == "1",
         "unchanged j2/j1 environment")
    if phase == "trust":
        need(not any(os.environ.get(k) for k in ("CC", "CXX", "CXXFLAGS", "LDFLAGS", "LLVM_PREFIX")),
             "ambient pre-consumption toolchain")
    else:
        qualified_env()
    receipt = {"phase": phase, "transport_head": os.environ["GITHUB_SHA"], "status": "FAIL_NOT_ACCEPTANCE"}
    exit_code = 1
    try:
        receipt["source_before"] = source_check(phase + "-before", phase in {"configure", "build", "profile"})
        if phase == "build":
            checked("build-profile", ["cmake", "--build", build, "--target", TEST, "--parallel", "2"])
        else:
            exit_code = {"trust": trust, "dependencies": dependencies, "instrument": instrument,
                         "configure": configure, "profile": profile}[phase]() or 0
        if phase == "build":
            exit_code = 0
        receipt["exit"] = exit_code
        receipt["status"] = "PASS_SCOPED_DIAGNOSTIC_NOT_ACCEPTANCE" if exit_code == 0 else "RUNTIME_RED_PRESERVED"
    finally:
        try:
            receipt["source_after"] = source_check(phase + "-after", phase in {"instrument", "configure", "build", "profile"})
        finally:
            save(evidence / (phase + "-receipt.json"), receipt)
    return exit_code


if __name__ == "__main__":
    def interrupted(number, _frame):
        # Defer cancellation until ownership is established; never interrupt Popen assignment or group join.
        global CANCEL_SIGNAL
        if not CANCEL_SIGNAL:
            CANCEL_SIGNAL = number

    signal.signal(signal.SIGTERM, interrupted)
    signal.signal(signal.SIGINT, interrupted)
    try:
        sys.exit(main())
    except Exception as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
