#!/usr/bin/env bash
# Manual diagnostic only. No runtime, recipe, loader-path or mandatory-CI repair.
set -euo pipefail
trap 'echo "HARNESS_ERROR: Linux diagnostic phase failed: ${phase:-startup}" >&2' ERR
phase="${1:?fixed diagnostic phase required}"
source_root="${DIAG_SOURCE:?}"
root="${DIAG_ROOT:?}"
test "$RUNNER_OS:$(uname -m)" = Linux:x86_64
test "$GITHUB_REPOSITORY" = SystasisIO/forge
test "$GITHUB_EVENT_NAME" = push
test "$GITHUB_REF" = refs/heads/vm-wasm-linux-diagnostics-v1
python3 - "$GITHUB_EVENT_PATH" <<'PY'
import json, sys
with open(sys.argv[1]) as stream:
    event = json.load(stream)
if not isinstance(event, dict) or event.get("deleted") is not False:
    raise SystemExit("HARNESS_ERROR: a non-deletion push event is required")
PY
test "$source_root" = "$GITHUB_WORKSPACE/baseline-source"
test "$root" = "$RUNNER_TEMP/vm-wasm-linux-diagnostic-$GITHUB_RUN_ID-$GITHUB_RUN_ATTEMPT"
evidence="$root/evidence"
build="$root/build"
leaf="$build/forge-package-vm-wasm-optimizing-component"
head=830b4cad6bf00dde0519cb7cd33d231a40bbeb43
tree=141465234fdfd5ac2f1905e3eb0b6b7bc5391213
mkdir -p "$evidence"

source_check() {
   test "$(git -C "$source_root" rev-parse HEAD)" = "$head"
   test "$(git -C "$source_root" rev-parse 'HEAD^{tree}')" = "$tree"
   test -z "$(git -C "$source_root" status --porcelain --untracked-files=no)"
   test "$(git -C "$source_root/vendor/wamr" rev-parse HEAD)" = da5892640f9168df5a097d096e6a3924192d6f86
   git -C "$source_root" submodule status --recursive > "$evidence/submodules-$phase.txt"
   ! grep -qE '^[-+U]' "$evidence/submodules-$phase.txt"
   git -C "$source_root" submodule foreach --quiet --recursive \
      'test -z "$(git status --porcelain --untracked-files=no)"'
}

clean_test_env() {
   local name
   for name in $(compgen -e); do
      case "$name" in
         BOOST_TEST_*|GTEST_*|CATCH_*|CTEST_TESTS_TO_RUN|CTEST_TEST_INCLUDE_FILE|\
         LD_*)
            test -z "${!name}" || { echo "HARNESS_ERROR: ambient test/loader control: $name" >&2; return 2; } ;;
      esac
   done
   unset CPATH
}

# A failed gate is recorded and returned. Callers explicitly continue other independent gates.
capture() {
   local name="$1" expected="$2" rc=0
   shift 2
   printf '%q ' "$@" > "$evidence/$name.argv"
   printf '\n' >> "$evidence/$name.argv"
   "$@" > "$evidence/$name.log" 2>&1 || rc=$?
   printf '%s\n' "$rc" > "$evidence/$name.exit"
   cat "$evidence/$name.log"
   test "$rc" = "$expected"
}

case "$phase" in
trust)
   source_check
   test ! -e "$root/bundle"
   python3 - "$root" "$source_root" <<'PY'
import hashlib, json, os, pathlib, stat, sys, urllib.error, urllib.parse, urllib.request, zipfile
root, source = map(pathlib.Path, sys.argv[1:])
out = root / "evidence"
head = "830b4cad6bf00dde0519cb7cd33d231a40bbeb43"
key = "cbd0880dd7325bae781da9da1ae851d7f7c42aa36d11fb28506b1f86f4d48583"
outer = "eca6aa7420526ab4082f2a8ef1f81df34bb2cd06a3b818185198f0f34af99469"
inner = "425f4cd6173b6130425419d0e608f6551c0fb4322a076b7618f850228438c9c3"
base = "https://api.github.com/repos/SystasisIO/forge"
def require(ok, message):
    if not ok:
        raise SystemExit("HARNESS_ERROR: " + message)
def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
def request(path):
    return urllib.request.Request(base + path, headers={
        "Authorization": "Bearer " + os.environ["GH_TOKEN"],
        "Accept": "application/vnd.github+json", "X-GitHub-Api-Version": "2022-11-28"})
def metadata(path, name):
    with urllib.request.urlopen(request(path), timeout=60) as response:
        data = response.read(4 * 1024 * 1024 + 1)
    require(len(data) <= 4 * 1024 * 1024, "API metadata exceeds bound")
    (out / name).write_bytes(data)
    return json.loads(data)
run = metadata("/actions/runs/39406169819", "trusted-run.json")
require(all(run.get(k) == v for k, v in {
    "id": 39406169819, "head_sha": head, "workflow_id": 312612190,
    "path": ".github/workflows/vm-wasm.yml", "event": "pull_request",
    "run_attempt": 1, "status": "completed", "conclusion": "failure"}.items()), "trusted run identity")
for field in ("repository", "head_repository"):
    require(run[field]["id"] == 1215380442 and run[field]["full_name"] == "SystasisIO/forge", "trusted repository")
commit = metadata("/git/commits/" + head, "trusted-commit.json")
require(commit["sha"] == head and commit["tree"]["sha"] == "141465234fdfd5ac2f1905e3eb0b6b7bc5391213", "trusted tree")
job = metadata("/actions/jobs/118572518399", "trusted-producer-job.json")
require(all(job.get(k) == v for k, v in {
    "id": 118572518399, "run_id": 39406169819, "head_sha": head,
    "name": "ubuntu-24.04 / dedicated LLVM22.1.8 libc++ source bootstrap",
    "status": "completed", "conclusion": "success"}.items()), "trusted producer job")
for name in ("Explicit source build and relocated ABI qualification (-j2, one linker)",
             "Publish only the qualified immutable toolchain"):
    require(any(s["name"] == name and s["status"] == "completed" and s["conclusion"] == "success"
                for s in job["steps"]), "producer qualification step")
artifact = metadata("/actions/artifacts/12266644506", "trusted-artifact.json")
require(artifact["id"] == 12266644506 and not artifact["expired"] and
        artifact["name"] == "llvm22.1.8-linux-x86-" + key and
        artifact["size_in_bytes"] == 281290282 and artifact["digest"] == "sha256:" + outer,
        "trusted artifact identity/digest")
require(all(artifact["workflow_run"].get(k) == v for k, v in {
    "id": 39406169819, "head_sha": head, "repository_id": 1215380442,
    "head_repository_id": 1215380442}.items()), "artifact workflow binding")
# Obtain the signed redirect without ever forwarding the GitHub token to blob storage.
class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None
try:
    urllib.request.build_opener(NoRedirect).open(request("/actions/artifacts/12266644506/zip"), timeout=60)
    raise SystemExit("HARNESS_ERROR: expected artifact API redirect")
except urllib.error.HTTPError as response:
    require(response.code == 302, "artifact API download status")
    location = response.headers["Location"]
url = urllib.parse.urlsplit(location)
require(url.scheme == "https" and url.hostname and not url.username and not url.password, "signed artifact URL")
archive = root / "trusted-artifact.zip"
with urllib.request.urlopen(location, timeout=120) as response, archive.open("xb") as stream:
    count = 0
    while block := response.read(1024 * 1024):
        count += len(block)
        require(count <= 281290282, "artifact archive exceeds bound")
        stream.write(block)
require(count == 281290282 and sha(archive) == outer, "outer artifact SHA/size")
bundle = root / "bundle"
bundle.mkdir()
with zipfile.ZipFile(archive) as zipped:
    entries = zipped.infolist()
    require(len(entries) == 3 and {e.filename for e in entries} ==
            {"toolchain.tar.gz", "recipe.json", "archive.sha256"}, "artifact member set")
    for entry in entries:
        require(not entry.is_dir() and not stat.S_ISLNK(entry.external_attr >> 16), "artifact entry type")
        limit = 1024 * 1024 * 1024 if entry.filename == "toolchain.tar.gz" else 4 * 1024 * 1024
        require(entry.file_size <= limit, "artifact member exceeds bound")
        with zipped.open(entry) as src, (bundle / entry.filename).open("xb") as dest:
            while block := src.read(1024 * 1024):
                dest.write(block)
require(sha(bundle / "toolchain.tar.gz") == inner, "inner archive SHA")
require((bundle / "archive.sha256").read_text().strip() == inner, "producer checksum record")
recipe = json.loads((bundle / "recipe.json").read_text())
require(recipe["key"] == key, "recipe key")
unsigned = {k: v for k, v in recipe.items() if k != "key"}
require(hashlib.sha256(json.dumps(unsigned, sort_keys=True).encode()).hexdigest() == key, "recipe content key")
for name, digest in recipe["recipe_files"].items():
    path = pathlib.PurePosixPath(name)
    require(not path.is_absolute() and ".." not in path.parts, "recipe source path")
    require(sha(source / path) == digest, "baseline recipe input mismatch: " + name)
(out / "recipe.json").write_bytes((bundle / "recipe.json").read_bytes())
(out / "trusted-digests.json").write_text(json.dumps({"outer": outer, "inner": inner, "key": key}, indent=2) + "\n")
PY
   ;;
dependencies)
   source_check
   clean_test_env
   if ! command -v brew >/dev/null 2>&1; then
      curl --fail --location --max-time 120 https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh \
         --output "$root/homebrew-install.sh"
      sha256sum "$root/homebrew-install.sh" > "$evidence/homebrew-install.sha256"
      capture brew-bootstrap 0 env NONINTERACTIVE=1 /bin/bash "$root/homebrew-install.sh"
      eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"
      echo /home/linuxbrew/.linuxbrew/bin >> "$GITHUB_PATH"
   fi
   capture brew-install 0 brew install boost cmake gmp libngtcp2 ninja openssl@3
   brew info --json=v2 boost cmake gmp libngtcp2 ninja openssl@3 > "$evidence/brew-dependencies.json"
   echo "OPENSSL_PREFIX=$(brew --prefix openssl@3)" >> "$GITHUB_ENV"
   capture glaze 0 "$source_root/.github/scripts/install-glaze.sh" "$RUNNER_TEMP/forge-glaze"
   echo "$LLVM_PREFIX/bin" >> "$GITHUB_PATH"
   echo "CMAKE_PREFIX_PATH=$RUNNER_TEMP/forge-glaze;$(brew --prefix);$(brew --prefix boost);$(brew --prefix libngtcp2);$(brew --prefix openssl@3)" >> "$GITHUB_ENV"
   ;;
configure)
   source_check
   clean_test_env
   test ! -e "$build"
   test "$(cmake --version | head -n1)" = 'cmake version 3.31.6'
   test "$("$LLVM_PREFIX/bin/llvm-config" --version)" = 22.1.8
   test "$("$LLVM_PREFIX/bin/llvm-config" --has-rtti)" = YES
   test "$CC" = "$LLVM_PREFIX/bin/clang"
   test "$CXX" = "$LLVM_PREFIX/bin/clang++"
   capture configure 0 cmake -S "$source_root" -B "$build" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
      -DCMAKE_PREFIX_PATH="$CMAKE_PREFIX_PATH" -DOPENSSL_ROOT_DIR="$OPENSSL_PREFIX" \
      -DOPENSSL_INCLUDE_DIR="$OPENSSL_PREFIX/include" \
      -DOPENSSL_SSL_LIBRARY="$OPENSSL_PREFIX/lib/libssl.so.3" \
      -DOPENSSL_CRYPTO_LIBRARY="$OPENSSL_PREFIX/lib/libcrypto.so.3" \
      -DFORGE_ENABLE_MODULES=ON -DFORGE_ENABLE_ROCKSDB=OFF -DFORGE_ENABLE_TUI=OFF \
      -DFORGE_ENABLE_TOOLING=OFF -DFORGE_ENABLE_VM_WASM_LLVM_JIT=ON \
      -DFORGE_ENABLE_VM_WASM_OPTIMIZING=ON -DFORGE_ENABLE_WASM_COMPILER=ON \
      -DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON -DFORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=OFF \
      -DBUILD_TESTING=ON -DFORGE_WAMR_COMPILER_TEST_AS_BYTES=1099511627776 \
      -DLLVM_DIR="$LLVM_PREFIX/lib/cmake/llvm" -DClang_DIR="$LLVM_PREFIX/lib/cmake/clang"
   ;;
build)
   source_check
   clean_test_env
   capture build 0 cmake --build "$build" --target forge wasm_compiler test_forge_vm_wasm_profile_public --parallel 2
   capture install 0 cmake --build "$build" --target test_forge_package_install --parallel 2
   capture helper-commands 0 ninja -C "$build" -t commands wasm_compiler
   source_check
   ;;
baseline)
   source_check
   clean_test_env
   result=0
   for role in build installed; do
      helper="$build/tools/wasm-compiler/forge-wasm-compiler"
      if [[ "$role" == installed ]]; then helper="$build/forge-package-install/bin/forge-wasm-compiler"; fi
      sha256sum "$helper" > "$evidence/helper-$role.sha256"
      capture "helper-$role-elf" 0 readelf -W -h -l -d "$helper" || result=1
      capture "helper-$role-ldd" 0 env -i LANG=C LC_ALL=C TZ=UTC /usr/bin/ldd "$helper" || result=1
      capture "helper-$role-entry" 2 env -i LANG=C LC_ALL=C TZ=UTC "$helper" </dev/null || result=1
   done
   ctest --test-dir "$build" -R '^(test_forge_vm_wasm_profile_public|test_forge_package_vm_wasm_optimizing_component)$' \
      --show-only=json-v1 > "$evidence/root-registry.json"
   python3 - "$evidence/root-registry.json" <<'PY'
import json, sys
tests = json.load(open(sys.argv[1]))["tests"]
assert {t["name"] for t in tests} == {"test_forge_vm_wasm_profile_public", "test_forge_package_vm_wasm_optimizing_component"}
assert len(tests) == 2 and all(t.get("command") for t in tests)
PY
   capture profile-baseline 0 ctest --test-dir "$build" -R '^test_forge_vm_wasm_profile_public$' \
      --parallel 1 --no-tests=error --output-on-failure \
      --output-log "$evidence/profile-ctest.log" --output-junit "$evidence/profile-ctest.xml" || result=1
   cp "$build/Testing/Temporary/LastTest.log" "$evidence/profile-LastTest.log"
   grep -q 'Running 10 test cases' "$evidence/profile-LastTest.log" || result=1
   capture package-baseline 0 ctest --test-dir "$build" -R '^test_forge_package_vm_wasm_optimizing_component$' \
      --parallel 1 --no-tests=error --output-on-failure \
      --output-log "$evidence/package-ctest.log" --output-junit "$evidence/package-ctest.xml" || result=1
   cp "$build/Testing/Temporary/LastTest.log" "$evidence/package-LastTest.log"
   ctest --test-dir "$leaf" --show-only=json-v1 > "$evidence/package-registry.json"
   python3 - "$evidence/package-registry.json" <<'PY'
import json, sys
tests = json.load(open(sys.argv[1]))["tests"]
assert len(tests) == 5 and {t["name"] for t in tests} == {
    "package_optimizing_export_names_compile", "package_optimizing_export_names_runtime",
    "package_optimizing_session_wasm", "package_optimizing_session_compile", "package_optimizing_session"}
assert all(t.get("command") for t in tests)
PY
   source_check
   exit "$result"
   ;;
observe)
   source_check
   clean_test_env
   result=0
   # Relocated FINAL-toolchain comparison, not the retired Stage A compiler/runtime identity.
   capture unwind-probe 0 cmake --debug-trycompile \
      -S "$GITHUB_WORKSPACE/.github/probes/vm-wasm-linux-diagnostic" -B "$root/unwind-probe" -G Ninja \
      -DCMAKE_C_COMPILER="$LLVM_PREFIX/bin/clang" -DCMAKE_CXX_COMPILER="$LLVM_PREFIX/bin/clang++" \
      -DFORGE_DIAGNOSTIC_TOOLCHAIN_PREFIX="$LLVM_PREFIX" || result=1
   capture helper-installed-observer 2 env -i LANG=C LC_ALL=C TZ=UTC LD_DEBUG=bindings \
      LD_DEBUG_OUTPUT="$evidence/helper-bindings" \
      "$build/forge-package-install/bin/forge-wasm-compiler" </dev/null || result=1
   python3 - "$build" "$evidence" <<'PY' || result=1
import hashlib, json, os, pathlib, re, signal, subprocess, sys, time
build, out = map(pathlib.Path, sys.argv[1:])
registry = json.loads(subprocess.check_output(["ctest", "--test-dir", str(build), "-R",
    "^test_forge_vm_wasm_profile_public$", "--show-only=json-v1"]))["tests"]
assert len(registry) == 1
test = registry[0]
props = {p["name"]: p["value"] for p in test["properties"]}
argv = test["command"]
assert argv == [str(build / "tests/vm_wasm_execution/test_forge_vm_wasm_profile_public"), "--catch_system_errors=no"]
assert props["TIMEOUT"] == 120
cwd = props["WORKING_DIRECTORY"]
def sha(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
binary = pathlib.Path(argv[0])
ldd = subprocess.run(["ldd", str(binary)], capture_output=True, text=True)
(out / "observer-ldd.log").write_text(ldd.stdout + ldd.stderr)
assert ldd.returncode == 0
providers = {pathlib.Path(p).resolve() for p in re.findall(r"(?:=>\s+|^\s*)(/[^\s]+)", ldd.stdout, re.M)}
providers = sorted(p for p in providers if re.search(r"lib(?:LLVM|unwind|gcc_s|c\+\+|c\+\+abi)", p.name))
before = {str(p): sha(p) for p in [binary, *providers]}
for i, path in enumerate(providers):
    audit = subprocess.run(["readelf", "-W", "-d", "--dyn-syms", "--version-info", str(path)], capture_output=True)
    (out / f"observer-provider-{i}.txt").write_bytes(audit.stdout + audit.stderr)
    assert audit.returncode == 0
env = dict(os.environ, LD_DEBUG="bindings", LD_DEBUG_OUTPUT=str(out / "profile-bindings"))
record = {"argv": argv, "cwd": cwd, "timeout": 120, "image_and_providers_before": before,
          "boundary": "fresh unchanged-830b rebuild, NOT original CI65 executable; LD_DEBUG observer only"}
with (out / "profile-observer.log").open("wb") as log:
    process = subprocess.Popen(argv, cwd=cwd, env=env, stdout=log, stderr=subprocess.STDOUT,
                               start_new_session=True)
    record["pid"] = process.pid
    try:
        record["exit"] = process.wait(timeout=120)
    except subprocess.TimeoutExpired:
        record["timed_out"] = True
        try:
            os.killpg(process.pid, signal.SIGTERM)
        except ProcessLookupError:
            pass
        # Escalation belongs to the group even if the leader exits first.
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            try:
                os.killpg(process.pid, 0)
            except ProcessLookupError:
                break
            time.sleep(0.1)
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
        record["exit"] = process.wait(timeout=10)
record["image_and_providers_after"] = {str(p): sha(p) for p in [binary, *providers]}
record["trace_files"] = [{"path": str(p), "sha256": sha(p), "size": p.stat().st_size}
                         for p in sorted(out.glob("profile-bindings.*"))]
(out / "profile-observer.json").write_text(json.dumps(record, indent=2) + "\n")
assert record["image_and_providers_after"] == before
assert record["trace_files"], "no actual loader trace; bindings NOT_OBSERVED"
sys.exit(0 if record["exit"] == 0 and not record.get("timed_out") else 1)
PY
   source_check
   exit "$result"
   ;;
collect)
   # Copy only evidence and finite metadata/fixtures, never a whole build or toolchain tree.
   python3 - "$root" "$source_root" "$GITHUB_WORKSPACE" <<'PY'
import hashlib, json, pathlib, re, shutil, sys
root, source, overlay = map(pathlib.Path, sys.argv[1:])
upload = root / "upload"
upload.mkdir(exist_ok=False)
selected = set((root / "evidence").rglob("*"))
for name in ("CMakeCache.txt", "cmake_install.cmake", "tools/wasm-compiler/cmake_install.cmake",
             "tests/ForgePackageConsumerCache.cmake", "tests/ForgePackageToolchainCache.cmake",
             "CMakeFiles/CMakeConfigureLog.yaml", "tools/wasm-compiler/CMakeFiles/wasm_compiler.dir/link.txt"):
    selected.add(root / "build" / name)
for folder in (root / "unwind-probe", root / "build/forge-package-vm-wasm-optimizing-component"):
    for path in folder.rglob("*"):
        if path.suffix in {".txt", ".cmake", ".json", ".xml", ".log", ".yaml", ".c", ".linkmap", ".wasm", ".abi", ".aot"}:
            selected.add(path)
selected.update((root / "build/forge-package-install").rglob("*.cmake"))
for path in (overlay / ".github/scripts/diagnose-vm-wasm-linux.sh",
             overlay / ".github/workflows/diagnose-vm-wasm-linux.yml",
             overlay / ".github/probes/vm-wasm-linux-diagnostic/CMakeLists.txt",
             overlay / ".github/probes/vm-wasm-linux-diagnostic/README.md"):
    selected.add(path)
records, copied, refused = [], 0, False
for path in sorted(selected):
    if not path.is_file() or path.is_symlink():
        continue
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    rel = pathlib.Path("evidence") / path.relative_to(root) if path.is_relative_to(root) else pathlib.Path("diagnostic-source") / path.relative_to(overlay)
    size = path.stat().st_size
    complete = size <= 64 * 1024 * 1024 and copied + size <= 512 * 1024 * 1024
    records.append({"path": str(path), "sha256": digest.hexdigest(), "size": size,
                    "upload_path": str(rel) if complete else None, "complete": complete})
    if complete:
        dest = upload / rel
        dest.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(path, dest)
        copied += size
    else:
        refused = True
dependencies = {}
for log in (root / "evidence").glob("helper-*-ldd.log"):
    for name in re.findall(r"(?:=>\s+|^\s*)(/[^\s]+)", log.read_text(), re.M):
        path = pathlib.Path(name).resolve()
        if path.is_file():
            digest = hashlib.sha256()
            with path.open("rb") as stream:
                for block in iter(lambda: stream.read(1024 * 1024), b""):
                    digest.update(block)
            dependencies[str(path)] = digest.hexdigest()
(upload / "manifest.json").write_text(json.dumps({"files": records, "copied_bytes": copied,
    "helper_ldd_provider_sha256": dependencies, "complete": not refused,
    "boundary": "manual diagnostic; execution statuses are separate .exit files; ldd is not symbol-binding proof"}, indent=2) + "\n")
if refused:
    raise SystemExit("HARNESS_ERROR: evidence exceeds finite upload cap; omitted raw files explicitly recorded")
PY
   ;;
*) echo 'HARNESS_ERROR: unknown diagnostic phase' >&2; exit 2 ;;
esac
