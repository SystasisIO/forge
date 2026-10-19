#!/usr/bin/env bash
set -euo pipefail
umask 077
ulimit -c 0

readonly phase="${1:?usage: attestation.sh <dependencies|build|boundary|test|benchmark|interop|packages|guest|sanitizers|evidence>}"
case "$phase" in dependencies|build|boundary|test|benchmark|interop|packages|guest|sanitizers|evidence) ;; *) exit 2 ;; esac
readonly root="$(git rev-parse --show-toplevel)"
readonly work="${RUNNER_TEMP:?}/forge-attestation"
readonly build="$work/build"
readonly deps="$work/deps"
readonly glaze="$work/glaze"
mkdir -p "$work/evidence" "$work/raw"
trap 'printf "%s\t%d\n" "$phase" "$?" >> "$work/evidence/phases.tsv"' EXIT

if [[ "$phase" == evidence ]]; then
   # Only build diagnostics and test result metadata leave the runner. Runtime
   # logs, generated credentials, private keys and caches stay private.
   python3 - "$work" <<'PY'
import json
import pathlib
import re
import shutil
import sys
import xml.etree.ElementTree as ET

work = pathlib.Path(sys.argv[1])
summary = {'tests': [], 'status': 'NOT_RUN'}
report = work / 'results.xml'
if report.is_file():
    try:
        cases = ET.parse(report).getroot().findall('.//testcase')
        for case in cases:
            name = case.get('name', '')
            if not re.fullmatch(r'test_forge_[a-z0-9_]+', name):
                raise ValueError('unexpected case')
            summary['tests'].append({'name': name, 'status': 'FAIL' if
                case.find('failure') is not None else ('NOT_RUN' if
                case.find('skipped') is not None else 'PASS')})
        summary['status'] = 'PASS' if cases and all(
            item['status'] == 'PASS' for item in summary['tests']) else 'FAIL'
    except Exception:
        summary['status'] = 'HARNESS_ERROR'
(work / 'evidence' / 'tests.json').write_text(json.dumps(summary, indent=2) + '\n')
# Preserve failure locations, never assertion operands or credential values.
locations = []
sanitizers = []
sanitizer_kinds = {'heap-use-after-free', 'heap-buffer-overflow', 'stack-buffer-overflow',
                   'stack-use-after-return', 'stack-use-after-scope', 'global-buffer-overflow',
                   'use-after-poison', 'double-free', 'alloc-dealloc-mismatch', 'SEGV', 'unknown-crash'}
test_log = work / 'raw' / 'tests.log'
if test_log.is_file():
    for line in test_log.read_text(errors='replace').splitlines():
        line = re.sub(r'\x1b\[[0-9;]*[A-Za-z]', '', line)
        found = re.search(r'([^/ ]+\.(?:cpp|cppm)|unknown location)\((\d+)\): (?:fatal error|error): in "([A-Za-z0-9_/]+)"', line)
        if found:
            locations.append({'source': found[1], 'line': int(found[2]), 'case': found[3]})
        found = re.search(r'ERROR: AddressSanitizer: ([A-Za-z0-9_-]+)', line)
        if found:
            sanitizers.append({'backend': 'ASan', 'kind': found[1] if found[1] in sanitizer_kinds else 'unknown'})
        if 'ERROR: LeakSanitizer: detected memory leaks' in line:
            sanitizers.append({'backend': 'LSan', 'kind': 'memory_leak'})
        found = re.search(r'([^/ ]+\.(?:cpp|cppm|c|h|hpp|hxx)):(\d+):\d+: runtime error:', line)
        if found:
            sanitizers.append({'backend': 'UBSan', 'source': found[1], 'line': int(found[2])})
(work / 'evidence' / 'failure-locations.json').write_text(json.dumps(locations, indent=2) + '\n')
(work / 'evidence' / 'sanitizers.json').write_text(json.dumps(sanitizers, indent=2) + '\n')
print(json.dumps({'tests': summary, 'failure_locations': locations, 'sanitizers': sanitizers}))
# Compile diagnostics contain no runtime values; cap retained output.
compile_log = work / 'raw' / 'compile.log'
if compile_log.is_file():
    lines = compile_log.read_text(errors='replace').splitlines()
    (work / 'evidence' / 'compile.txt').write_text('\n'.join(lines[-600:]) + '\n')
for name in ('build', 'package-consumer', 'interop', 'jansson-build', 'jansson-source',
             'llvm-project', 'contract-runtimes', 'contract-sdk', 'contract-sdk-prefix', 'guest'):
    path = work / name
    if path.is_dir():
        shutil.rmtree(path)
PY
   df -Pk "$work" > "$work/evidence/disk-after.txt"
   exit 0
fi

[[ "${ATTESTATION_SHA:?}" =~ ^[0-9a-f]{40}$ && "$(git rev-parse HEAD)" == "$ATTESTATION_SHA" ]]
export CMAKE_TOOLCHAIN_FILE="$work/compile-pools.cmake"
if [[ "$RUNNER_OS" == Linux ]]; then
   export CC=/usr/bin/clang-22 CXX=/usr/bin/clang++-22
   readonly brew_bin=/home/linuxbrew/.linuxbrew/bin/brew
else
   readonly brew_bin="$(command -v brew)"
fi

if [[ "$phase" == dependencies ]]; then
   df -Pk "$work" > "$work/evidence/disk-before.txt"
   if [[ "$RUNNER_OS" == Linux ]]; then
      sudo apt-get update
      sudo apt-get install -y clang-22 clang-tools-22 lld-22 llvm-22-dev libclang-22-dev \
         libclang-cpp22-dev g++-15 pkg-config libcurl4-openssl-dev libpcre2-dev
      if [[ ! -x "$brew_bin" ]]; then
         curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh -o "$work/homebrew.sh"
         NONINTERACTIVE=1 /bin/bash "$work/homebrew.sh"
      fi
   else
      "$brew_bin" install llvm@22 lld@22
   fi
   eval "$("$brew_bin" shellenv)"
   brew install boost cmake gmp libngtcp2 ninja openssl@3
   # Keep the requested -j4 budget; two compilers avoid runner OOM on Chain modules.
   printf '%s\n' 'set(CMAKE_JOB_POOLS "att_compile=2;att_link=1" CACHE STRING "CI RAM bound" FORCE)' \
      'set(CMAKE_JOB_POOL_COMPILE att_compile CACHE STRING "CI RAM bound" FORCE)' \
      'set(CMAKE_JOB_POOL_LINK att_link CACHE STRING "CI link bound" FORCE)' > "$CMAKE_TOOLCHAIN_FILE"
   bash "$root/.github/scripts/install-glaze.sh" "$glaze"
   git init --quiet "$work/jansson-source"
   git -C "$work/jansson-source" remote add origin https://github.com/akheron/jansson.git
   git -C "$work/jansson-source" fetch --quiet --depth=1 origin ed5cae4ed0621ef409510f94270c9f8f263736d0
   git -C "$work/jansson-source" checkout --quiet --detach FETCH_HEAD
   if [[ "$RUNNER_OS" != Linux ]]; then
      export CC="$(brew --prefix llvm@22)/bin/clang" CXX="$(brew --prefix llvm@22)/bin/clang++"
   fi
   cmake -S "$work/jansson-source" -B "$work/jansson-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_COMPILER="$CC" -DCMAKE_INSTALL_PREFIX="$deps" \
      -DCMAKE_INSTALL_LIBDIR=lib -DJANSSON_BUILD_SHARED_LIBS=ON -DJANSSON_BUILD_DOCS=OFF -DJANSSON_EXAMPLES=OFF
   cmake --build "$work/jansson-build" -j4
   cmake --install "$work/jansson-build"
   printf 'forge=%s\ncjose=%s\njansson=ed5cae4ed0621ef409510f94270c9f8f263736d0\n' \
      "$ATTESTATION_SHA" "$(git -C vendor/cjose rev-parse HEAD)" > "$work/evidence/provenance.txt"
   "$CC" --version >> "$work/evidence/provenance.txt"
   brew list --versions boost openssl@3 cmake ninja >> "$work/evidence/provenance.txt"
   exit 0
fi

eval "$("$brew_bin" shellenv)"
if [[ "$RUNNER_OS" != Linux ]]; then
   export CC="$(brew --prefix llvm@22)/bin/clang" CXX="$(brew --prefix llvm@22)/bin/clang++"
   export SDKROOT="$(xcrun --sdk macosx --show-sdk-path)"
   readonly llvm_root="$(brew --prefix llvm@22)"
   readonly wasm_ld="$(brew --prefix lld@22)/bin/wasm-ld"
else
   readonly llvm_root=/usr/lib/llvm-22
   readonly wasm_ld=/usr/lib/llvm-22/bin/wasm-ld
fi
readonly prefixes="$deps;$glaze;$(brew --prefix);$(brew --prefix boost);$(brew --prefix libngtcp2);$(brew --prefix openssl@3)"
export PKG_CONFIG_PATH="$deps/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig"
options=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
   -DCMAKE_PREFIX_PATH="$prefixes" -Dglaze_DIR="$glaze/share/glaze"
   -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DCMAKE_INSTALL_LIBDIR=lib
   -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
if [[ "$RUNNER_OS" == Linux ]]; then
   options+=(-DCMAKE_CXX_FLAGS=-stdlib=libstdc++)
else
   options+=(-DCMAKE_OSX_SYSROOT="$SDKROOT")
fi

case "$phase" in
   build)
      cmake -S "$root" -B "$build" "${options[@]}" -DFORGE_ENABLE_MODULES=ON \
         -DFORGE_ENABLE_JOSE=ON -DFORGE_ENABLE_LIBOAUTH2=ON -DFORGE_ENABLE_OIDC_AGENT=OFF \
         -DFORGE_ENABLE_MDBX=ON -DFORGE_ENABLE_ROCKSDB=OFF -DFORGE_ENABLE_TUI=OFF -DBUILD_TESTING=ON \
         -DFORGE_ENABLE_TOOLING=ON -DClang_DIR="$llvm_root/lib/cmake/clang" -DLLVM_DIR="$llvm_root/lib/cmake/llvm"
      cmake --build "$build" --target test_forge_crypto_jose test_forge_auth_attestation \
         test_forge_api_attestation test_forge_schema_attestation test_forge_json_strict test_forge_attestation_policy \
         forge_attestation_interop forge_attestation_benchmark -j4 2>&1 | tee "$work/raw/compile.log"
      ctest --test-dir "$build" --no-tests=error --output-on-failure -j1 --timeout 180 \
         --output-junit "$work/results.xml" \
         -R '^test_forge_(crypto_jose|auth_attestation|api_attestation|schema_attestation|json_strict|attestation_policy|attestation_emit|attestation_schema_export)$' \
         > "$work/raw/tests.log" 2>&1
      ;;
   boundary)
      # Reuse the same build tree after the independent early qualification.
      cmake --build "$build" --target \
         test_forge_schema test_forge_variant test_forge_codec_json test_forge_raw \
         test_forge_config_core test_forge_codec_yaml test_forge_codec_xml \
         test_forge_api_core test_forge_http_strict_json test_forge_auth_liboauth2 \
         test_forge_openapi_wire_baseline \
         forge_attestation_schema_fixture forge_attestation_boundary \
         -j4 2>&1 | tee -a "$work/raw/compile.log"
      ;;
   test)
      ctest --test-dir "$build" --no-tests=error --output-on-failure -j1 --timeout 180 \
         --output-junit "$work/results.xml" \
         -R '^test_forge_(crypto_jose|auth_attestation|api_attestation|schema_attestation|json_strict|attestation_policy|attestation_emit|attestation_schema_export|schema|variant|codec_json|raw|config_core|codec_yaml|codec_xml|api_core|http_strict_json|auth_liboauth2|openapi_wire_baseline|structure|contract_sdk_cmake_runtime_policy)$' \
         > "$work/raw/tests.log" 2>&1
      ;;
   benchmark)
      python3 - "$build/tests/forge_attestation_benchmark" "$work/evidence" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

executable, evidence = sys.argv[1], Path(sys.argv[2])
normal = subprocess.run([executable, '--iterations', '256', '--workers', '2', '--inflight', '4'],
                        capture_output=True, timeout=60, check=False)
metrics = json.loads(normal.stdout)
(evidence / 'benchmark.json').write_text(json.dumps(metrics, indent=2) + '\n')
if normal.returncode != 0 or metrics.get('status') != 'MEASURED':
    raise SystemExit('benchmark measurement failed')
failure = subprocess.run([executable, '--iterations', '1', '--workers', '1', '--inflight', '1',
                          '--fail-worker', '1'], capture_output=True, timeout=20, check=False)
if failure.returncode != 1 or json.loads(failure.stdout) != {
        'status': 'HARNESS_ERROR', 'reason': 'injected_worker_failure'}:
    raise SystemExit('benchmark worker-failure regression failed')
(evidence / 'benchmark-failure.json').write_text(json.dumps({
    'status': 'PASS', 'exit_code': failure.returncode, 'timeout_seconds': 20,
    'heartbeat_started_before_worker_failure': True}) + '\n')
PY
      ;;
   sanitizers)
      cmake -S "$root" -B "$build" "${options[@]}" -DFORGE_ENABLE_MODULES=ON \
         -DFORGE_ENABLE_JOSE=ON -DFORGE_ENABLE_LIBOAUTH2=OFF -DFORGE_ENABLE_OIDC_AGENT=OFF \
         -DFORGE_ENABLE_MDBX=OFF -DFORGE_ENABLE_ROCKSDB=OFF -DFORGE_ENABLE_TUI=OFF \
         -DFORGE_ENABLE_TOOLING=OFF -DBUILD_TESTING=ON \
         '-DCMAKE_C_FLAGS=-fsanitize=address,undefined -fno-omit-frame-pointer -g1' \
         '-DCMAKE_CXX_FLAGS=-stdlib=libstdc++ -fsanitize=address,undefined -fno-omit-frame-pointer -g1' \
         '-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address,undefined'
      cmake --build "$build" --target test_forge_crypto_jose test_forge_auth_attestation \
         test_forge_schema_attestation test_forge_json_strict test_forge_attestation_policy -j4 2>&1 | tee "$work/raw/compile.log"
      ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
         ctest --test-dir "$build" --no-tests=error --output-on-failure -j1 --timeout 180 \
         --output-junit "$work/results.xml" \
         -R '^test_forge_(crypto_jose|auth_attestation|schema_attestation|json_strict|attestation_policy)$' \
         > "$work/raw/tests.log" 2>&1
      ;;
   interop)
      python3 -m venv "$work/python"
      "$work/python/bin/pip" install --disable-pip-version-check -r tests/attestation_interop/requirements.txt
      "$work/python/bin/python" tests/attestation_interop/run.py \
         --driver "$build/tests/forge_attestation_interop" --work "$work/interop" \
         --evidence "$work/evidence/interop.json"
      ;;
   packages)
      cmake --build "$build" --target test_forge_package_install -j4 >> "$work/raw/compile.log" 2>&1
      cmake -S tests/package_attestation -B "$work/package-consumer" "${options[@]}" \
         -C "$build/tests/ForgePackageConsumerCache.cmake"
      cmake --build "$work/package-consumer" -j4 >> "$work/raw/compile.log" 2>&1
      ctest --test-dir "$work/package-consumer" --no-tests=error -j1 \
         --output-junit "$work/evidence/packages.xml"
      cmake -S tests/package_attestation_missing_backend -B "$work/package-missing-backend" "${options[@]}" \
         -C "$build/tests/ForgePackageConsumerCache.cmake"
      printf 'PASS\n' > "$work/evidence/missing-backend.txt"
      ;;
   guest)
      if [[ ! -d "$work/sysroot/include/c++/v1" ]]; then
         git init --quiet "$work/llvm-project"
         git -C "$work/llvm-project" remote add origin https://github.com/llvm/llvm-project.git
         git -C "$work/llvm-project" fetch --quiet --depth=1 origin ca7933e47d3a3451d81e72ac174dcb5aa28b59d1
         git -C "$work/llvm-project" checkout --quiet --detach FETCH_HEAD
         guest/sysroot/build.sh "$work/llvm-project" "$work/contract-runtimes" "$work/sysroot" "$llvm_root/bin"
      fi
      cmake -S guest -B "$work/contract-sdk" "${options[@]}" \
         -DFORGE_CONTRACT_PROFILE=developer \
         -DFORGE_CONTRACT_FORGE_DIR="$build/forge-package-install/lib/cmake/Forge" \
         -DClang_DIR="$llvm_root/lib/cmake/clang" -DLLVM_DIR="$llvm_root/lib/cmake/llvm" \
         -DFORGE_CONTRACT_SYSROOT="$work/sysroot" -DFORGE_CONTRACT_CLANG="$llvm_root/bin/clang++" \
         -DFORGE_CONTRACT_WASM_LD="$wasm_ld" -DFORGE_CONTRACT_LLVM_AR="$llvm_root/bin/llvm-ar" \
         -DFORGE_CONTRACT_LLVM_RANLIB="$llvm_root/bin/llvm-ranlib" -DFORGE_CONTRACT_JOBS=4
      cmake --build "$work/contract-sdk" -j4 >> "$work/raw/compile.log" 2>&1
      cmake --install "$work/contract-sdk" --prefix "$work/contract-sdk-prefix" >> "$work/raw/compile.log" 2>&1
      cmake -S guest/tests/attestation/guest -B "$work/guest" -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_TOOLCHAIN_FILE="$work/contract-sdk-prefix/lib/cmake/ForgeContract/ForgeContractToolchain.cmake" \
         -DForgeContract_DIR="$work/contract-sdk-prefix/lib/cmake/ForgeContract"
      cmake --build "$work/guest" -j4 >> "$work/raw/compile.log" 2>&1
      "$build/tests/forge_attestation_schema_fixture" "$work/constraints.json"
      python3 tests/attestation_boundary/bind_artifacts.py \
         --abi "$work/guest/artifacts/granttest.abi" \
         --metadata "$work/guest/artifacts/granttest.abi.metadata.json" \
         --constraints "$work/constraints.json" --output "$work/binding.json"
      "$build/tests/forge_attestation_boundary" "$work/guest/artifacts/granttest.wasm" \
         "$work/guest/artifacts/granttest.abi" "$work/guest/artifacts/granttest.abi.metadata.json" \
         "$work/constraints.json" "$work/binding.json"
      for changed in abi metadata constraints; do
         case "$changed" in
            abi) source="$work/guest/artifacts/granttest.abi" ;;
            metadata) source="$work/guest/artifacts/granttest.abi.metadata.json" ;;
            constraints) source="$work/constraints.json" ;;
         esac
         cp "$source" "$work/changed-artifact.json"
         printf ' ' >> "$work/changed-artifact.json"
         abi="$work/guest/artifacts/granttest.abi"
         metadata="$work/guest/artifacts/granttest.abi.metadata.json"
         constraints="$work/constraints.json"
         case "$changed" in
            abi) abi="$work/changed-artifact.json" ;;
            metadata) metadata="$work/changed-artifact.json" ;;
            constraints) constraints="$work/changed-artifact.json" ;;
         esac
         if "$build/tests/forge_attestation_boundary" "$work/guest/artifacts/granttest.wasm" \
            "$abi" "$metadata" "$constraints" "$work/binding.json" > "$work/raw/mismatch.log" 2>&1; then
            exit 1
         fi
      done
      cp "$work/constraints.json" "$work/evidence/constraints.json"
      cp "$work/binding.json" "$work/evidence/artifact-binding.json"
      printf 'PASS\n' > "$work/evidence/guest-boundary.txt"
      ;;
esac
