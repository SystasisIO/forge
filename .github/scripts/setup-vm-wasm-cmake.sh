#!/usr/bin/env bash

set -euo pipefail
trap 'echo "HARNESS_ERROR: pinned VM CMake provisioning failed; no version fallback" >&2' ERR

evidence="${1:?CMake evidence directory required}"
mkdir -p "$evidence"
# CMake 4.4 synthesizes incompatible duplicate BMIs with the qualified Clang22.
# Keep the tested generator instead of changing runtime FP flags or public APIs.
case "$RUNNER_OS:$(uname -m)" in
   Linux:x86_64)
      name=cmake-3.31.6-linux-x86_64
      digest=5a1133ff103c71eb5120e2cc3de922733e7d8a26a98ae716397e8676adb367bf
      bin_suffix=bin
      checksum=(sha256sum --check --strict)
      ;;
   macOS:arm64|macOS:x86_64)
      name=cmake-3.31.6-macos-universal
      digest=330b9514f5112e5ed4fb08b8b05803b776fd9b539a6ae12927d14dcc0ee2ba8d
      bin_suffix=CMake.app/Contents/bin
      checksum=(shasum -a 256 --check)
      ;;
   *) echo 'HARNESS_ERROR: unqualified VM CMake platform' >&2; exit 2 ;;
esac

work="$RUNNER_TEMP/forge-vm-cmake-3.31.6"
mkdir "$work"
archive="$work/$name.tar.gz"
curl --fail --location --max-time 300 \
   "https://github.com/Kitware/CMake/releases/download/v3.31.6/$name.tar.gz" --output "$archive"
printf '%s  %s\n' "$digest" "$archive" | "${checksum[@]}" | tee "$evidence/archive-sha256.txt"
tar -xzf "$archive" -C "$work"
bin="$work/$name/$bin_suffix"
version="$("$bin/cmake" --version)"
printf '%s\n' "$version" | tee "$evidence/cmake-version.txt"
test "${version%%$'\n'*}" = 'cmake version 3.31.6'
printf '%s\n' "$bin" >> "$GITHUB_PATH"
printf 'READY: pinned CMake3.31.6; source/module/runtime tests remain required\n'
