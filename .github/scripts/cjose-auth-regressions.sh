#!/usr/bin/env bash
set -euo pipefail
umask 077

readonly phase="${1:?usage: cjose-auth-regressions.sh <dependencies|baseline|candidate|compare|evidence>}"
case "$phase" in dependencies|baseline|candidate|compare|evidence) ;; *) exit 2 ;; esac
readonly root="$(git rev-parse --show-toplevel)"
readonly work="${RUNNER_TEMP:?}/forge-cjose-auth-regressions"
readonly deps="$work/deps"
readonly old="$work/old-cjose"
readonly glaze="$work/glaze"
readonly baseline_sha=8975f234dabf2cbfb91d766bf32194f3d8832a5b
readonly old_cjose_sha=e787ace6a203d483038a793fd58f958a2f9b48c1
readonly jansson_sha=ed5cae4ed0621ef409510f94270c9f8f263736d0
mkdir -p "$work/raw" "$work/evidence"

if [[ "$phase" == compare || "$phase" == evidence ]]; then
   # Runtime evidence is whitelist-only; compile-only diagnostics are separate.
   python3 - "$root" "$work" "$phase" <<'PY'
import ast
import hashlib
import json
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

root, work, phase = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
tests = {
    'test_forge_auth_liboauth2': 'tests/auth/liboauth2_tests.cpp',
    'test_forge_auth_token_verifier': 'tests/auth/token_verifier_tests.cpp',
    'test_forge_api_trusted_subject': 'tests/http_websocket/trusted_subject_tests.cpp',
    'test_forge_auth_http': 'tests/auth/http_auth_tests.cpp',
    'test_forge_auth_liboauth2_patched_regression': 'tests/auth/liboauth2_qualification/patched_run.py',
}
identifier = re.compile(r'[a-z][a-z0-9_]*')
case_identifier = re.compile(r'[a-z][a-z0-9-]*')
sha = re.compile(r'[0-9a-f]{40}')


def require(value):
    if not value:
        raise ValueError('invalid evidence')


def read_json(path):
    return json.loads(path.read_text())


def expected_cases(source, test):
    text = (source / tests[test]).read_text()
    if test.endswith('_patched_regression'):
        tree = ast.parse(text)
        cases = next(ast.literal_eval(node.value) for node in tree.body
                     if isinstance(node, ast.Assign) and any(
                         isinstance(target, ast.Name) and target.id == 'CASES' for target in node.targets))
        require(all(isinstance(name, str) and case_identifier.fullmatch(name) for name in cases))
    else:
        cases = re.findall(r'BOOST_AUTO_TEST_CASE\(\s*([a-z][a-z0-9_]*)\s*\)', text)
    require(cases and len(set(cases)) == len(cases))
    return set(cases), hashlib.sha256(text.encode()).hexdigest()


def collect(lane):
    source = work / 'baseline-source' if lane == 'baseline' else root
    raw = work / 'raw' / lane
    result = {'status': 'HARNESS_ERROR', 'tests': {}, 'installed_package': 'NOT_RUN',
              'build_tree_package': 'NOT_RUN' if lane == 'candidate' else 'NOT_REQUIRED'}
    try:
        codes = {}
        for line in (raw / 'exits.tsv').read_text().splitlines():
            name, code = line.split('\t')
            require(name in tests and name not in codes and code.isdecimal())
            codes[name] = int(code)
        for test in tests:
            item = {'status': 'HARNESS_ERROR', 'cases': [], 'expected_count': 0}
            result['tests'][test] = item
            try:
                expected, digest = expected_cases(source, test)
                item.update(expected_count=len(expected), source_sha256=digest)
                ctest = ET.parse(raw / (test + '.ctest.xml')).getroot().findall('.//testcase')
                require(len(ctest) == 1 and ctest[0].get('name') == test)
                require(ctest[0].find('skipped') is None)
                records = []
                if test.endswith('_patched_regression'):
                    summary = None
                    for line in (raw / (test + '.log')).read_text(errors='replace').splitlines():
                        line = re.sub(r'^\d+: ', '', line)
                        if not line.startswith('{'):
                            continue
                        data = json.loads(line)
                        if 'case' in data:
                            require(data['case'] in expected)
                            require(data.get('verdict') in ('PASS', 'FAIL', 'HARNESS_ERROR'))
                            records.append({'case': data['case'], 'status': data['verdict']})
                        elif 'case_count' in data:
                            require(summary is None)
                            summary = data
                    require(summary is not None and type(summary.get('fixture_valid')) is bool)
                    require(summary['case_count'] == len(expected))
                    require(type(summary.get('regression_passed')) is bool)
                    require(summary['regression_passed'] == all(r['status'] == 'PASS' for r in records))
                    if not summary['fixture_valid']:
                        records = [{'case': record['case'], 'status': 'HARNESS_ERROR'} for record in records]
                else:
                    report = ET.parse(raw / (test + '.boost.xml')).getroot()
                    for node in report.findall('.//TestCase'):
                        name, status = node.get('name'), node.get('result')
                        require(name in expected and identifier.fullmatch(name))
                        require(status in ('passed', 'failed', 'aborted', 'timed-out', 'skipped'))
                        records.append({'case': name, 'status': 'PASS' if status == 'passed' else (
                            'HARNESS_ERROR' if status == 'skipped' else 'FAIL')})
                require(len(records) == len(expected) and {r['case'] for r in records} == expected)
                item['cases'] = sorted(records, key=lambda r: r['case'])
                item['executed_count'] = sum(r['status'] != 'HARNESS_ERROR' for r in records)
                passed = all(r['status'] == 'PASS' for r in records)
                require(test in codes)
                item['exit'] = codes[test]
                item['status'] = 'PASS' if passed and codes[test] == 0 and ctest[0].find('failure') is None else 'FAIL'
                if any(r['status'] == 'HARNESS_ERROR' for r in records):
                    item['status'] = 'HARNESS_ERROR'
            except Exception:
                item['status'] = 'HARNESS_ERROR'
        require((raw / 'linkage.ok').read_text() == 'PASS\n')
        result['linkage'] = 'PASS'
        if (raw / 'installed-package.ok').is_file():
            require((raw / 'installed-package.ok').read_text() == 'PASS\n')
            result['installed_package'] = 'PASS'
        if lane == 'candidate' and (raw / 'build-tree-package.ok').is_file():
            require((raw / 'build-tree-package.ok').read_text() == 'PASS\n')
            result['build_tree_package'] = 'PASS'
        statuses = [test['status'] for test in result['tests'].values()]
        result['status'] = 'HARNESS_ERROR' if 'HARNESS_ERROR' in statuses else (
            'PASS' if all(status == 'PASS' for status in statuses) else 'FAIL')
        if result['installed_package'] != 'PASS':
            result['status'] = 'HARNESS_ERROR'
        if lane == 'candidate' and result['build_tree_package'] != 'PASS':
            result['status'] = 'HARNESS_ERROR'
        require((raw / 'complete').read_text() == 'PASS\n')
    except Exception:
        result['status'] = 'HARNESS_ERROR'
    return result


report = {'scope': 'F0-B-Q7-existing-auth-regressions', 'status': 'HARNESS_ERROR', 'f0_b_closed': False,
          'installed_package_required': True,
          'baseline': collect('baseline'), 'candidate': collect('candidate'),
          'case_comparison': {}, 'provenance': {},
          'gaps': ['unmodified-cjose-0.8.1-middle-state-NOT_RUN',
                   'HS256-dir-A256GCM-old-to-new-and-new-to-old-persisted-data-NOT_RUN',
                   'backend-C-harness-is-independent', 'F1-auth-attestation-out-of-scope']}
stages = {'source-pin', 'dependency-setup', 'jansson-build', 'old-cjose-build', 'provenance',
          'configure', 'configured-version', 'focused-build', 'linkage', 'tests', 'catalog-build',
          'dev-install', 'consumer-configure', 'consumer-build', 'consumer-linkage', 'consumer-run',
          'build-tree-consumer-configure', 'build-tree-consumer-build', 'build-tree-consumer-run', 'complete'}
report['phases'] = {}
for lane in ('dependencies', 'baseline', 'candidate'):
    path = work / 'raw' / lane / 'last-step'
    if path.is_file() and path.read_text().strip() in stages:
        report['phases'][lane] = {'last_step': path.read_text().strip()}
path = work / 'raw' / 'phases.tsv'
if path.is_file():
    for line in path.read_text().splitlines():
        match = re.fullmatch(r'(dependencies|baseline|candidate)\texit=([0-9]{1,3})', line)
        if match:
            report['phases'].setdefault(match[1], {})['exit'] = int(match[2])
probe = root / 'tests/auth/liboauth2_qualification/patched_probe.cpp'
if probe.is_file() and re.search(r'std::strcmp\(CJOSE_VERSION,\s*"0\.6\.2\.8"', probe.read_text()):
    report['gaps'].append('candidate-existing-patched-probe-hardcodes-0.6.2.8-HARNESS_ERROR')
try:
    provenance = read_json(work / 'raw' / 'provenance.json')
    # All values are validated, not a recursive copy of arbitrary input fields.
    for key in ('baseline_forge_sha', 'candidate_forge_sha', 'old_cjose_sha', 'candidate_cjose_sha',
                'jansson_sha', 'glaze_sha', 'baseline_liboauth2_sha', 'candidate_liboauth2_sha'):
        require(sha.fullmatch(provenance[key]))
        report['provenance'][key] = provenance[key]
    for key in ('clang', 'cmake', 'ninja', 'boost', 'openssl', 'pcre2', 'curl', 'gmp', 'libngtcp2'):
        value = provenance[key]
        require(isinstance(value, str) and re.fullmatch(r'[a-zA-Z0-9.+:~_@ -]{1,160}', value))
        report['provenance'][key] = value
    require(provenance['baseline_forge_sha'] == '8975f234dabf2cbfb91d766bf32194f3d8832a5b')
    require(provenance['old_cjose_sha'] == 'e787ace6a203d483038a793fd58f958a2f9b48c1')
    require(provenance['jansson_sha'] == 'ed5cae4ed0621ef409510f94270c9f8f263736d0')
    report['provenance'].update(runner='ubuntu-26.04', stdlib='libstdc++', build_jobs=4,
                                compile_pool=2, old_cjose='0.6.2.8-system-shared-isolated-prefix',
                                candidate_cjose='0.8.1-vendored-static', jansson='2.14.1')
    for test in tests:
        before = {r['case']: r['status'] for r in report['baseline']['tests'].get(test, {}).get('cases', [])}
        after = {r['case']: r['status'] for r in report['candidate']['tests'].get(test, {}).get('cases', [])}
        report['case_comparison'][test] = {
            'baseline_count': len(before), 'candidate_count': len(after),
            'removed': sorted(before.keys() - after.keys()), 'added': sorted(after.keys() - before.keys()),
            'regressed': sorted(name for name, status in before.items()
                                if status == 'PASS' and after.get(name) != 'PASS')}
    ready = all(report[lane]['status'] == 'PASS' for lane in ('baseline', 'candidate'))
    preserved = all(not entry['removed'] and not entry['regressed'] for entry in report['case_comparison'].values())
    report['status'] = 'PASS' if ready and preserved else (
        'HARNESS_ERROR' if any(report[lane]['status'] == 'HARNESS_ERROR' for lane in ('baseline', 'candidate')) else 'FAIL')
except Exception:
    report['status'] = 'HARNESS_ERROR'
for lane in ('baseline', 'candidate'):
    if report[lane]['installed_package'] != 'PASS':
        report['gaps'].append(lane + '-default-dev-install-consumer-NOT_RUN-or-incomplete')
if report['candidate']['build_tree_package'] != 'PASS':
    report['gaps'].append('candidate-build-tree-consumer-NOT_RUN-or-incomplete')
# These logs have a dedicated writer used only for configure/build/install, never
# for test execution. Keep useful compiler context without uploading runtime logs.
for lane in ('dependencies', 'baseline', 'candidate'):
    path = work / 'raw' / lane / 'compile.log'
    if path.is_file():
        with path.open('rb') as stream:
            stream.seek(max(0, path.stat().st_size - 262144))
            data = stream.read().decode('utf-8', errors='replace')
        data = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', data)
        data = data.replace(str(root), '$SOURCE').replace(str(work), '$WORK')
        (work / 'evidence' / (lane + '-compile.txt')).write_text(data)
(work / 'evidence' / 'q7.json').write_text(json.dumps(report, indent=2, sort_keys=True) + '\n')
print('Q7 ' + report['status'] + '; sanitized case lists/counts: q7.json')
sys.exit(0 if phase == 'evidence' or report['status'] == 'PASS' else 1)
PY
   exit $?
fi

[[ "${CI:-}" == true && "${RUNNER_OS:-}" == Linux ]]
[[ "${GITHUB_SHA:?}" =~ ^[0-9a-f]{40}$ && "$(git rev-parse HEAD)" == "$GITHUB_SHA" ]]
exec 3>&1
trap 'code=$?; printf "%s\texit=%d\n" "$phase" "$code" >> "$work/raw/phases.tsv"; printf "Q7 %s exit=%d (compile diagnostics in artifacts; runtime logs withheld)\n" "$phase" "$code" >&3' EXIT
exec > "$work/raw/$phase.log" 2>&1
mkdir -p "$work/raw/$phase"
unset LD_LIBRARY_PATH LD_PRELOAD CMAKE_PREFIX_PATH CMAKE_MODULE_PATH
unset BOOST_TEST_RUN_FILTERS BOOST_TEST_REPORT_FORMAT BOOST_TEST_REPORT_LEVEL BOOST_TEST_REPORT_SINK
export CC=/usr/bin/clang-22 CXX=/usr/bin/clang++-22
export CMAKE_BUILD_PARALLEL_LEVEL=4 CTEST_PARALLEL_LEVEL=1
export CMAKE_TOOLCHAIN_FILE="$work/compile-pools.cmake"

fetch_pin() {
   local source="$1" url="$2" commit="$3"
   git init --quiet "$source"
   git -C "$source" remote add origin "$url"
   git -C "$source" fetch --quiet --depth=1 origin "$commit"
   git -C "$source" checkout --quiet --detach FETCH_HEAD
   [[ "$(git -C "$source" rev-parse HEAD)" == "$commit" ]]
}

compile_only() {
   printf '\n[Q7 compile-only] %s\n' "$1" >> "$work/raw/$phase/compile.log"
   "$@" >> "$work/raw/$phase/compile.log" 2>&1
}

stage() {
   printf '%s\n' "$1" > "$work/raw/$phase/last-step"
}

if [[ "$phase" == dependencies ]]; then
   stage source-pin
   # Other teams may advance dev while this lane runs; the baseline is immutable.
   git fetch --no-tags origin "$baseline_sha"
   [[ "$(git rev-parse FETCH_HEAD)" == "$baseline_sha" ]]
   git merge-base --is-ancestor "$baseline_sha" HEAD
   git worktree add --detach "$work/baseline-source" "$baseline_sha"
   [[ "$(git -C "$work/baseline-source" rev-parse HEAD)" == "$baseline_sha" ]]
   git -C "$work/baseline-source" submodule update --init --recursive
   [[ "$(git -C vendor/cjose rev-parse HEAD)" == "$(git ls-tree HEAD vendor/cjose | awk '{print $3}')" ]]
   [[ -z "$(git -C vendor/cjose status --porcelain)" ]]
   [[ "${Q7_INSTALLED_PACKAGE:-true}" == true ]]
   printf 'required\n' > "$work/raw/package-requested"
   stage dependency-setup
   sudo apt-get update
   sudo apt-get install -y clang-22 clang-tools-22 g++-15 build-essential autoconf automake libtool \
      pkg-config python3 libcurl4-openssl-dev libpcre2-dev ca-certificates
   if ! command -v brew >/dev/null 2>&1; then
      NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   fi
   eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"
   brew install boost cmake gmp libngtcp2 ninja openssl@3
   [[ "$(brew list --versions boost)" == 'boost 1.90.0' ]]
   [[ "$($CC -dumpversion)" == 22.* ]]
   # -j4 remains the scheduler budget; two compiler jobs avoid runner OOMs.
   printf '%s\n' 'set(CMAKE_JOB_POOLS "q7_compile=2;q7_link=1" CACHE STRING "CI RAM bound" FORCE)' \
      'set(CMAKE_JOB_POOL_COMPILE q7_compile CACHE STRING "CI RAM bound" FORCE)' \
      'set(CMAKE_JOB_POOL_LINK q7_link CACHE STRING "CI link bound" FORCE)' > "$CMAKE_TOOLCHAIN_FILE"
   compile_only bash "$root/.github/scripts/install-glaze.sh" "$glaze"
   fetch_pin "$work/jansson-source" https://github.com/akheron/jansson.git "$jansson_sha"
   stage jansson-build
   compile_only cmake -S "$work/jansson-source" -B "$work/jansson-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POLICY_VERSION_MINIMUM=3.5 -DCMAKE_C_COMPILER="$CC" \
      -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib \
      -DJANSSON_BUILD_SHARED_LIBS=ON -DJANSSON_BUILD_DOCS=OFF -DJANSSON_EXAMPLES=OFF
   compile_only cmake --build "$work/jansson-build" -j4
   compile_only cmake --install "$work/jansson-build"
   stage old-cjose-build
   fetch_pin "$work/old-cjose-source" https://github.com/OpenIDC/cjose.git "$old_cjose_sha"
   (cd "$work/old-cjose-source" && compile_only autoreconf -fi)
   mkdir -p "$work/old-cjose-build"
   (cd "$work/old-cjose-build" && compile_only "$work/old-cjose-source/configure" --prefix="$old" --with-jansson="$deps" \
      --with-openssl="$(brew --prefix openssl@3)" --disable-static --disable-doxygen-doc \
      LDFLAGS="-Wl,-rpath,$deps/lib -Wl,-rpath,$(brew --prefix openssl@3)/lib" && compile_only make -j4 && compile_only make -j4 install)
   stage provenance
   python3 - "$root" "$work" <<'PY'
import json
from pathlib import Path
import subprocess
import sys

root, work = Path(sys.argv[1]), Path(sys.argv[2])
def command(*args):
    return subprocess.check_output(args, text=True).strip()
def pin(source, path):
    return command('git', '-C', str(source), 'ls-tree', 'HEAD', path).split()[2]
values = {
    'baseline_forge_sha': command('git', '-C', str(work / 'baseline-source'), 'rev-parse', 'HEAD'),
    'candidate_forge_sha': command('git', '-C', str(root), 'rev-parse', 'HEAD'),
    'old_cjose_sha': command('git', '-C', str(work / 'old-cjose-source'), 'rev-parse', 'HEAD'),
    'candidate_cjose_sha': pin(root, 'vendor/cjose'),
    'jansson_sha': command('git', '-C', str(work / 'jansson-source'), 'rev-parse', 'HEAD'),
    'glaze_sha': command('git', '-C', str(work.parent / 'forge-glaze-7.5.0/source'), 'rev-parse', 'HEAD'),
    'baseline_liboauth2_sha': pin(work / 'baseline-source', 'vendor/liboauth2'),
    'candidate_liboauth2_sha': pin(root, 'vendor/liboauth2'),
    'clang': command('dpkg-query', '-W', '-f=${Version}', 'clang-22'),
    'cmake': command('cmake', '--version').splitlines()[0],
    'ninja': command('ninja', '--version'),
    'pcre2': command('pkg-config', '--modversion', 'libpcre2-8'),
    'curl': command('pkg-config', '--modversion', 'libcurl'),
}
for formula, key in [('boost', 'boost'), ('openssl@3', 'openssl'), ('gmp', 'gmp'), ('libngtcp2', 'libngtcp2')]:
    values[key] = command('brew', 'list', '--versions', formula)
(work / 'raw' / 'provenance.json').write_text(json.dumps(values) + '\n')
PY
   stage complete
   exit 0
fi

eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"
readonly build="$work/$phase-build"
readonly raw="$work/raw/$phase"
mkdir -p "$raw"
source="$root"
export PKG_CONFIG_PATH="$deps/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig"
prefixes="$deps;$glaze;$(brew --prefix);$(brew --prefix boost);$(brew --prefix libngtcp2);$(brew --prefix openssl@3)"
runtime_paths="$deps/lib;$(brew --prefix openssl@3)/lib"
if [[ "$phase" == baseline ]]; then
   source="$work/baseline-source"
   export PKG_CONFIG_PATH="$old/lib/pkgconfig:$PKG_CONFIG_PATH"
   prefixes="$old;$prefixes"
   runtime_paths="$old/lib;$runtime_paths"
   [[ "$(pkg-config --modversion cjose)" == 0.6.2.8 ]]
   [[ "$(pkg-config --variable=pcfiledir cjose)" == "$old/lib/pkgconfig" ]]
else
   # A globally available CJOSE would make accidental system discovery ambiguous.
   if pkg-config --exists cjose; then
      exit 1
   fi
fi
[[ "$(pkg-config --modversion jansson)" == 2.14.1 ]]
[[ "$(pkg-config --variable=pcfiledir jansson)" == "$deps/lib/pkgconfig" ]]
options=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
   -DCMAKE_CXX_FLAGS=-stdlib=libstdc++ -DCMAKE_PREFIX_PATH="$prefixes" -Dglaze_DIR="$glaze/share/glaze"
   -DOPENSSL_ROOT_DIR="$(brew --prefix openssl@3)" -DCMAKE_BUILD_RPATH="$runtime_paths"
   -DCMAKE_FIND_USE_PACKAGE_REGISTRY=OFF -DCMAKE_FIND_USE_SYSTEM_PACKAGE_REGISTRY=OFF)
if [[ "$phase" == candidate ]]; then
   options+=(-DCMAKE_IGNORE_PREFIX_PATH="$old")
fi
stage configure
compile_only cmake -S "$source" -B "$build" "${options[@]}" -DCMAKE_INSTALL_LIBDIR=lib \
   -DFORGE_BUILD_PROFILE=full -DFORGE_ENABLE_MODULES=ON -DFORGE_ENABLE_LIBOAUTH2=ON \
   -DFORGE_ENABLE_OIDC_AGENT=OFF -DFORGE_ENABLE_MDBX=ON -DFORGE_ENABLE_ROCKSDB=OFF \
   -DFORGE_ENABLE_TUI=OFF -DFORGE_ENABLE_S3=OFF -DFORGE_ENABLE_TOOLING=OFF -DBUILD_TESTING=ON
if [[ "$phase" == candidate ]]; then
   stage configured-version
   # Source pin alone does not prove which version the configured build selected.
   grep -Eq '^#define CJOSE_VERSION +"0\.8\.1"' "$build/vendor/cjose/generated-include/cjose/version.h"
fi
tests=(test_forge_auth_liboauth2 test_forge_auth_token_verifier test_forge_api_trusted_subject test_forge_auth_http
   test_forge_auth_liboauth2_patched_regression)
stage focused-build
compile_only cmake --build "$build" --target "${tests[@]:0:4}" forge_auth_liboauth2_patched_qualification -j4

check_linkage() {
   local binary="$1" graph="$2" target="$3" archive="${4:-vendor/cjose/libcjose.a}"
   local linkage name expected resolved commands
   linkage="$(env -u LD_LIBRARY_PATH -u LD_PRELOAD LC_ALL=C ldd -r "$binary" 2>&1)"
   printf '%s\n' "$linkage"
   [[ "$linkage" != *"not found"* && "$linkage" != *"undefined symbol:"* ]]
   for name in libjansson.so.4 libcrypto.so.3 libssl.so.3; do
      case "$name" in
         libjansson.so.4) expected="$deps/lib/libjansson.so" ;;
         *) expected="$(brew --prefix openssl@3)/lib/$name" ;;
      esac
      resolved="$(awk -v name="$name" '$1 == name && $2 == "=>" {print $3}' <<< "$linkage")"
      [[ -n "$resolved" && "$(readlink -f "$resolved")" == "$(readlink -f "$expected")" ]]
   done
   if [[ "$phase" == baseline ]]; then
      resolved="$(awk '$1 == "libcjose.so.0" && $2 == "=>" {print $3}' <<< "$linkage")"
      [[ -n "$resolved" && "$(readlink -f "$resolved")" == "$(readlink -f "$old/lib/libcjose.so")" ]]
   else
      [[ "$linkage" != *libcjose* && "$linkage" != *"$old"* ]]
      # Inspect the real final link command, not just the existence of an unused archive.
      commands="$(ninja -C "$graph" -t commands "$target" | tail -n1)"
      [[ "$commands" == *"$archive"* && "$commands" != *"$old"* ]]
   fi
}
stage linkage
check_linkage "$build/tests/test_forge_auth_liboauth2" "$build" test_forge_auth_liboauth2
check_linkage "$build/tests/auth/liboauth2_qualification/forge_auth_liboauth2_patched_qualification" \
   "$build" forge_auth_liboauth2_patched_qualification
printf 'PASS\n' > "$raw/linkage.ok"

failed=0
stage tests
for test in "${tests[@]}"; do
   code=0
   env -u LD_LIBRARY_PATH -u LD_PRELOAD BOOST_TEST_REPORT_FORMAT=XML BOOST_TEST_REPORT_LEVEL=detailed \
      BOOST_TEST_REPORT_SINK="$raw/$test.boost.xml" BOOST_TEST_LOG_LEVEL=nothing BOOST_TEST_COLOR_OUTPUT=no \
      ctest --test-dir "$build" --no-tests=error -V -j1 --timeout 180 \
      --output-junit "$raw/$test.ctest.xml" -R "^${test}$" > "$raw/$test.log" 2>&1 || code=$?
   printf '%s\t%d\n' "$test" "$code" >> "$raw/exits.tsv"
   if (( code != 0 )); then failed=1; fi
done

[[ -f "$work/raw/package-requested" ]]
(
   # The stock dev component installs every exported target. Do not invent a
   # sparse package or silently ignore missing archives to claim install PASS.
   stage catalog-build
   compile_only cmake --build "$build" --target forge -j4
   installed="$work/$phase-installed"
   stage dev-install
   compile_only cmake --install "$build" --component dev --prefix "$installed"
   consumer="$work/$phase-consumer"
   stage consumer-configure
   compile_only cmake -S "$source/tests/package_auth_liboauth2_component" -B "$consumer" "${options[@]}" \
      -DForge_DIR="$installed/lib/cmake/Forge" -DCMAKE_PREFIX_PATH="$installed;$prefixes"
   stage consumer-build
   compile_only cmake --build "$consumer" -j4
   # The installed candidate must resolve its installed archive, not the build tree.
   stage consumer-linkage
   check_linkage "$consumer/forge_package_auth_liboauth2_component" "$consumer" \
      forge_package_auth_liboauth2_component "$installed/lib/libcjose.a"
   stage consumer-run
   (cd "$consumer" && env -u LD_LIBRARY_PATH -u LD_PRELOAD timeout --kill-after=5s 60s ./forge_package_auth_liboauth2_component)
   printf 'PASS\n' > "$raw/installed-package.ok"
   if [[ "$phase" == candidate ]]; then
      build_tree_consumer="$work/candidate-build-tree-consumer"
      stage build-tree-consumer-configure
      compile_only cmake -S "$source/tests/package_auth_liboauth2_component" -B "$build_tree_consumer" \
         "${options[@]}" -DForge_DIR="$build" -DCMAKE_MODULE_PATH="$source/cmake"
      stage build-tree-consumer-build
      compile_only cmake --build "$build_tree_consumer" -j4
      check_linkage "$build_tree_consumer/forge_package_auth_liboauth2_component" "$build_tree_consumer" \
         forge_package_auth_liboauth2_component "$build/vendor/cjose/libcjose.a"
      stage build-tree-consumer-run
      (cd "$build_tree_consumer" && env -u LD_LIBRARY_PATH -u LD_PRELOAD timeout --kill-after=5s 60s \
         ./forge_package_auth_liboauth2_component)
      printf 'PASS\n' > "$raw/build-tree-package.ok"
   fi
)
stage complete
printf 'PASS\n' > "$raw/complete"
exit "$failed"
