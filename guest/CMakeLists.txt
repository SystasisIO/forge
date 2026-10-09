cmake_minimum_required(VERSION 3.31)

file(
   STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../CMakeLists.txt"
   _forge_project_declaration
   REGEX "^project\\(forge VERSION [0-9]+\\.[0-9]+\\.[0-9]+ "
   LIMIT_COUNT 1
)
if(NOT _forge_project_declaration MATCHES "VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)")
   message(FATAL_ERROR "cannot determine Forge version for the Contract SDK")
endif()

project(ForgeContractSDK VERSION "${CMAKE_MATCH_1}" LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(ExternalProject)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(FORGE_CONTRACT_PROFILE "developer" CACHE STRING "Contract SDK profile: developer or release")
set_property(CACHE FORGE_CONTRACT_PROFILE PROPERTY STRINGS developer release)
set(FORGE_CONTRACT_FORGE_DIR "" CACHE PATH "Directory containing ForgeConfig.cmake for host tools")
set(FORGE_CONTRACT_SYSROOT "" CACHE PATH "Existing wasm32 sysroot used by developer mode")
set(FORGE_CONTRACT_CLANG "" CACHE FILEPATH "Developer-mode clang++")
set(FORGE_CONTRACT_CLANG_C "" CACHE FILEPATH "Developer-mode clang C driver")
set(FORGE_CONTRACT_WASM_LD "" CACHE FILEPATH "Developer-mode wasm-ld")
set(FORGE_CONTRACT_LLVM_AR "" CACHE FILEPATH "Developer-mode llvm-ar")
set(FORGE_CONTRACT_LLVM_RANLIB "" CACHE FILEPATH "Developer-mode llvm-ranlib")
set(FORGE_CONTRACT_LLVM_SOURCE_DIR "" CACHE PATH "Existing pinned LLVM source checkout used by release mode")
set(FORGE_CONTRACT_JOBS 4 CACHE STRING "Parallel jobs used by SDK superbuilds")

set(FORGE_CONTRACT_LLVM_TAG "llvmorg-22.1.8")
set(FORGE_CONTRACT_LLVM_COMMIT "ca7933e47d3a3451d81e72ac174dcb5aa28b59d1")
set(FORGE_CONTRACT_MANIFEST_LLVM_VERSION "${FORGE_CONTRACT_LLVM_TAG}")
set(FORGE_CONTRACT_MANIFEST_LLVM_COMMIT "${FORGE_CONTRACT_LLVM_COMMIT}")
set(FORGE_CONTRACT_SYSROOT_SCHEMA_VERSION 1)
set(FORGE_CONTRACT_INTRINSIC_VERSION 1)

file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/.." _forge_contract_source_root)
set(
   _forge_contract_path_map_options
   "-ffile-prefix-map=${_forge_contract_source_root}=."
   "-fdebug-prefix-map=${_forge_contract_source_root}=."
   "-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build"
   "-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build"
)
list(JOIN _forge_contract_path_map_options " " _forge_contract_path_map_flags)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_package(Boost 1.90.0 REQUIRED CONFIG COMPONENTS headers)

set(_forge_contract_boost_include "")
foreach(_candidate IN LISTS Boost_INCLUDE_DIRS)
   if(EXISTS "${_candidate}/boost/pfr/core.hpp")
      set(_forge_contract_boost_include "${_candidate}")
      break()
   endif()
endforeach()
if(NOT _forge_contract_boost_include)
   message(FATAL_ERROR "Boost.PFR headers were not found in Boost ${Boost_VERSION}")
endif()

if(FORGE_CONTRACT_PROFILE STREQUAL "release")
   set(_llvm_install "${CMAKE_BINARY_DIR}/toolchain")
   if(FORGE_CONTRACT_LLVM_SOURCE_DIR)
      file(REAL_PATH "${FORGE_CONTRACT_LLVM_SOURCE_DIR}" _llvm_source)
      if(NOT EXISTS "${_llvm_source}/llvm/CMakeLists.txt")
         message(FATAL_ERROR "FORGE_CONTRACT_LLVM_SOURCE_DIR is not an llvm-project checkout: ${_llvm_source}")
      endif()
      find_package(Git REQUIRED)
      execute_process(
         COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
         WORKING_DIRECTORY "${_llvm_source}"
         OUTPUT_VARIABLE _llvm_source_commit
         OUTPUT_STRIP_TRAILING_WHITESPACE
         COMMAND_ERROR_IS_FATAL ANY
      )
      if(NOT _llvm_source_commit STREQUAL FORGE_CONTRACT_LLVM_COMMIT)
         message(
            FATAL_ERROR
            "release LLVM checkout is ${_llvm_source_commit}, expected ${FORGE_CONTRACT_LLVM_COMMIT}"
         )
      endif()
      set(
         _llvm_source_arguments
         SOURCE_DIR "${_llvm_source}"
         DOWNLOAD_COMMAND ""
         UPDATE_COMMAND ""
         PATCH_COMMAND ""
      )
   else()
      set(_llvm_source "${CMAKE_BINARY_DIR}/forge_contract_llvm-prefix/src/forge_contract_llvm")
      set(
         _llvm_source_arguments
         GIT_REPOSITORY https://github.com/llvm/llvm-project.git
         GIT_TAG ${FORGE_CONTRACT_LLVM_COMMIT}
         GIT_SHALLOW FALSE
      )
   endif()
   set(
      _forge_contract_llvm_path_map_options
      ${_forge_contract_path_map_options}
      "-ffile-prefix-map=${_llvm_source}=./llvm-project"
      "-fdebug-prefix-map=${_llvm_source}=./llvm-project"
   )
   list(JOIN _forge_contract_llvm_path_map_options " " _forge_contract_llvm_path_map_flags)
   set(_forge_contract_wasm_path_map_flags "-mcpu=mvp ${_forge_contract_llvm_path_map_flags}")
   if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64)$")
      set(_llvm_host_target AArch64)
   else()
      set(_llvm_host_target X86)
   endif()
   execute_process(
      COMMAND "${CMAKE_C_COMPILER}" -dumpmachine
      OUTPUT_VARIABLE _llvm_host_triple
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY
   )
   if(_llvm_host_triple STREQUAL "")
      message(FATAL_ERROR "release LLVM bootstrap compiler did not report a host target triple")
   endif()
   set(_llvm_projects "clang|lld")
   set(_llvm_targets "WebAssembly|${_llvm_host_target}")
   ExternalProject_Add(
      forge_contract_llvm
      ${_llvm_source_arguments}
      SOURCE_SUBDIR llvm
      CMAKE_GENERATOR Ninja
      LIST_SEPARATOR "|"
      CMAKE_ARGS
         -DCMAKE_BUILD_TYPE=Release
         -DCMAKE_INSTALL_PREFIX=${_llvm_install}
         -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
         -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
         -DCMAKE_C_FLAGS=${_forge_contract_llvm_path_map_flags}
         -DCMAKE_CXX_FLAGS=${_forge_contract_llvm_path_map_flags}
         -DLLVM_ENABLE_PROJECTS:STRING=${_llvm_projects}
         -DLLVM_TARGETS_TO_BUILD:STRING=${_llvm_targets}
         -DLLVM_HOST_TRIPLE:STRING=${_llvm_host_triple}
         -DLLVM_DEFAULT_TARGET_TRIPLE:STRING=${_llvm_host_triple}
         -DLLVM_BUILD_LLVM_DYLIB=ON
         -DLLVM_LINK_LLVM_DYLIB=ON
         -DCLANG_LINK_CLANG_DYLIB=ON
         -DLLVM_ENABLE_RTTI=ON
         -DLLVM_ENABLE_TERMINFO=OFF
         -DLLVM_ENABLE_ZLIB=OFF
         -DLLVM_ENABLE_ZSTD=OFF
         -DLLVM_INCLUDE_TESTS=OFF
         -DLLVM_INCLUDE_EXAMPLES=OFF
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> -j ${FORGE_CONTRACT_JOBS}
      INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install -j ${FORGE_CONTRACT_JOBS}
   )

   set(FORGE_CONTRACT_CLANG "${_llvm_install}/bin/clang++")
   set(FORGE_CONTRACT_CLANG_C "${_llvm_install}/bin/clang")
   set(FORGE_CONTRACT_CLANG_SCAN_DEPS "${_llvm_install}/bin/clang-scan-deps")
   set(FORGE_CONTRACT_WASM_LD "${_llvm_install}/bin/wasm-ld")
   set(FORGE_CONTRACT_LLVM_AR "${_llvm_install}/bin/llvm-ar")
   set(FORGE_CONTRACT_LLVM_RANLIB "${_llvm_install}/bin/llvm-ranlib")
   set(FORGE_CONTRACT_CLANG_RESOURCE_DIR "${_llvm_install}/lib/clang/22")
   set(FORGE_CONTRACT_SYSROOT "${CMAKE_BINARY_DIR}/sysroot-base")
   set(_llvm_runtimes "libcxx|libcxxabi|compiler-rt")

   ExternalProject_Add(
      forge_contract_sysroot
      DEPENDS forge_contract_llvm
      SOURCE_DIR "${_llvm_source}/runtimes"
      DOWNLOAD_COMMAND ""
      UPDATE_COMMAND ""
      PATCH_COMMAND ""
      CMAKE_GENERATOR Ninja
      LIST_SEPARATOR "|"
      CMAKE_ARGS
         -DCMAKE_BUILD_TYPE=Release
         -DCMAKE_INSTALL_PREFIX=${FORGE_CONTRACT_SYSROOT}
         -DCMAKE_SYSTEM_NAME=Generic
         -DCMAKE_SYSROOT=${FORGE_CONTRACT_SYSROOT}
         -DCMAKE_C_COMPILER=${_llvm_install}/bin/clang
         -DCMAKE_CXX_COMPILER=${_llvm_install}/bin/clang++
         -DCMAKE_ASM_COMPILER=${_llvm_install}/bin/clang
         -DCMAKE_C_COMPILER_TARGET=wasm32
         -DCMAKE_CXX_COMPILER_TARGET=wasm32
         -DCMAKE_ASM_COMPILER_TARGET=wasm32
         -DCMAKE_AR=${_llvm_install}/bin/llvm-ar
         -DCMAKE_RANLIB=${_llvm_install}/bin/llvm-ranlib
         -DCMAKE_ASM_COMPILER_AR=${_llvm_install}/bin/llvm-ar
         -DCMAKE_ASM_COMPILER_RANLIB=${_llvm_install}/bin/llvm-ranlib
         -DCMAKE_C_FLAGS=${_forge_contract_wasm_path_map_flags}
         -DCMAKE_CXX_FLAGS=${_forge_contract_wasm_path_map_flags}
         -DCMAKE_ASM_FLAGS=${_forge_contract_wasm_path_map_flags}
         -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
         -DLLVM_ENABLE_RUNTIMES:STRING=${_llvm_runtimes}
         -DLLVM_INCLUDE_TESTS=OFF
         -DLLVM_INCLUDE_DOCS=OFF
         -DLIBCXX_ENABLE_EXCEPTIONS=OFF
         -DLIBCXX_ENABLE_RTTI=OFF
         -DLIBCXX_ENABLE_THREADS=OFF
         -DLIBCXX_ENABLE_FILESYSTEM=OFF
         -DLIBCXX_ENABLE_LOCALIZATION=ON
         -DLIBCXX_HAS_MUSL_LIBC=ON
         -DLIBCXX_ENABLE_RANDOM_DEVICE=OFF
         -DLIBCXX_ENABLE_MONOTONIC_CLOCK=OFF
         -DLIBCXX_ENABLE_WIDE_CHARACTERS=OFF
         -DLIBCXX_ENABLE_UNICODE=OFF
         -DLIBCXX_ENABLE_TIME_ZONE_DATABASE=OFF
         -DLIBCXX_HAS_TERMINAL_AVAILABLE=OFF
         -DLIBCXX_ENABLE_SHARED=OFF
         -DLIBCXX_ENABLE_STATIC=ON
         -DLIBCXX_SHARED_OUTPUT_NAME=c++-shared-disabled
         -DLIBCXX_ENABLE_STATIC_ABI_LIBRARY=ON
         -DLIBCXX_ENABLE_NEW_DELETE_DEFINITIONS=OFF
         -DLIBCXXABI_ENABLE_EXCEPTIONS=OFF
         -DLIBCXXABI_ENABLE_THREADS=OFF
         -DLIBCXXABI_ENABLE_SHARED=OFF
         -DLIBCXXABI_ENABLE_STATIC=ON
         -DLIBCXXABI_SHARED_OUTPUT_NAME=c++abi-shared-disabled
         -DLIBCXXABI_ENABLE_NEW_DELETE_DEFINITIONS=OFF
         -DLIBCXXABI_BAREMETAL=ON
         -DLIBCXXABI_USE_LLVM_UNWINDER=OFF
         -DCOMPILER_RT_BAREMETAL_BUILD=ON
         -DCOMPILER_RT_DEFAULT_TARGET_ONLY=ON
         -DCOMPILER_RT_BUILD_BUILTINS=ON
         -DCOMPILER_RT_BUILD_SANITIZERS=OFF
         -DCOMPILER_RT_BUILD_XRAY=OFF
         -DCOMPILER_RT_BUILD_LIBFUZZER=OFF
         -DCOMPILER_RT_BUILD_PROFILE=OFF
         -DCOMPILER_RT_INCLUDE_TESTS=OFF
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> -j ${FORGE_CONTRACT_JOBS}
      INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install -j ${FORGE_CONTRACT_JOBS}
   )
   ExternalProject_Add_Step(
      forge_contract_sysroot stage_guest_headers
      COMMAND ${CMAKE_COMMAND} -E make_directory "${FORGE_CONTRACT_SYSROOT}/include"
      COMMAND
         ${CMAKE_COMMAND} -E copy_directory
         "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/include"
         "${FORGE_CONTRACT_SYSROOT}/include"
      DEPENDEES download
      DEPENDERS configure
   )
   if(NOT FORGE_CONTRACT_FORGE_DIR)
      message(FATAL_ERROR "Release profile requires -DFORGE_CONTRACT_FORGE_DIR=<installed Forge package>")
   endif()
   set(_host_tools_install "${CMAKE_BINARY_DIR}/host-tools")
   string(REPLACE ";" "|" _forge_contract_prefix_path "${CMAKE_PREFIX_PATH}")
   ExternalProject_Add(
      forge_contract_tools
      DEPENDS forge_contract_llvm
      SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../tools"
      CMAKE_GENERATOR Ninja
      LIST_SEPARATOR "|"
      CMAKE_ARGS
         -DCMAKE_BUILD_TYPE=Release
         -DCMAKE_INSTALL_PREFIX=${_host_tools_install}
         -DCMAKE_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
         -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
         -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
         -DCMAKE_C_FLAGS=${_forge_contract_path_map_flags}
         -DCMAKE_CXX_FLAGS=${_forge_contract_path_map_flags}
         -DCMAKE_PREFIX_PATH:PATH=${_forge_contract_prefix_path}
         -DClang_DIR=${_llvm_install}/lib/cmake/clang
         -DLLVM_DIR=${_llvm_install}/lib/cmake/llvm
         -DForge_DIR=${FORGE_CONTRACT_FORGE_DIR}
      BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> -j ${FORGE_CONTRACT_JOBS}
      INSTALL_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target install -j ${FORGE_CONTRACT_JOBS}
   )
   set(FORGE_CONTRACT_REPRODUCIBLE true)
else()
   find_package(Clang 22.1 CONFIG REQUIRED)
   find_library(
      FORGE_CONTRACT_CLANG_CPP_RUNTIME
      NAMES clang-cpp
      HINTS ${CLANG_LIBRARY_DIRS} ${LLVM_LIBRARY_DIRS}
      REQUIRED
      NO_DEFAULT_PATH
   )
   find_library(
      FORGE_CONTRACT_LLVM_RUNTIME
      NAMES LLVM
      HINTS ${LLVM_LIBRARY_DIRS}
      REQUIRED
      NO_DEFAULT_PATH
   )
   include(cmake/select_developer_toolchain.cmake)
   forge_contract_select_developer_toolchain()
   foreach(
      _tool
      FORGE_CONTRACT_CLANG
      FORGE_CONTRACT_CLANG_C
      FORGE_CONTRACT_CLANG_SCAN_DEPS
      FORGE_CONTRACT_WASM_LD
      FORGE_CONTRACT_LLVM_AR
      FORGE_CONTRACT_LLVM_RANLIB
   )
      if(NOT EXISTS "${${_tool}}" OR IS_DIRECTORY "${${_tool}}")
         message(FATAL_ERROR "Developer profile requires a valid ${_tool}: ${${_tool}}")
      endif()
   endforeach()
   if(NOT FORGE_CONTRACT_SYSROOT OR NOT IS_DIRECTORY "${FORGE_CONTRACT_SYSROOT}")
      message(FATAL_ERROR "Developer profile requires -DFORGE_CONTRACT_SYSROOT=<wasm32 sysroot>")
   endif()
   execute_process(
      COMMAND "${FORGE_CONTRACT_CLANG}" -print-resource-dir
      OUTPUT_VARIABLE FORGE_CONTRACT_CLANG_RESOURCE_DIR
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY
   )
   execute_process(
      COMMAND "${FORGE_CONTRACT_CLANG}" --version
      OUTPUT_VARIABLE _forge_contract_clang_version
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY
   )
   string(REGEX MATCH "^[^\r\n]+" FORGE_CONTRACT_MANIFEST_LLVM_VERSION "${_forge_contract_clang_version}")
   if(FORGE_CONTRACT_MANIFEST_LLVM_VERSION STREQUAL "")
      message(FATAL_ERROR "cannot determine the selected developer Clang version")
   endif()
   set(FORGE_CONTRACT_MANIFEST_LLVM_COMMIT "")
   set(FORGE_CONTRACT_REPRODUCIBLE false)
   if(FORGE_CONTRACT_FORGE_DIR)
      find_package(
         Forge CONFIG REQUIRED
         COMPONENTS
            chain_protocol
            codec_json
            vm_wasm_interpret
            tooling_abi
            tooling_attributes
            tooling_validation
            tooling_manifest
         PATHS "${FORGE_CONTRACT_FORGE_DIR}"
         NO_DEFAULT_PATH
      )
   else()
      find_package(
         Forge CONFIG REQUIRED
         COMPONENTS
            chain_protocol
            codec_json
            vm_wasm_interpret
            tooling_abi
            tooling_attributes
            tooling_validation
            tooling_manifest
      )
   endif()
   add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../tools" "${CMAKE_CURRENT_BINARY_DIR}/tools")
endif()

set(_forge_contract_input_sysroot "${FORGE_CONTRACT_SYSROOT}")
set(FORGE_CONTRACT_SYSROOT "${CMAKE_BINARY_DIR}/sdk-sysroot")
set(_forge_contract_sysroot_stage_dependencies)
if(TARGET forge_contract_sysroot)
   list(APPEND _forge_contract_sysroot_stage_dependencies forge_contract_sysroot)
endif()
# A developer sysroot is a mutable directory, so staging must run on every dependent build.
add_custom_target(
   forge_contract_stage_sysroot
   COMMAND
      ${CMAKE_COMMAND}
      -DSOURCE=${_forge_contract_input_sysroot}
      -DDESTINATION=${FORGE_CONTRACT_SYSROOT}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_sysroot.cmake"
   COMMAND
      ${CMAKE_COMMAND} -E copy_directory
      "${CMAKE_CURRENT_SOURCE_DIR}/sysroot/include"
      "${FORGE_CONTRACT_SYSROOT}/include"
   DEPENDS
      ${_forge_contract_sysroot_stage_dependencies}
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/stage_sysroot.cmake"
   VERBATIM
)

set(_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_sysroot_hash "${_generated_dir}/sysroot.sha256")
set(_foundation_manifest "${_generated_dir}/foundation.json")
set(_intrinsic_registry "${CMAKE_CURRENT_SOURCE_DIR}/libraries/contract/include/forge/contract/intrinsics.hpp")
set(_intrinsic_manifest "${_generated_dir}/intrinsics.json")
set(
   _intrinsic_headers
   "${_generated_dir}/include/forge/contract/intrinsics.h"
   "${_generated_dir}/include/forge/contract/internal/intrinsics.hpp"
   "${_generated_dir}/include/forge/contract/types.h"
   "${_generated_dir}/include/eosio/action.h"
   "${_generated_dir}/include/eosio/call.h"
   "${_generated_dir}/include/eosio/chain.h"
   "${_generated_dir}/include/eosio/crypto.h"
   "${_generated_dir}/include/eosio/crypto_bls_ext.h"
   "${_generated_dir}/include/eosio/crypto_ext.h"
   "${_generated_dir}/include/eosio/db.h"
   "${_generated_dir}/include/eosio/instant_finality.h"
   "${_generated_dir}/include/eosio/permission.h"
   "${_generated_dir}/include/eosio/print.h"
   "${_generated_dir}/include/eosio/privileged.h"
   "${_generated_dir}/include/eosio/system.h"
   "${_generated_dir}/include/eosio/transaction.h"
   "${_generated_dir}/include/eosio/types.h"
   "${_generated_dir}/include/eosio/internal/action.hpp"
   "${_generated_dir}/include/eosio/internal/call.hpp"
   "${_generated_dir}/include/eosio/internal/chain.hpp"
   "${_generated_dir}/include/eosio/internal/crypto.hpp"
   "${_generated_dir}/include/eosio/internal/crypto_bls_ext.hpp"
   "${_generated_dir}/include/eosio/internal/crypto_ext.hpp"
   "${_generated_dir}/include/eosio/internal/db.hpp"
   "${_generated_dir}/include/eosio/internal/instant_finality.hpp"
   "${_generated_dir}/include/eosio/internal/permission.hpp"
   "${_generated_dir}/include/eosio/internal/print.hpp"
   "${_generated_dir}/include/eosio/internal/privileged.hpp"
   "${_generated_dir}/include/eosio/internal/system.hpp"
   "${_generated_dir}/include/eosio/internal/transaction.hpp"
)
add_custom_command(
   OUTPUT "${_intrinsic_manifest}" ${_intrinsic_headers}
   COMMAND
      ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_intrinsic_manifest.py"
      --registry "${_intrinsic_registry}"
      --output "${_intrinsic_manifest}"
      --include-dir "${_generated_dir}/include"
   DEPENDS
      "${_intrinsic_registry}"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_intrinsic_manifest.py"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/types.h.in"
   VERBATIM
)
add_custom_target(
   forge_contract_intrinsic_manifest ALL
   DEPENDS "${_intrinsic_manifest}" ${_intrinsic_headers}
)

ExternalProject_Add(
   forge_contract_guest_foundation
   DEPENDS forge_contract_stage_sysroot forge_contract_intrinsic_manifest
   SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake/foundation-build"
   DOWNLOAD_COMMAND ""
   UPDATE_COMMAND ""
   PATCH_COMMAND ""
   CMAKE_GENERATOR Ninja
   CMAKE_ARGS
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM}
      -DCMAKE_INSTALL_PREFIX=${FORGE_CONTRACT_SYSROOT}
      -DCMAKE_SYSTEM_NAME=Generic
      -DCMAKE_SYSTEM_PROCESSOR=wasm32
      -DCMAKE_SYSROOT=${FORGE_CONTRACT_SYSROOT}
      -DCMAKE_OSX_SYSROOT=
      -DCMAKE_C_COMPILER=${FORGE_CONTRACT_CLANG_C}
      -DCMAKE_CXX_COMPILER=${FORGE_CONTRACT_CLANG}
      -DCMAKE_C_COMPILER_TARGET=wasm32
      -DCMAKE_CXX_COMPILER_TARGET=wasm32
      -DCMAKE_C_FLAGS=${_forge_contract_path_map_flags}
      -DCMAKE_CXX_FLAGS=${_forge_contract_path_map_flags}
      -DCMAKE_AR=${FORGE_CONTRACT_LLVM_AR}
      -DCMAKE_RANLIB=${FORGE_CONTRACT_LLVM_RANLIB}
      -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
      -DFORGE_SOURCE_DIR=${_forge_contract_source_root}
      -DFORGE_GUEST_INCLUDE_DIR=${_generated_dir}/include
      -DFORGE_GUEST_LIBC_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/../vendor/cdt-musl
      -DFORGE_GUEST_BOOST_INCLUDE_DIR=${_forge_contract_boost_include}
   BUILD_ALWAYS TRUE
   BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> -j ${FORGE_CONTRACT_JOBS}
   INSTALL_COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR>
)

add_custom_target(
   forge_contract_foundation_manifest ALL
   COMMAND
      ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_foundation_manifest.py"
      --lib-dir "${FORGE_CONTRACT_SYSROOT}/lib"
      --output "${_foundation_manifest}"
   DEPENDS
      forge_contract_guest_foundation
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_foundation_manifest.py"
   BYPRODUCTS "${_foundation_manifest}"
   VERBATIM
)

add_custom_target(
   forge_contract_sysroot_hash ALL
   COMMAND
      ${Python3_EXECUTABLE}
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hash_tree.py"
      --root "${FORGE_CONTRACT_SYSROOT}"
      --output "${_sysroot_hash}"
   DEPENDS forge_contract_foundation_manifest "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hash_tree.py"
   BYPRODUCTS "${_sysroot_hash}"
   VERBATIM
)

configure_file(cmake/host.hpp.in "${_generated_dir}/include/forge/contract/host.hpp" COPYONLY)

set(_forge_contract_prefix_anchor "/__forge_contract_prefix")
set(_forge_contract_config_install_dir
    "${_forge_contract_prefix_anchor}/${CMAKE_INSTALL_LIBDIR}/cmake/ForgeContract")
file(
   RELATIVE_PATH
   FORGE_CONTRACT_PREFIX_FROM_CONFIG_DIR
   "${_forge_contract_config_install_dir}"
   "${_forge_contract_prefix_anchor}"
)
configure_file(
   cmake/ForgeContractToolchain.cmake.in
   "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractToolchain.cmake"
   @ONLY
)
configure_file(
   cmake/ForgeContractPaths.cmake.in
   "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractPaths.cmake"
   @ONLY
)
configure_package_config_file(
   cmake/ForgeContractConfig.cmake.in
   "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractConfig.cmake"
   INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ForgeContract"
   NO_SET_AND_CHECK_MACRO
   NO_CHECK_REQUIRED_COMPONENTS_MACRO
)
write_basic_package_version_file(
   "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractConfigVersion.cmake"
   VERSION ${PROJECT_VERSION}
   COMPATIBILITY SameMajorVersion
   ARCH_INDEPENDENT
)

install(
   FILES
      "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractConfigVersion.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractPaths.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/ForgeContractToolchain.cmake"
      cmake/ForgeContractFunctions.cmake
      cmake/ForgeContractLibraries.cmake
      cmake/ForgeContractBuild.cmake
      cmake/ForgeContractGuestComponents.cmake
   DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ForgeContract"
)
install(DIRECTORY libraries/contract/include/ DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules")
install(
   FILES
      libraries/contract/include/forge/contract/intrinsics.hpp
      libraries/contract/include/forge/contract/serialize.hpp
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/include/forge/contract"
)
install(DIRECTORY libraries/eosio/include/ DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/include")
install(DIRECTORY "${_generated_dir}/include/" DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/include")
install(
   DIRECTORY "${_forge_contract_boost_include}/boost/pfr"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/include/boost"
)
install(
   FILES "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/exceptions/include/forge/exceptions/policy.hpp"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/include/forge/exceptions"
)
install(DIRECTORY examples/ DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/examples")
install(FILES README.md libraries/runtime/PROVENANCE.md DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/forge-contract")
install(
   FILES "${CMAKE_CURRENT_SOURCE_DIR}/../vendor/cdt-musl/COPYRIGHT"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/forge-contract"
   RENAME cdt-musl-COPYRIGHT
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE"
      "${CMAKE_CURRENT_SOURCE_DIR}/../NOTICE"
      "${CMAKE_CURRENT_SOURCE_DIR}/../THIRD_PARTY_LICENSES"
      "${CMAKE_CURRENT_SOURCE_DIR}/../PROVENANCE.md"
   DESTINATION .
)
install(FILES "${_intrinsic_manifest}" DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/registry")
install(FILES "${_sysroot_hash}" "${_foundation_manifest}" DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract")

install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/codec/base64/include/forge/codec/base64/base64.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/codec/base64/include/forge/codec/base64/exceptions.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/codec/base64"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/codec/base58/include/forge/codec/base58/base58.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/codec/base58/include/forge/codec/base58/exceptions.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/codec/base58"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/codec/hex/include/forge/codec/hex/hex.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/codec/hex/include/forge/codec/hex/exceptions.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/codec/hex"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/raw/include/forge/raw/stream.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/raw/include/forge/raw/varint_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/raw/include/forge/raw/codec.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/raw"
)
install(
   FILES "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/db/ids/include/forge/db/ids/typed_id.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/db/ids"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/digest/include/forge/crypto/digest/sha256.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/digest/include/forge/crypto/digest/sha256_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/digest/include/forge/crypto/digest/sha512.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/digest/include/forge/crypto/digest/sha512_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/digest/include/forge/crypto/digest/ripemd160.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/digest/include/forge/crypto/digest/ripemd160_value.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/crypto/digest"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/asymmetric/include/forge/crypto/asymmetric/asymmetric.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/asymmetric/include/forge/crypto/asymmetric/values.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/crypto/asymmetric"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/crypto/bls/include/forge/crypto/bls/bls_values.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/crypto/bls"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/savanna/values/include/forge/chain/savanna/values.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/chain/savanna"
)
install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/values.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/time.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/types_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/types.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/fixed_key.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/fixed_key_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/action_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/action.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/transaction_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/transaction.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/authority_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/authority.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/producer_schedule_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/producer_schedule.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/producer_authority_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/producer_authority.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/system_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/system.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/code_hash_result.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/blockchain_parameters.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/chain_config.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/wasm_parameters.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/ratio.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/elastic_limit_parameters.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/usage_accumulator.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/activated_protocol_feature.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/float64_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/float64.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/float128_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/float128.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/native_ids.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/entity_selector.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/code.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/table.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/currency_stats.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/kv_parameters.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/finalizer_authority.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/finalizer_policy_value.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/finalizer_policy.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/hash_id.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/call_access_mode.cppm"
      "${CMAKE_CURRENT_SOURCE_DIR}/../libraries/chain/protocol/include/forge/chain/protocol/call_data_header.cppm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/chain/protocol"
)

install(DIRECTORY "${FORGE_CONTRACT_SYSROOT}/" DESTINATION sysroot)
install(DIRECTORY "${FORGE_CONTRACT_CLANG_RESOURCE_DIR}/" DESTINATION "lib/clang/22")
install(
   CODE
      "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin\")
       file(COPY_FILE \"${FORGE_CONTRACT_CLANG}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/clang++\" ONLY_IF_DIFFERENT)
       file(COPY_FILE \"${FORGE_CONTRACT_CLANG_SCAN_DEPS}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/clang-scan-deps\" ONLY_IF_DIFFERENT)
       file(COPY_FILE \"${FORGE_CONTRACT_LLVM_AR}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/llvm-ar\" ONLY_IF_DIFFERENT)
       file(COPY_FILE \"${FORGE_CONTRACT_LLVM_RANLIB}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/llvm-ranlib\" ONLY_IF_DIFFERENT)"
)
if(EXISTS "${FORGE_CONTRACT_WASM_LD}")
   file(REAL_PATH "${FORGE_CONTRACT_WASM_LD}" _forge_contract_wasm_ld)
else()
   set(_forge_contract_wasm_ld "${FORGE_CONTRACT_WASM_LD}")
endif()
install(
   CODE
      "file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/wasm-ld\")
       file(COPY_FILE \"${_forge_contract_wasm_ld}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/wasm-ld\")
       file(CHMOD \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/bin/wasm-ld\"
            PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)"
)
if(NOT FORGE_CONTRACT_PROFILE STREQUAL "release")
   function(_forge_contract_install_runtime path)
      file(REAL_PATH "${path}" _runtime_path)
      get_filename_component(_runtime_name "${path}" NAME)
      get_filename_component(_real_name "${_runtime_path}" NAME)
      install(FILES "${_runtime_path}" DESTINATION "${CMAKE_INSTALL_LIBDIR}")
      if(NOT "${_runtime_name}" STREQUAL "${_real_name}")
         install(
            CODE
               "file(MAKE_DIRECTORY \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}\")
                file(REMOVE \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/${_runtime_name}\")
                file(CREATE_LINK \"${_real_name}\" \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/${_runtime_name}\" SYMBOLIC)"
         )
      endif()
   endfunction()

   _forge_contract_install_runtime("${FORGE_CONTRACT_CLANG_CPP_RUNTIME}")
   _forge_contract_install_runtime("${FORGE_CONTRACT_LLVM_RUNTIME}")
   get_filename_component(_forge_contract_wasm_ld_bin "${_forge_contract_wasm_ld}" DIRECTORY)
   file(GLOB _forge_contract_lld_runtime LIST_DIRECTORIES FALSE
        "${_forge_contract_wasm_ld_bin}/../lib/liblld*${CMAKE_SHARED_LIBRARY_SUFFIX}")
   foreach(_runtime IN LISTS _forge_contract_lld_runtime)
      _forge_contract_install_runtime("${_runtime}")
   endforeach()
endif()
configure_file(
   cmake/BundleContractSDK.cmake.in
   "${CMAKE_CURRENT_BINARY_DIR}/BundleContractSDK.cmake"
   @ONLY
)
configure_file(
   cmake/ContractSDKRuntimeDependencies.cmake
   "${CMAKE_CURRENT_BINARY_DIR}/ContractSDKRuntimeDependencies.cmake"
   COPYONLY
)
if(FORGE_CONTRACT_PROFILE STREQUAL "release")
   install(
      DIRECTORY "${_llvm_install}/lib/"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      FILES_MATCHING
         PATTERN "libLLVM*"
         PATTERN "libclang-cpp*"
         PATTERN "liblld*"
   )
   install(DIRECTORY "${_host_tools_install}/bin/" DESTINATION bin USE_SOURCE_PERMISSIONS)
   install(DIRECTORY "${_host_tools_install}/${CMAKE_INSTALL_LIBDIR}/forge-contract/"
           DESTINATION "${CMAKE_INSTALL_LIBDIR}/forge-contract")
   install(
      FILES
         "${_llvm_source}/llvm/LICENSE.TXT"
         "${_llvm_source}/libcxx/LICENSE.TXT"
         "${_llvm_source}/compiler-rt/LICENSE.TXT"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/forge-contract"
   )
endif()
install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/BundleContractSDK.cmake")

if(APPLE)
   set(_forge_contract_package_os macos)
else()
   string(TOLOWER "${CMAKE_HOST_SYSTEM_NAME}" _forge_contract_package_os)
endif()
string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" _forge_contract_package_arch)
set(_forge_contract_package_file_name
    "forge-contract-sdk-${PROJECT_VERSION}-${_forge_contract_package_os}-${_forge_contract_package_arch}")
set(CPACK_GENERATOR TGZ)
set(CPACK_PACKAGE_FILE_NAME "${_forge_contract_package_file_name}")
include(CPack)

set(_forge_contract_archive "${CMAKE_CURRENT_BINARY_DIR}/${_forge_contract_package_file_name}.tar.gz")
set(_forge_contract_archive_dependencies forge_contract_sysroot_hash forge_contract_intrinsic_manifest)
if(TARGET forge_contract_tools)
   list(APPEND _forge_contract_archive_dependencies forge_contract_tools)
else()
   list(
      APPEND _forge_contract_archive_dependencies
      attr_plugin
      abigen
      contract_check
      contract_manifest
   )
endif()
add_custom_target(
   forge_contract_sdk_archive
   COMMAND "${CMAKE_CPACK_COMMAND}" -G TGZ --config "${CMAKE_CURRENT_BINARY_DIR}/CPackConfig.cmake"
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/cmake/hash_file.py"
      --input "${_forge_contract_archive}"
      --output "${_forge_contract_archive}.sha256"
   DEPENDS ${_forge_contract_archive_dependencies}
   BYPRODUCTS "${_forge_contract_archive}" "${_forge_contract_archive}.sha256"
   VERBATIM
)

add_custom_target(
   forge_contract_sdk_relocation
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/tooling/run_relocation.py"
      --archive "${_forge_contract_archive}"
      --cmake "${CMAKE_COMMAND}"
      --cxx-compiler "${CMAKE_CXX_COMPILER}"
      --forge-package "${FORGE_CONTRACT_FORGE_DIR}"
      --source-root "${CMAKE_CURRENT_SOURCE_DIR}/.."
      --dual-target-source "${CMAKE_CURRENT_SOURCE_DIR}/tests/dual_target"
      --output "${CMAKE_CURRENT_BINARY_DIR}/relocation"
   DEPENDS forge_contract_sdk_archive
   VERBATIM
)
