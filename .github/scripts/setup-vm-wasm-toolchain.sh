#!/usr/bin/env bash

set -Eeuo pipefail
trap 'echo "HARNESS_ERROR: exact VM toolchain provisioning failed; no implicit source fallback" >&2' ERR

evidence="${1:?toolchain evidence directory required}"
mkdir -p "$evidence"
checker="$GITHUB_WORKSPACE/.github/scripts/check-vm-wasm-toolchain.py"
mode="${2:-native}"

log_run() {
   local name="$1"
   shift
   printf '%q ' "$@" > "$evidence/$name.command"
   printf '\n' >> "$evidence/$name.command"
   "$@" 2>&1 | tee "$evidence/$name.log"
}

if [[ "$mode" == intel-runtime-off ]]; then
   # Historical compiler/headers only: never use this keg to qualify LLVM/JIT.
   log_run intel-scope python3 "$checker" intel-scope "$mode"
   test "${GITHUB_ACTIONS:-}" = true
   test "${CI:-}" = true
   test "$RUNNER_OS" = macOS
   test "$(sysctl -n sysctl.proc_translated 2>/dev/null || printf 0)" != 1
   command -v brew >/dev/null
   test "$(brew --prefix)" = /usr/local
   test "$(brew --cellar)" = /usr/local/Cellar
   export HOMEBREW_NO_AUTO_UPDATE=1 HOMEBREW_NO_INSTALL_CLEANUP=1 HOMEBREW_NO_INSTALL_UPGRADE=1
   export HOMEBREW_NO_INSTALLED_DEPENDENTS_CHECK=1
   prefix=/usr/local/Cellar/llvm/22.1.8
   test ! -e "$prefix"
   work="$RUNNER_TEMP/forge-intel-runtime-off-22.1.8"
   mkdir "$work"
   archive="$work/llvm--22.1.8.sonoma.bottle.tar.gz"
   formula="$work/llvm.rb"
   log_run intel-formula-download curl --fail --location --max-time 120 \
      https://raw.githubusercontent.com/Homebrew/homebrew-core/75d8fa416b18bc8caedacf1696831422902a2bb3/Formula/l/llvm.rb \
      --output "$formula"
   # Homebrew's public anonymous GHCR credential; not an account token.
   log_run intel-bottle-download curl --fail --location --max-time 900 \
      --header 'Authorization: Bearer QQ==' \
      https://ghcr.io/v2/homebrew/core/llvm/blobs/sha256:2f07536754d0854565f9ac37436681bb3d04a4fbb15c45c51896933262df5e48 \
      --output "$archive"
   log_run intel-archive python3 "$checker" intel-archive "$archive" "$formula" "$evidence/intel-bottle.json"
   # Default Homebrew forbids formula/bottle file-path arguments. Use its normal
   # local-tap API with the unchanged pinned formula, never a path-policy override.
   tap=forge/vm-ci
   target_formula="$tap/llvm"
   brew tap > "$evidence/intel-taps-before.txt"
   if grep -Fxq "$tap" "$evidence/intel-taps-before.txt"; then
      echo 'HARNESS_ERROR: Intel CI tap already exists; refusing mutable formula reuse' >&2
      exit 2
   fi
   log_run intel-tap-create brew tap-new --no-git "$tap"
   tap_formula="$(brew --repository "$tap")/Formula/llvm.rb"
   test ! -e "$tap_formula"
   cp "$formula" "$tap_formula"
   cmp -- "$formula" "$tap_formula"
   shasum -a 256 "$tap_formula" > "$evidence/intel-tap-formula.sha256"
   brew info --json=v2 "$target_formula" > "$evidence/intel-tap-formula.json"
   log_run intel-formula python3 "$checker" intel-formula "$evidence/intel-tap-formula.json"

   # Nested Homebrew installers do not inherit --force-bottle. Install leaves first
   # and refuse newly missing children so no nested installer can build from source.
   brew deps --missing --topological --formula "$target_formula" > "$evidence/intel-missing-deps.txt"
   index=0
   while IFS= read -r dependency; do
      [[ -n "$dependency" && "$dependency" != -* && "$dependency" != *[[:space:]]* ]]
      index=$((index + 1))
      brew deps --missing --formula "$dependency" > "$evidence/intel-dependency-$index-missing.txt"
      test ! -s "$evidence/intel-dependency-$index-missing.txt"
      log_run "intel-dependency-$index-install" brew install --force-bottle --formula "$dependency"
   done < "$evidence/intel-missing-deps.txt"
   brew deps --missing --formula "$target_formula" > "$evidence/intel-remaining-deps.txt"
   test ! -s "$evidence/intel-remaining-deps.txt"
   log_run intel-bottle-install brew install --force-bottle --skip-link "$target_formula"
   log_run intel-installed-receipt cat "$prefix/INSTALL_RECEIPT.json"

   sdk="$(xcrun --show-sdk-path)"
   unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH LIBRARY_PATH \
      DYLD_LIBRARY_PATH DYLD_FALLBACK_LIBRARY_PATH DYLD_INSERT_LIBRARIES
   export CC="$prefix/bin/clang" CXX="$prefix/bin/clang++" SDKROOT="$sdk"
   export CXXFLAGS="-stdlib=libc++ -isysroot $sdk" LDFLAGS="-stdlib=libc++ -isysroot $sdk"
   log_run intel-headers python3 "$checker" intel-headers "$prefix" "$evidence/headers"
   probe="$GITHUB_WORKSPACE/.github/probes/llvm-cxx-abi"
   host="$work/host-abi"
   mkdir "$host"
   host_flags=(-std=c++23 -stdlib=libc++ -isysroot "$sdk")
   log_run intel-host-dso "$CXX" "${host_flags[@]}" -dynamiclib "$probe/bootstrap_abi.cpp" \
      -Wl,-install_name,@rpath/libforge_bootstrap_abi.dylib -o "$host/libforge_bootstrap_abi.dylib"
   log_run intel-host-client "$CXX" "${host_flags[@]}" "$probe/bootstrap_abi_main.cpp" \
      -L"$host" -lforge_bootstrap_abi -Wl,-rpath,@loader_path -o "$host/host-abi"
   log_run intel-host-linkage otool -L "$host/host-abi" "$host/libforge_bootstrap_abi.dylib"
   log_run intel-host-run "$host/host-abi"
   {
      echo "CC=$CC"
      echo "CXX=$CXX"
      echo "CXXFLAGS=$CXXFLAGS"
      echo "LDFLAGS=$LDFLAGS"
      echo "SDKROOT=$SDKROOT"
   } >> "$GITHUB_ENV"
   echo "$prefix/bin" >> "$GITHUB_PATH"
   echo 'READY: Intel runtime-OFF host toolchain only; Forge runtime gates remain required'
   exit 0
elif [[ "$mode" == stage0-linux ]]; then
   test "$RUNNER_OS" = Linux
   test "$(uname -m)" = x86_64
   grep -qx 'VERSION_CODENAME=noble' /etc/os-release
   key="$evidence/stage0-key.asc"
   log_run stage0-key-download curl --fail --location --max-time 120 \
      https://apt.llvm.org/llvm-snapshot.gpg.key --output "$key"
   echo "8b2a587ffd672c4687e7581dad4b2f6c1bb2ad6b480cd9771ba2ff48e0b8c75d  $key" \
      | sha256sum --check --strict | tee "$evidence/stage0-key-sha256.log"
   key_home="$RUNNER_TEMP/forge-stage0-gnupg"
   mkdir -m 700 "$key_home"
   gpg --no-options --homedir "$key_home" --batch --with-colons --show-keys "$key" \
      > "$evidence/stage0-key-fingerprint.txt"
   grep -qx 'fpr:::::::::6084F3CF814B57C1CF12EFD515CF4D18AF4F7421:' "$evidence/stage0-key-fingerprint.txt"
   keyring="$key_home/llvm22.gpg"
   gpg --no-options --homedir "$key_home" --batch --dearmor --output "$keyring" "$key"
   sudo install -m 644 "$keyring" /usr/share/keyrings/forge-stage0-llvm22.gpg
   printf '%s\n' 'deb [arch=amd64 signed-by=/usr/share/keyrings/forge-stage0-llvm22.gpg] https://apt.llvm.org/noble/ llvm-toolchain-noble-22 main' \
      | sudo tee /etc/apt/sources.list.d/forge-stage0-llvm22.list >/dev/null
   log_run stage0-apt-update sudo apt-get update
   log_run stage0-release-download curl --fail --location --max-time 120 \
      https://apt.llvm.org/noble/dists/llvm-toolchain-noble-22/InRelease --output "$evidence/stage0-InRelease"
   log_run stage0-signature gpgv --homedir "$key_home" --keyring "$keyring" --status-fd 1 "$evidence/stage0-InRelease"
   python3 "$checker" stage0-packages "$evidence/stage0-packages.json" > "$evidence/stage0-packages.txt"
   packages=()
   while IFS= read -r package; do packages+=("$package"); done < "$evidence/stage0-packages.txt"
   log_run stage0-apt-install sudo apt-get install -y --no-install-recommends "${packages[@]}"
   log_run stage0-guard python3 "$checker" stage0 "$evidence/stage0"
   exit 0
elif [[ "$mode" == bootstrap-linux ]]; then
   # This is the dedicated, explicitly budgeted CI producer, never a fallback.
   test "$RUNNER_OS" = Linux
   test "$(uname -m)" = x86_64
   bundle="${3:?artifact bundle directory required}"
   recipe="${4:?immutable recipe required}"
   mkdir -p "$bundle"
   log_run stage0-recheck python3 "$checker" stage0 "$evidence/stage0-before-build"
   log_run disk-preflight python3 "$checker" disk "$RUNNER_TEMP" "$evidence/disk-preflight.json"
   work="$RUNNER_TEMP/forge-llvm-source-bootstrap"
   mkdir "$work"
   archive="$work/llvm-project-22.1.8.src.tar.xz"
   log_run source-download curl --fail --location --max-time 900 \
      https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/llvm-project-22.1.8.src.tar.xz \
      --output "$archive"
   echo "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888  $archive" \
      | sha256sum --check --strict | tee "$evidence/source-sha256.log"
   source="$work/source"
   prefix="$work/install"
   export CMAKE_BUILD_PARALLEL_LEVEL=2
   mkdir "$source"
   tar -xJf "$archive" --strip-components=1 -C "$source"

   # Stage A: upstream standalone runtimes, built by the qualified pinned stage0.
   log_run runtimes-configure cmake -S "$source/runtimes" -B "$work/runtimes" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
      -DCMAKE_C_COMPILER=/usr/bin/clang-22 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-22 \
      -DCMAKE_JOB_POOLS=link=1 -DCMAKE_JOB_POOL_LINK=link \
      '-DLLVM_ENABLE_RUNTIMES=libunwind;libcxxabi;libcxx' \
      -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
      -DLIBUNWIND_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_DOCS=OFF \
      -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
      -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_USE_COMPILER_RT=OFF \
      -DLIBUNWIND_USE_COMPILER_RT=OFF -DLIBCXX_ENABLE_EXCEPTIONS=ON \
      -DLIBCXXABI_ENABLE_EXCEPTIONS=ON -DCMAKE_INSTALL_RPATH='$ORIGIN' \
      -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF
   log_run runtimes-build cmake --build "$work/runtimes" -j 2
   log_run runtimes-install cmake --install "$work/runtimes" --strip

   probe="$GITHUB_WORKSPACE/.github/probes/llvm-cxx-abi"
   abi_flags=(-std=c++23 -stdlib=libc++ -nostdinc++ -isystem "$prefix/include/c++/v1" \
      -L"$prefix/lib" -fexceptions -frtti)
   # Preserve this exact DSO across the final runtime/compiler replacement.
   log_run stage0-dso /usr/bin/clang++-22 "${abi_flags[@]}" -shared -fPIC \
      "$probe/bootstrap_abi.cpp" -Wl,-rpath,'$ORIGIN' \
      -o "$prefix/lib/libforge_bootstrap_abi.so"
   sha256sum "$prefix/lib/libforge_bootstrap_abi.so" > "$evidence/stage0-dso.sha256"
   mkdir -p "$prefix/bin"
   log_run stage0-client /usr/bin/clang++-22 "${abi_flags[@]}" "$probe/bootstrap_abi_main.cpp" \
      -lforge_bootstrap_abi -Wl,-rpath,'$ORIGIN/../lib' -o "$prefix/bin/forge-bootstrap-abi-stage0"
   log_run stage0-run "$prefix/bin/forge-bootstrap-abi-stage0"

   # Stage B: explicitly bind both headers and link search to Stage A libc++.
   # Upstream llvm-config exports only its stdlib/ABI flags, not this staging include path.
   log_run llvm-configure cmake -S "$source/llvm" -B "$work/llvm" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
      -DCMAKE_C_COMPILER=/usr/bin/clang-22 -DCMAKE_CXX_COMPILER=/usr/bin/clang++-22 \
      "-DCMAKE_CXX_FLAGS=-stdlib=libc++ -isystem $prefix/include/c++/v1" \
      "-DCMAKE_EXE_LINKER_FLAGS=-L$prefix/lib -fuse-ld=/usr/bin/ld.lld-22" \
      "-DCMAKE_SHARED_LINKER_FLAGS=-L$prefix/lib -fuse-ld=/usr/bin/ld.lld-22" \
      -DCMAKE_BUILD_RPATH="$prefix/lib" -DCMAKE_INSTALL_RPATH='$ORIGIN/../lib' \
      -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF \
      '-DLLVM_ENABLE_PROJECTS=clang;clang-tools-extra' \
      -DLLVM_TARGETS_TO_BUILD=X86 -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
      -DLLVM_ENABLE_LIBCXX=ON -DLLVM_ENABLE_RTTI=ON -DLLVM_ENABLE_EH=ON \
      -DLLVM_ENABLE_PIC=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DLLVM_BUILD_LLVM_DYLIB=ON -DLLVM_LINK_LLVM_DYLIB=ON \
      -DCLANG_LINK_CLANG_DYLIB=ON -DCLANG_DEFAULT_CXX_STDLIB=libc++ \
      -DLLVM_ENABLE_LTO=OFF -DLLVM_PARALLEL_COMPILE_JOBS=2 -DLLVM_PARALLEL_LINK_JOBS=1 \
      -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF \
      -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_DOCS=OFF \
      -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_LIBXML2=OFF \
      -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_FFI=OFF
   cp "$work/llvm/CMakeCache.txt" "$evidence/llvm-CMakeCache.txt"
   log_run llvm-c-pic-check grep -Fx 'C_SUPPORTS_FPIC:INTERNAL=1' "$work/llvm/CMakeCache.txt"
   log_run llvm-cxx-pic-check grep -Fx 'CXX_SUPPORTS_FPIC:INTERNAL=1' "$work/llvm/CMakeCache.txt"
   log_run llvm-build cmake --build "$work/llvm" -j 2
   log_run llvm-install cmake --install "$work/llvm" --strip

   # Stage C is a separate upstream runtime graph, serialized after the LLVM graph.
   # This prevents a nested runtime Ninja from exceeding the two-job/one-linker budget.
   log_run final-runtimes-configure cmake -S "$source/runtimes" -B "$work/final-runtimes" -G Ninja \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$prefix" \
      -DCMAKE_C_COMPILER="$prefix/bin/clang" -DCMAKE_CXX_COMPILER="$prefix/bin/clang++" \
      -DLLVM_CONFIG_PATH="$prefix/bin/llvm-config" -DLLVM_DIR="$prefix/lib/cmake/llvm" \
      -DCMAKE_JOB_POOLS=link=1 -DCMAKE_JOB_POOL_LINK=link \
      '-DLLVM_ENABLE_RUNTIMES=compiler-rt;libunwind;libcxxabi;libcxx' \
      -DLLVM_INCLUDE_TESTS=OFF -DLIBCXX_INCLUDE_TESTS=OFF -DLIBCXXABI_INCLUDE_TESTS=OFF \
      -DLIBUNWIND_INCLUDE_TESTS=OFF -DCOMPILER_RT_INCLUDE_TESTS=OFF \
      -DLIBCXX_INCLUDE_BENCHMARKS=OFF -DLIBCXX_INCLUDE_DOCS=OFF \
      -DLLVM_ENABLE_PER_TARGET_RUNTIME_DIR=OFF \
      -DLIBCXXABI_USE_LLVM_UNWINDER=ON -DLIBCXXABI_USE_COMPILER_RT=OFF \
      -DLIBUNWIND_USE_COMPILER_RT=OFF -DLIBCXX_ENABLE_EXCEPTIONS=ON \
      -DLIBCXXABI_ENABLE_EXCEPTIONS=ON -DCOMPILER_RT_BUILD_SANITIZERS=ON \
      -DCMAKE_INSTALL_RPATH='$ORIGIN' -DCMAKE_INSTALL_RPATH_USE_LINK_PATH=OFF
   log_run final-runtimes-build cmake --build "$work/final-runtimes" -j 2
   log_run final-runtimes-install cmake --install "$work/final-runtimes" --strip
   sha256sum --check --strict "$evidence/stage0-dso.sha256"
   log_run final-client "$prefix/bin/clang++" -std=c++23 -stdlib=libc++ \
      "$probe/bootstrap_abi_main.cpp" -L"$prefix/lib" -lforge_bootstrap_abi \
      -Wl,-rpath,'$ORIGIN/../lib' -o "$prefix/bin/forge-bootstrap-abi"

   # Keep original upstream license/notice texts even when documentation is disabled.
   license_dir="$prefix/share/licenses/llvm-project"
   shopt -s nullglob
   license_files=()
   for component in . llvm clang clang-tools-extra libcxx libcxxabi libunwind compiler-rt; do
      destination="$license_dir/$component"
      mkdir -p "$destination"
      for notice in "$source/$component"/LICENSE* "$source/$component"/NOTICE* \
         "$source/$component"/license* "$source/$component"/notice*; do
         [[ -f "$notice" ]] || continue
         cp -p "$notice" "$destination/"
         installed="$destination/$(basename "$notice")"
         cmp -- "$notice" "$installed"
         license_files+=("$installed")
      done
   done
   test -s "$license_dir/llvm/LICENSE.TXT"
   sha256sum "${license_files[@]}" > "$evidence/upstream-license-sha256.txt"

   # Qualification runs only after relocation, without loader search overrides.
   relocated="$work/relocated"
   mv "$prefix" "$relocated"
   chmod 000 "$work/llvm" "$work/runtimes" "$work/final-runtimes" "$source"
   unset LD_LIBRARY_PATH LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
   test "$("$relocated/bin/llvm-config" --prefix)" = "$relocated"
   log_run relocated-stage0-abi "$relocated/bin/forge-bootstrap-abi-stage0"
   log_run relocated-bootstrap-abi "$relocated/bin/forge-bootstrap-abi"
   log_run llvm-abi python3 "$GITHUB_WORKSPACE/.github/scripts/check-llvm-cxx-abi.py" \
      "$relocated" "$evidence/llvm-abi"
   cp "$recipe" "$bundle/recipe.json"
   python3 "$checker" pack "$relocated" "$recipe" "$bundle/toolchain.tar.gz" \
      | tee "$bundle/archive.sha256"
   echo "sha256=$(cat "$bundle/archive.sha256")" >> "$GITHUB_OUTPUT"
   exit 0
elif [[ "$mode" == consume-linux ]]; then
   test "$RUNNER_OS" = Linux
   test "$(uname -m)" = x86_64
   bundle="${3:?artifact bundle directory required}"
   prefix="${4:?fresh toolchain prefix required}"
   digest="${5:?producer archive SHA required}"
   key="${6:?producer recipe key required}"
   log_run artifact-check python3 "$checker" unpack "$bundle/toolchain.tar.gz" "$prefix" "$digest" "$key"
   unset LD_LIBRARY_PATH LIBRARY_PATH CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH
   log_run llvm-abi python3 "$GITHUB_WORKSPACE/.github/scripts/check-llvm-cxx-abi.py" \
      "$prefix" "$evidence/llvm-abi"
   echo "$prefix/bin" >> "$GITHUB_PATH"
   exit 0
elif [[ "$mode" != native ]]; then
   echo 'HARNESS_ERROR: unknown explicit toolchain provisioning mode' >&2
   exit 2
fi

if [[ "$RUNNER_OS" == "Linux" ]]; then
   echo 'HARNESS_ERROR: Linux requires the dedicated source-bootstrap artifact; no apt fallback' >&2
   exit 2
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
