#!/usr/bin/env bash

set -euo pipefail
trap 'echo "HARNESS_ERROR: exact VM toolchain setup failed; no LLVM source fallback" >&2' ERR

evidence="${1:?toolchain evidence directory required}"
mkdir -p "$evidence"
checker="$GITHUB_WORKSPACE/.github/scripts/check-vm-wasm-toolchain.py"

if [[ "$RUNNER_OS" == "Linux" ]]; then
   sudo apt-get update
   python3 "$checker" apt-packages "$evidence/apt-selection.json" > "$evidence/apt-selection.txt"
   packages=()
   while IFS= read -r package; do packages+=("$package"); done < "$evidence/apt-selection.txt"
   sudo apt-get install -y "${packages[@]}"
   dpkg-query --show --showformat='${binary:Package}\t${Version}\n' \
      clang-22 clang-tools-22 libc++-22-dev libc++abi-22-dev \
      libc++1-22 libc++abi1-22 libunwind-22-dev libunwind-22 > "$evidence/apt-installed.txt"
   {
      echo 'CC=/usr/lib/llvm-22/bin/clang'
      echo 'CXX=/usr/lib/llvm-22/bin/clang++'
      echo 'CXXFLAGS=-stdlib=libc++'
      echo 'LDFLAGS=-stdlib=libc++ -L/usr/lib/llvm-22/lib -Wl,-rpath,/usr/lib/llvm-22/lib'
   } >> "$GITHUB_ENV"
   echo '/usr/lib/llvm-22/bin' >> "$GITHUB_PATH"
elif [[ "$RUNNER_OS" == "macOS" ]]; then
   command -v brew >/dev/null
   case "$(sw_vers -productVersion)" in
      15.*) tag=sequoia ;;
      *) echo 'HARNESS_ERROR: unqualified native macOS bottle tag' >&2; exit 2 ;;
   esac
   case "$(uname -m)" in
      arm64) tag="arm64_$tag" ;;
      x86_64) ;;
      *) echo 'HARNESS_ERROR: unqualified macOS architecture' >&2; exit 2 ;;
   esac
   brew info --json=v2 llvm@22 > "$evidence/bottle.json"
   python3 "$checker" bottle "$evidence/bottle.json" "$tag" | tee "$evidence/bottle-check.log"
   brew install --force-bottle llvm@22
   llvm_prefix="$(brew --prefix llvm@22)"
   sdk="$(xcrun --show-sdk-path)"
   {
      echo "CC=$llvm_prefix/bin/clang"
      echo "CXX=$llvm_prefix/bin/clang++"
      echo "LLVM_PREFIX=$llvm_prefix"
      echo "CXXFLAGS=-stdlib=libc++ -isysroot $sdk"
      echo "LDFLAGS=-stdlib=libc++ -isysroot $sdk"
      echo "SDKROOT=$sdk"
   } >> "$GITHUB_ENV"
   echo "$llvm_prefix/bin" >> "$GITHUB_PATH"
else
   echo 'HARNESS_ERROR: unqualified VM CI operating system' >&2
   exit 2
fi
