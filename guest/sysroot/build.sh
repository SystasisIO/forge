#!/usr/bin/env bash

set -euo pipefail

if [[ $# -ne 4 ]]; then
   echo "usage: build.sh <llvm-source> <build-dir> <install-dir> <llvm-bin>" >&2
   exit 2
fi

script_dir="$(cd "$(dirname "$0")" && pwd)"
llvm_source="$(cd "$1" && pwd)"
cmake -E make_directory "$2" "$3"
build_dir="$(cd "$2" && pwd)"
install_dir="$(cd "$3" && pwd)"
llvm_bin="$(cd "$4" && pwd)"

cmake -E make_directory "$install_dir/include"
cmake -E copy_directory "$script_dir/include" "$install_dir/include"

cmake -S "$llvm_source/runtimes" -B "$build_dir" -G Ninja \
   -DCMAKE_BUILD_TYPE=Release \
   -DCMAKE_INSTALL_PREFIX="$install_dir" \
   -DCMAKE_SYSTEM_NAME=Generic \
   -DCMAKE_SYSROOT="$install_dir" \
   -DCMAKE_C_COMPILER="$llvm_bin/clang" \
   -DCMAKE_CXX_COMPILER="$llvm_bin/clang++" \
   -DCMAKE_ASM_COMPILER="$llvm_bin/clang" \
   -DCMAKE_C_COMPILER_TARGET=wasm32 \
   -DCMAKE_CXX_COMPILER_TARGET=wasm32 \
   -DCMAKE_ASM_COMPILER_TARGET=wasm32 \
   -DCMAKE_AR="$llvm_bin/llvm-ar" \
   -DCMAKE_RANLIB="$llvm_bin/llvm-ranlib" \
   -DCMAKE_ASM_COMPILER_AR="$llvm_bin/llvm-ar" \
   -DCMAKE_ASM_COMPILER_RANLIB="$llvm_bin/llvm-ranlib" \
   -DCMAKE_C_FLAGS=-mcpu=mvp \
   -DCMAKE_CXX_FLAGS=-mcpu=mvp \
   -DCMAKE_ASM_FLAGS=-mcpu=mvp \
   -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
   -DLLVM_ENABLE_RUNTIMES='libcxx;libcxxabi;compiler-rt' \
   -DLIBCXX_ENABLE_EXCEPTIONS=OFF \
   -DLIBCXX_ENABLE_RTTI=OFF \
   -DLIBCXX_ENABLE_THREADS=OFF \
   -DLIBCXX_ENABLE_FILESYSTEM=OFF \
   -DLIBCXX_ENABLE_LOCALIZATION=ON \
   -DLIBCXX_HAS_MUSL_LIBC=ON \
   -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF \
   -DLIBCXX_ENABLE_MONOTONIC_CLOCK=OFF \
   -DLIBCXX_ENABLE_WIDE_CHARACTERS=OFF \
   -DLIBCXX_ENABLE_UNICODE=OFF \
   -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF \
   -DLIBCXX_HAS_TERMINAL_AVAILABLE=OFF \
   -DLIBCXX_ENABLE_SHARED=OFF \
   -DLIBCXX_ENABLE_STATIC=ON \
   -DLIBCXX_SHARED_OUTPUT_NAME=c++-shared-disabled \
   -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON \
   -DLIBCXX_ENABLE_NEW_DELETE_DEFINITIONS=OFF \
   -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF \
   -DLIBCXXABI_ENABLE_THREADS=OFF \
   -DLIBCXXABI_ENABLE_SHARED=OFF \
   -DLIBCXXABI_ENABLE_STATIC=ON \
   -DLIBCXXABI_SHARED_OUTPUT_NAME=c++abi-shared-disabled \
   -DLIBCXXABI_ENABLE_NEW_DELETE_DEFINITIONS=OFF \
   -DLIBCXXABI_BAREMETAL=ON \
   -DLIBCXXABI_USE_LLVM_UNWINDER=OFF \
   -DCOMPILER_RT_BAREMETAL_BUILD=ON \
   -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON \
   -DCOMPILER_RT_BUILD_BUILTINS=ON \
   -DCOMPILER_RT_BUILD_SANITIZERS=OFF \
   -DCOMPILER_RT_BUILD_XRAY=OFF \
   -DCOMPILER_RT_BUILD_LIBFUZZER=OFF \
   -DCOMPILER_RT_BUILD_PROFILE=OFF \
   -DCOMPILER_RT_INCLUDE_TESTS=OFF

cmake --build "$build_dir" --target install -j 4
