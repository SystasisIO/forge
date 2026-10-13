#!/usr/bin/env bash
set -euo pipefail

readonly phase="${1:?usage: auth-resource.sh <dependencies|catalog|regressions|packages|native-negative|evidence>}"
case "$phase" in dependencies|catalog|regressions|packages|native-negative|evidence) ;; *) exit 2 ;; esac
readonly work="${RUNNER_TEMP:?}/forge-auth-resource"
readonly build="$work/forge"
readonly deps="$work/deps"
readonly oidc="$work/oidc-installed"
readonly glaze="$work/glaze"
mkdir -p "$work/raw" "$work/evidence"
trap 'printf "%s\texit=%d\n" "$phase" "$?" >> "$work/evidence/phases.tsv"' EXIT

if [[ "$phase" == evidence ]]; then
   # Upload only whitelisted CTest result lines, never raw logs, env, keys or caches.
   python3 - "$work" <<'PY'
import pathlib, re, sys
work = pathlib.Path(sys.argv[1])
ansi = re.compile(r'\x1b\[[0-?]*[ -/]*[@-~]')
result = re.compile(r'^\s*\d+/\d+ Test\s+#\d+: test_forge_[a-z0-9_]+\s+\.+\s+(?:Passed|\*\*\*Failed|\*\*\*Timeout|\*\*\*Not Run)\s+[0-9.]+ sec\s*$')
summary = re.compile(r'^(?:\d+% tests passed, \d+ tests failed out of \d+|Total Test time \(real\) =\s+[0-9.]+ sec)$')
for name in ('regressions',):
    source = work / 'raw' / (name + '.log')
    lines = [ansi.sub('', line) for line in source.read_text(errors='replace').splitlines()] if source.exists() else []
    selected = [line for line in lines if result.fullmatch(line) or summary.fullmatch(line)]
    (work / 'evidence' / (name + '.txt')).write_text('\n'.join(selected) + '\n')
PY
   exit 0
fi

exec > >(tee "$work/raw/$phase.log") 2>&1
[[ "${GITHUB_SHA:?}" =~ ^[0-9a-f]{40}$ && "$(git rev-parse HEAD)" == "$GITHUB_SHA" ]]
export CC=/usr/bin/clang-22 CXX=/usr/bin/clang++-22
export CMAKE_BUILD_PARALLEL_LEVEL=4 CTEST_PARALLEL_LEVEL=1
# Four 5.5 GiB Chain compilers exceed the public runner's 16 GiB RAM.
# This temporary toolchain also reaches fresh installed-package consumers.
export CMAKE_TOOLCHAIN_FILE="$work/compile-pools.cmake"

fetch_pin() {
   local source="$1" url="$2" commit="$3"
   git init --quiet "$source"
   git -C "$source" remote add origin "$url"
   git -C "$source" fetch --quiet --depth=1 origin "$commit"
   git -C "$source" checkout --quiet --detach FETCH_HEAD
   [[ "$(git -C "$source" rev-parse HEAD)" == "$commit" ]]
}

if [[ "$phase" == dependencies ]]; then
   sudo apt-get update
   sudo apt-get install -y clang-22 clang-tools-22 g++-15 build-essential autoconf automake libtool \
      pkg-config python3 libcurl4-openssl-dev libpcre2-dev libsodium-dev libmicrohttpd-dev \
      libqrencode-dev libglib2.0-dev check ca-certificates
   if ! command -v brew >/dev/null 2>&1; then
      NONINTERACTIVE=1 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
   fi
   eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"
   brew install boost cmake gmp libngtcp2 ninja openssl@3
   printf '%s\n' 'set(CMAKE_JOB_POOLS "auth_compile=2;auth_link=1" CACHE STRING "CI RAM bound" FORCE)' \
      'set(CMAKE_JOB_POOL_COMPILE auth_compile CACHE STRING "CI RAM bound" FORCE)' \
      'set(CMAKE_JOB_POOL_LINK auth_link CACHE STRING "CI link bound" FORCE)' > "$CMAKE_TOOLCHAIN_FILE"
   bash .github/scripts/install-glaze.sh "$glaze"
   fetch_pin "$work/jansson-src" https://github.com/akheron/jansson.git ed5cae4ed0621ef409510f94270c9f8f263736d0
   cmake -S "$work/jansson-src" -B "$work/jansson-build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER="$CC" -DCMAKE_INSTALL_PREFIX="$deps" -DCMAKE_INSTALL_LIBDIR=lib \
      -DJANSSON_BUILD_SHARED_LIBS=ON -DJANSSON_BUILD_DOCS=OFF -DJANSSON_EXAMPLES=OFF
   cmake --build "$work/jansson-build" -j4
   cmake --install "$work/jansson-build"
   fetch_pin "$work/cjose-src" https://github.com/OpenIDC/cjose.git e787ace6a203d483038a793fd58f958a2f9b48c1
   (cd "$work/cjose-src" && autoreconf -fi)
   mkdir -p "$work/cjose-build"
   (cd "$work/cjose-build" && "$work/cjose-src/configure" --prefix="$deps" --with-jansson="$deps" \
      --with-openssl="$(brew --prefix openssl@3)" --disable-static --disable-doxygen-doc \
      LDFLAGS="-Wl,-rpath,$deps/lib -Wl,-rpath,$(brew --prefix openssl@3)/lib" && make -j4 && make -j4 install)
   cp -a vendor/oidc-agent "$work/oidc-source"
   [[ "$(< "$work/oidc-source/VERSION")" == 5.3.8-systasis.1 ]]
   oidc_options=(CC="$CC" PREFIX="$oidc" BIN_PATH="$oidc" BIN_AFTER_INST_PATH="$oidc"
      LIB_PATH="$oidc/lib" LIBDEV_PATH="$oidc/lib" INCLUDE_PATH="$oidc/include"
      USE_CJSON_SO=0 USE_LIST_SO=0 USE_MUSTACHE_SO=0)
   make -C "$work/oidc-source" -j4 "${oidc_options[@]}" bin/oidc-agent shared_lib lib/api/liboidc-agent.a pkgconfig
   make -C "$work/oidc-source" -j4 "${oidc_options[@]}" install_lib-dev "$oidc/bin/oidc-agent"
   printf '%s\n' '/home/linuxbrew/.linuxbrew/bin' >> "$GITHUB_PATH"
   printf 'forge_sha=%s\nrunner=ubuntu-26.04\nprofile=full\noptional_rocksdb_tui_s3_tooling=OFF\nstdlib=libstdc++\nbuild_jobs=4\ncompile_pool=2\n' \
      "$GITHUB_SHA" > "$work/evidence/provenance.txt"
fi

eval "$(/home/linuxbrew/.linuxbrew/bin/brew shellenv)"
export PKG_CONFIG_PATH="$deps/lib/pkgconfig:$oidc/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig"
export LD_LIBRARY_PATH="$deps/lib:$oidc/lib:$(brew --prefix openssl@3)/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
readonly prefixes="$deps;$oidc;$glaze;$(brew --prefix);$(brew --prefix boost);$(brew --prefix libngtcp2);$(brew --prefix openssl@3)"
cmake_options=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX"
   -DCMAKE_CXX_FLAGS=-stdlib=libstdc++ -DCMAKE_PREFIX_PATH="$prefixes" -Dglaze_DIR="$glaze/share/glaze")
[[ "$(pkg-config --modversion jansson)" == 2.14.1 && "$(pkg-config --modversion cjose)" == 0.6.2.8 ]]
[[ "$(pkg-config --modversion liboidc-agent)" == 5.3.8-systasis.1 ]]
[[ "$(pkg-config --variable=pcfiledir liboidc-agent)" == "$oidc/lib/pkgconfig" ]]
pkg-config --validate liboidc-agent

case "$phase" in
   dependencies)
      printf 'jansson=2.14.1@ed5cae4ed0621ef409510f94270c9f8f263736d0\ncjose=0.6.2.8@e787ace6a203d483038a793fd58f958a2f9b48c1\noidc-agent=5.3.8-systasis.1 (vendored, Make install_lib-dev)\n' >> "$work/evidence/provenance.txt"
      ;;
   catalog)
      cmake -S . -B "$build" "${cmake_options[@]}" -DCMAKE_INSTALL_LIBDIR=lib -DFORGE_BUILD_PROFILE=full \
         -DFORGE_ENABLE_MODULES=ON -DFORGE_ENABLE_MDBX=ON -DFORGE_ENABLE_ROCKSDB=OFF -DFORGE_ENABLE_TUI=OFF \
         -DFORGE_ENABLE_OIDC_AGENT=ON -DFORGE_ENABLE_LIBOAUTH2=ON -DBUILD_TESTING=ON
      # This existing target depends on the complete forge catalog before dev installation.
      cmake --build "$build" --target test_forge_package_install forge_test_oidc_agent_package_stub -j4
      ;;
   regressions)
      targets=(test_forge_auth_credential test_forge_auth_pairing test_forge_auth_session test_forge_auth_http
         test_forge_auth_token_verifier test_forge_auth_liboauth2 test_forge_client_auth test_forge_auth_oidc_agent
         test_forge_api_core test_forge_api_transport test_forge_http_error_renderer test_forge_http_parameters
         test_forge_http_client_preparation test_forge_http_websocket test_forge_api_trusted_subject
         forge_auth_liboauth2_patched_qualification)
      cmake --build "$build" --target "${targets[@]}" -j4
      ctest --test-dir "$build" --no-tests=error --output-on-failure -j1 --timeout 180 \
         -R '^test_forge_(structure|net_plugin_structure|contract_sdk_(cmake_runtime_policy|relocation_runtime_policy|dependencies)|auth_(credential|pairing|session|http|token_verifier|liboauth2|liboauth2_patched_regression|oidc_agent)|client_auth|api_core|api_transport|http_error_renderer|http_parameters|http_client_preparation|http_websocket|api_trusted_subject)$'
      ctest --test-dir "$build" --no-tests=error --output-on-failure -j1 --repeat until-fail:20 \
         -R '^test_forge_(auth_liboauth2|api_trusted_subject)$'
      ;;
   packages)
      consumers=(auth_oauth2_component auth_liboauth2_component auth_oidc_agent auth_credential_component
         auth_pairing_component auth_session_component auth_http_component api_core_component
         api_transport_component api_http_component api_websocket_component net_http_component net_websocket_component)
      [[ -f "$build/forge-package-install/lib/cmake/Forge/ForgeConfig.cmake" ]]
      [[ -f "$build/tests/ForgePackageConsumerCache.cmake" ]]
      : > "$work/evidence/package-consumers.tsv"
      for consumer in "${consumers[@]}"; do
         consumer_build="$work/package-consumers/$consumer"
         (
            # The ABI stub is confined to this fixture, never production or the native probe.
            if [[ "$consumer" == auth_oidc_agent ]]; then
               export PKG_CONFIG_PATH="$build/tests/oidc-agent-pkgconfig"
            fi
            cmake -S "tests/package_$consumer" -B "$consumer_build" -G Ninja \
               -C "$build/tests/ForgePackageConsumerCache.cmake" -DCMAKE_BUILD_TYPE=Release \
               -DCMAKE_C_COMPILER="$CC" -DCMAKE_CXX_COMPILER="$CXX" \
               -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE"
            # Explicit -j4: CTest build-and-test suppresses CMAKE_BUILD_PARALLEL_LEVEL.
            cmake --build "$consumer_build" -j4
            cd "$consumer_build"
            timeout --kill-after=5s 60s "./forge_package_$consumer"
         )
         printf 'forge_package_%s\tPASS\n' "$consumer" >> "$work/evidence/package-consumers.tsv"
      done
      ;;
   native-negative)
      readonly native="$work/native-installed"
      cmake -S tests/auth/live -B "$native" "${cmake_options[@]}" \
         -DForge_DIR="$build/forge-package-install/lib/cmake/Forge"
      cmake --build "$native" --target forge_auth_live_oidc_agent -j4
      ldd "$native/forge_auth_live_oidc_agent" | grep -F "$oidc/lib/liboidc-agent.so.5"
      [[ ! -e "$work/absent-agent.sock" ]]
      code=0
      env -u LD_PRELOAD OIDC_SOCK="$work/absent-agent.sock" OIDC_REMOTE_SOCK="$work/absent-agent.sock" \
         timeout --kill-after=5s 20s "$native/forge_auth_live_oidc_agent" --issuer https://forge-ci.invalid/issuer \
         --client forge-ci --run-id 00000000-0000-0000-0000-000000000001 \
         --account live-00000000-0000-0000-0000-000000000001 --flow acquire --timeout 5 \
         > "$work/raw/native-negative.jsonl" 2> "$work/raw/native-negative.stderr" < /dev/null || code=$?
      [[ "$code" == 3 ]]
      python3 - "$work" <<'PY'
import json, pathlib, sys
work = pathlib.Path(sys.argv[1])
events = [json.loads(line) for line in (work / 'raw/native-negative.jsonl').read_text().splitlines()]
expected = {'status': 'BLOCKED', 'event': 'setup_required', 'success': False, 'expires_at': 0, 'issuer_match_count': 0}
assert events == [expected], 'expected only typed setup_required without token acquisition'
(work / 'evidence/native-negative.json').write_text(json.dumps({'exit': 3, **expected}) + '\n')
PY
      ;;
esac
