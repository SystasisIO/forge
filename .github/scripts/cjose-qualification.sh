#!/usr/bin/env bash
set -euo pipefail

readonly phase="${1:?usage: cjose-qualification.sh <dependencies|prepare|baseline|candidate|legacy-abi|consumers|sanitizers|evidence>}"
case "$phase" in dependencies|prepare|baseline|candidate|legacy-abi|consumers|sanitizers|evidence) ;; *) exit 2 ;; esac
[[ "${CI:-}" == true && "${GITHUB_ACTIONS:-}" == true ]]
[[ "${GITHUB_SHA:?}" =~ ^[0-9a-f]{40}$ && "$(git rev-parse HEAD)" == "$GITHUB_SHA" ]]
readonly work="${RUNNER_TEMP:?}/forge-cjose-qualification"
mkdir -p "$work/raw" "$work/evidence"
trap 'printf "%s\texit=%d\n" "$phase" "$?" >> "$work/evidence/phases.tsv"' EXIT

if [[ "$phase" == dependencies ]]; then
   # Do not echo dependency diagnostics (or a future package-manager token).
   if [[ "$RUNNER_OS" == Linux ]]; then
      sudo apt-get update > "$work/raw/apt-update.log" 2>&1
      sudo apt-get install -y clang cmake ninja-build pkg-config check libssl-dev autoconf automake libtool \
         > "$work/raw/apt-install.log" 2>&1
   elif [[ "$RUNNER_OS" == macOS ]]; then
      brew install cmake ninja pkgconf check openssl@3 llvm autoconf automake libtool > "$work/raw/brew.log" 2>&1
   else
      exit 2
   fi
   python3 -m venv "$work/python"
   "$work/python/bin/python" -m pip install --disable-pip-version-check \
      -r tests/cjose_qualification/requirements.txt > "$work/raw/pip.log" 2>&1
   "$work/python/bin/python" -m pip check >> "$work/raw/pip.log" 2>&1
   exit 0
fi

if [[ "$RUNNER_OS" == macOS ]]; then
   export CC="$(brew --prefix llvm)/bin/clang"
   export OPENSSL_ROOT_DIR="$(brew --prefix openssl@3)"
   export PKG_CONFIG_PATH="$(brew --prefix check)/lib/pkgconfig:$(brew --prefix openssl@3)/lib/pkgconfig"
   export SDKROOT="$(xcrun --show-sdk-path)"
   export LIBTOOLIZE=glibtoolize
else
   export CC=/usr/bin/clang
   export OPENSSL_ROOT_DIR=/usr
fi
export PKG_CONFIG_PATH="$work/deps/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:allocator_may_return_null=1
# Apple ASan has no LeakSanitizer. This limitation is recorded in provenance.
if [[ "$RUNNER_OS" == macOS ]]; then
   export ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:allocator_may_return_null=1
fi
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=0
"$work/python/bin/python" .github/scripts/cjose-qualification.py "$phase" --work "$work"
