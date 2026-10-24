#!/bin/bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
build_root="${EXPRESSION_NATIVE_BUILD_ROOT:-$repo_root/build/expression_native}"
source_root="$repo_root/vendor/cel-cpp"
bazel_binary="$build_root/bootstrap/bazel-8.7.0-darwin-arm64"
bazel_sha256=575f20fb23955e02f73519befd180df635b4ed0960c60f0e70fcc8d74014a713
export CC="${CC:-/opt/homebrew/opt/llvm/bin/clang}"
export CXX="${CXX:-/opt/homebrew/opt/llvm/bin/clang++}"
export BAZEL_USE_CPP_ONLY_TOOLCHAIN=1
export SDKROOT="${SDKROOT:-/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk}"
# These removals affect this child process only, not the operator's environment.
unset CPATH CPLUS_INCLUDE_PATH C_INCLUDE_PATH

if [[ "$(uname -s)" != Darwin || "$(uname -m)" != arm64 ]]; then
   echo 'This research bootstrap currently supports macOS arm64 only.' >&2
   exit 2
fi
"${EXPRESSION_NATIVE_PYTHON:-/opt/homebrew/bin/python3}" \
   "$repo_root/tests/expression_native/prepare.py" --build-root "$build_root"
mkdir -p "$build_root/bootstrap" "$source_root/expression_native"
if [[ ! -f "$bazel_binary" ]]; then
   curl --fail --location --silent --show-error \
      https://github.com/bazelbuild/bazel/releases/download/8.7.0/bazel-8.7.0-darwin-arm64 \
      --output "$bazel_binary.download"
   actual_sha256="$(shasum -a 256 "$bazel_binary.download" | cut -d ' ' -f 1)"
   [[ "$actual_sha256" == "$bazel_sha256" ]]
   mv "$bazel_binary.download" "$bazel_binary"
   chmod +x "$bazel_binary"
fi
actual_sha256="$(shasum -a 256 "$bazel_binary" | cut -d ' ' -f 1)"
[[ "$actual_sha256" == "$bazel_sha256" ]]

cp "$repo_root/tests/expression_native/cel.cpp" "$source_root/expression_native/cel.cpp"
cp "$repo_root/tests/expression_native/benchmark.hpp" "$source_root/expression_native/benchmark.hpp"
cp "$repo_root/tests/expression_native/cel/BUILD.bazel" "$source_root/expression_native/BUILD.bazel"
if [[ -f "$build_root/generated/cases.inc" ]]; then
   cp "$build_root/generated/cases.inc" "$source_root/expression_native/cases.inc"
elif [[ -f "$build_root/cases.inc" ]]; then
   cp "$build_root/cases.inc" "$source_root/expression_native/cases.inc"
elif [[ -f "$repo_root/tests/expression_native/cases.inc" ]]; then
   cp "$repo_root/tests/expression_native/cases.inc" "$source_root/expression_native/cases.inc"
else
   echo 'Generate cases.inc from the frozen corpus before building.' >&2
   exit 2
fi

cd "$source_root"
"$bazel_binary" --output_user_root="$build_root/bazel" --host_jvm_args=-Xmx768m build \
   //expression_native:cel_native \
   --jobs=4 --compilation_mode=opt --copt=-g0 --copt=-O2 --host_copt=-O2 --strip=always \
   --copt=-fno-fast-math --host_copt=-fno-fast-math \
   --copt=-ffp-contract=off --host_copt=-ffp-contract=off \
   --features=-layering_check --host_features=-layering_check \
   --cxxopt=-std=c++23 --host_cxxopt=-std=c++23 \
   --cxxopt=-mmacosx-version-min=14.0 --host_cxxopt=-mmacosx-version-min=14.0 \
   --copt=-isysroot --copt="$SDKROOT" \
   --host_copt=-isysroot --host_copt="$SDKROOT" \
   --linkopt=-isysroot --linkopt="$SDKROOT" \
   --host_linkopt=-isysroot --host_linkopt="$SDKROOT" \
   --copt=-mmacosx-version-min=14.0 --host_copt=-mmacosx-version-min=14.0 \
   --linkopt=-mmacosx-version-min=14.0 --host_linkopt=-mmacosx-version-min=14.0 \
   --linkopt=-L/opt/homebrew/opt/llvm/lib/c++ \
   --linkopt=-Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++ \
   --host_linkopt=-L/opt/homebrew/opt/llvm/lib/c++ \
   --host_linkopt=-Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++ \
   --repository_cache="$build_root/repository-cache" \
   --symlink_prefix="$build_root/cel-bazel-" \
   "$@"
cp -f "$build_root/cel-bazel-bin/expression_native/cel_native" "$build_root/cel_native"
echo "$build_root/cel_native"
