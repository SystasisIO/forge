cmake_minimum_required(VERSION 3.31)

project(forge VERSION 8.29.1 LANGUAGES C CXX ASM)

include(GNUInstallDirs)
include(CheckCXXSourceCompiles)
include(CMakePackageConfigHelpers)
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(ForgeVendorCompilePolicy)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
   include(CTest)
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FORGE_ENABLE_MODULES "Build FORGE public module interfaces when supported" ON)
option(FORGE_ENABLE_VENDOR_TESTS "Build donor dependency tests and benchmarks" OFF)
option(FORGE_ENABLE_TUI "Build FORGE Notcurses-backed terminal UI library when notcurses-core is available" ON)
option(FORGE_ENABLE_LIBP2P_INTEROP "Run live go-libp2p/rust-libp2p interoperability tests" OFF)
option(FORGE_ENABLE_ROCKSDB "Build RocksDB-backed optional FORGE features when RocksDB is available" ON)
option(FORGE_ENABLE_MDBX "Build the vendored MDBX-backed FORGE database driver" ON)
option(FORGE_ENABLE_TOOLING "Build optional Forge Contract Tooling host libraries" OFF)
option(FORGE_ENABLE_S3 "Build the optional AWS SDK-backed S3 client" OFF)
set(FORGE_ENABLE_OIDC_AGENT "AUTO" CACHE STRING "Build the oidc-agent authentication backend: AUTO, ON or OFF")
set_property(CACHE FORGE_ENABLE_OIDC_AGENT PROPERTY STRINGS AUTO ON OFF)
if(NOT FORGE_ENABLE_OIDC_AGENT MATCHES "^(AUTO|ON|OFF)$")
   message(FATAL_ERROR "FORGE_ENABLE_OIDC_AGENT must be AUTO, ON or OFF")
endif()

set(FORGE_BUILD_PROFILE "full" CACHE STRING "Forge build profile")
set_property(CACHE FORGE_BUILD_PROFILE PROPERTY STRINGS full contract-sdk-host)
if(NOT FORGE_BUILD_PROFILE STREQUAL "full" AND NOT FORGE_BUILD_PROFILE STREQUAL "contract-sdk-host")
   message(FATAL_ERROR "Unknown FORGE_BUILD_PROFILE: ${FORGE_BUILD_PROFILE}")
endif()

if(FORGE_ENABLE_OIDC_AGENT STREQUAL "ON" AND
   (NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR FORGE_BUILD_PROFILE STREQUAL "contract-sdk-host"))
   message(FATAL_ERROR "FORGE_ENABLE_OIDC_AGENT=ON is supported only on the Linux full profile")
endif()

set(FORGE_CONTRACT_SDK_HOST_PROFILE OFF)
if(FORGE_BUILD_PROFILE STREQUAL "contract-sdk-host")
   set(FORGE_CONTRACT_SDK_HOST_PROFILE ON)
   set(FORGE_ENABLE_TOOLING ON CACHE BOOL "Build optional Forge Contract Tooling host libraries" FORCE)
   set(FORGE_ENABLE_ROCKSDB OFF CACHE BOOL "Build RocksDB-backed optional FORGE features" FORCE)
   set(FORGE_ENABLE_MDBX OFF CACHE BOOL "Build the vendored MDBX-backed Forge database driver" FORCE)
   set(FORGE_ENABLE_TUI OFF CACHE BOOL "Build Forge terminal UI library" FORCE)
   set(FORGE_ENABLE_LIBP2P_INTEROP OFF CACHE BOOL "Run libp2p interoperability tests" FORCE)
   set(FORGE_ENABLE_OIDC_AGENT OFF CACHE STRING "Build the oidc-agent authentication backend" FORCE)
endif()

if(FORGE_CONTRACT_SDK_HOST_PROFILE)
   file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}" _forge_contract_host_source_root)
   add_compile_options(
      "$<BUILD_INTERFACE:-ffile-prefix-map=${_forge_contract_host_source_root}=.>"
      "$<BUILD_INTERFACE:-fdebug-prefix-map=${_forge_contract_host_source_root}=.>"
      "$<BUILD_INTERFACE:-ffile-prefix-map=${CMAKE_BINARY_DIR}=./build>"
      "$<BUILD_INTERFACE:-fdebug-prefix-map=${CMAKE_BINARY_DIR}=./build>"
   )
endif()

set(FORGE_LIBP2P_DONORS_ROOT "" CACHE PATH "Path to workspace donors directory containing go-libp2p, rust-libp2p and libp2p-specs")
if(NOT FORGE_LIBP2P_DONORS_ROOT)
   set(_forge_default_donors_root "/Users/vladimirtarnakin/.openclaw/workspace/Projects/Storlane/donors")
   if(EXISTS "${_forge_default_donors_root}")
      set(FORGE_LIBP2P_DONORS_ROOT "${_forge_default_donors_root}" CACHE PATH "Path to workspace donors directory containing go-libp2p, rust-libp2p and libp2p-specs" FORCE)
   endif()
endif()

if(FORGE_ENABLE_MODULES AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
   message(FATAL_ERROR "FORGE_ENABLE_MODULES=ON requires module-capable Homebrew LLVM clang++; AppleClang is not a supported FORGE module toolchain")
endif()

if("${CMAKE_GENERATOR}" STREQUAL "Ninja")
   add_compile_options(-fdiagnostics-color=always)
endif()

if(APPLE)
   message(STATUS "Configuring Forge on macOS")
   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
else()
   message(STATUS "Configuring Forge on Linux")
   set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall")
   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall")
   set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -latomic")
endif()

add_compile_definitions(BOOST_TEST_DEFAULTS_TO_CORE_DUMP)
add_compile_definitions(BOOST_UNORDERED_DISABLE_NEON)

set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
set(THREADS_PREFER_PTHREAD_FLAG TRUE)
find_package(Threads REQUIRED)

set(_forge_boost_components headers date_time iostreams)
if(NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   list(APPEND _forge_boost_components charconv url program_options)
endif()
find_package(Boost 1.90 REQUIRED CONFIG COMPONENTS ${_forge_boost_components})
find_package(ZLIB REQUIRED)
if(NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   find_package(boost_stacktrace_basic CONFIG QUIET)
   check_cxx_source_compiles(
      "#include <stacktrace>
      #ifndef __cpp_lib_stacktrace
      #error no std stacktrace feature macro
      #endif
      int main() { auto trace = std::stacktrace::current(); return static_cast<int>(trace.size()); }"
      FORGE_HAS_STD_STACKTRACE
   )
   if(FORGE_HAS_STD_STACKTRACE)
      message(STATUS "FORGE log stacktrace backend: std::stacktrace")
   elseif(TARGET Boost::stacktrace_basic)
      message(STATUS "FORGE log stacktrace backend: Boost::stacktrace_basic")
   else()
      message(STATUS "FORGE log stacktrace backend: unavailable")
   endif()
endif()

if(FORGE_CONTRACT_SDK_HOST_PROFILE)
   find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
else()
   find_package(OpenSSL 3 REQUIRED COMPONENTS SSL Crypto)
   find_package(Ngtcp2 REQUIRED)
endif()

if(APPLE AND NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   enable_language(OBJC OBJCXX)
   find_library(FORGE_APPAUTH_APPKIT_FRAMEWORK AppKit REQUIRED)
   find_library(FORGE_APPAUTH_AUTHENTICATION_SERVICES_FRAMEWORK AuthenticationServices REQUIRED)
   find_library(FORGE_APPAUTH_CORE_SERVICES_FRAMEWORK CoreServices REQUIRED)
   find_library(FORGE_APPAUTH_FOUNDATION_FRAMEWORK Foundation REQUIRED)
   file(
      GLOB FORGE_APPAUTH_CORE_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor/appauth/Sources/AppAuthCore/*.m"
   )
   file(
      GLOB_RECURSE FORGE_APPAUTH_MACOS_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor/appauth/Sources/AppAuth/macOS/*.m"
   )
   add_library(
      appauth-internal STATIC
      ${FORGE_APPAUTH_CORE_SOURCES}
      ${FORGE_APPAUTH_MACOS_SOURCES}
   )
   add_library(AppAuth::AppAuth ALIAS appauth-internal)
   set(AppAuth_VERSION 3.0.0)
   set_target_properties(
      appauth-internal PROPERTIES
      EXPORT_NAME appauth_internal
      MACOSX_DEPLOYMENT_TARGET "12.0"
      POSITION_INDEPENDENT_CODE ON
   )
   target_include_directories(
      appauth-internal SYSTEM PUBLIC
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/appauth/Sources>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/appauth/Sources/AppAuthCore>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/appauth/Sources/AppAuth/macOS>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/appauth/Sources/AppAuth/macOS/LoopbackHTTPServer>"
   )
   target_compile_options(appauth-internal PRIVATE -fobjc-arc)
   target_link_libraries(
      appauth-internal PUBLIC
      ${FORGE_APPAUTH_APPKIT_FRAMEWORK}
      ${FORGE_APPAUTH_AUTHENTICATION_SERVICES_FRAMEWORK}
      ${FORGE_APPAUTH_CORE_SERVICES_FRAMEWORK}
      ${FORGE_APPAUTH_FOUNDATION_FRAMEWORK}
   )
   forge_apply_vendored_implementation_policy(appauth-internal)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT FORGE_CONTRACT_SDK_HOST_PROFILE AND
   NOT FORGE_ENABLE_OIDC_AGENT STREQUAL "OFF")
   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND)
      pkg_check_modules(OIDC_AGENT QUIET IMPORTED_TARGET GLOBAL liboidc-agent)
      if(NOT TARGET PkgConfig::OIDC_AGENT)
         pkg_check_modules(OIDC_AGENT QUIET IMPORTED_TARGET GLOBAL oidc-agent)
      endif()
   endif()
   if(TARGET PkgConfig::OIDC_AGENT AND OIDC_AGENT_VERSION VERSION_EQUAL "5.3.8")
      add_library(OIDCAgent::client ALIAS PkgConfig::OIDC_AGENT)
      set(OIDCAgent_VERSION "${OIDC_AGENT_VERSION}")
      message(STATUS "Forge oidc-agent backend: ${OIDC_AGENT_VERSION}")
   elseif(FORGE_ENABLE_OIDC_AGENT STREQUAL "ON")
      message(FATAL_ERROR "FORGE_ENABLE_OIDC_AGENT=ON requires liboidc-agent 5.3.8")
   else()
      message(STATUS "Skipping Forge oidc-agent backend: exact liboidc-agent 5.3.8 not found")
   endif()
endif()

set(FORGE_HAS_ROCKSDB OFF)
if(FORGE_ENABLE_ROCKSDB AND NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   find_package(RocksDB QUIET)
   if(TARGET RocksDB::rocksdb)
      set(FORGE_HAS_ROCKSDB ON)
      message(STATUS "FORGE RocksDB backend: RocksDB::rocksdb")
   else()
      message(STATUS "Skipping FORGE RocksDB-backed features: RocksDB not found")
   endif()
else()
   message(STATUS "FORGE RocksDB-backed features: disabled")
endif()
find_package(glaze 7.5 CONFIG REQUIRED)
if(TARGET glaze::glaze)
   message(STATUS "FORGE codec backend: glaze::glaze")
else()
   message(FATAL_ERROR "Glaze was found, but target glaze::glaze is not available")
endif()

set(FORGE_HAS_TUI OFF)
set(FORGE_TUI_NOTCURSES_TARGET "")
if(FORGE_ENABLE_TUI AND NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   find_package(Notcurses CONFIG QUIET)
   if(TARGET Notcurses::notcurses-core)
      set(FORGE_TUI_NOTCURSES_TARGET Notcurses::notcurses-core)
   elseif(TARGET notcurses::notcurses-core)
      set(FORGE_TUI_NOTCURSES_TARGET notcurses::notcurses-core)
   elseif(TARGET notcurses-core)
      set(FORGE_TUI_NOTCURSES_TARGET notcurses-core)
   endif()

   if(NOT FORGE_TUI_NOTCURSES_TARGET)
      find_package(PkgConfig QUIET)
      if(PkgConfig_FOUND)
         pkg_check_modules(NOTCURSES_CORE QUIET IMPORTED_TARGET notcurses-core)
         if(TARGET PkgConfig::NOTCURSES_CORE)
            set(FORGE_TUI_NOTCURSES_TARGET PkgConfig::NOTCURSES_CORE)
         endif()
      endif()
   endif()

   if(FORGE_TUI_NOTCURSES_TARGET)
      set(FORGE_HAS_TUI ON)
      message(STATUS "FORGE TUI backend: ${FORGE_TUI_NOTCURSES_TARGET}")
   else()
      message(STATUS "Skipping forge_tui: notcurses-core not found; install notcurses-core or set FORGE_ENABLE_TUI=OFF")
   endif()
endif()

function(forge_add_header_only_boost_target component)
   if(NOT TARGET Boost::${component})
      add_library(Boost::${component} INTERFACE IMPORTED)
      set_property(
         TARGET Boost::${component}
         APPEND PROPERTY INTERFACE_LINK_LIBRARIES Boost::headers
      )
   endif()
endfunction()

forge_add_header_only_boost_target(asio)
forge_add_header_only_boost_target(beast)
forge_add_header_only_boost_target(crc)
forge_add_header_only_boost_target(dll)
forge_add_header_only_boost_target(multi_index)
forge_add_header_only_boost_target(multiprecision)

if(NOT TARGET Boost::included_unit_test_framework)
   add_library(Boost::included_unit_test_framework INTERFACE IMPORTED)
   set_property(
      TARGET Boost::included_unit_test_framework
      APPEND PROPERTY INTERFACE_LINK_LIBRARIES Boost::headers
   )
endif()

find_package(Git QUIET)
set(FORGE_GIT_REVISION_SHA "unknown")
set(FORGE_GIT_REVISION_UNIX_TIMESTAMP 0)

if(Git_FOUND AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/.git")
   execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE FORGE_GIT_REVISION_SHA
      RESULT_VARIABLE _forge_git_rev_parse_result
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
   if(NOT _forge_git_rev_parse_result EQUAL 0)
      set(FORGE_GIT_REVISION_SHA "unknown")
   endif()

   execute_process(
      COMMAND "${GIT_EXECUTABLE}" log -1 --format=%ct
      WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
      OUTPUT_VARIABLE FORGE_GIT_REVISION_UNIX_TIMESTAMP
      RESULT_VARIABLE _forge_git_log_result
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET
   )
   if(NOT _forge_git_log_result EQUAL 0)
      set(FORGE_GIT_REVISION_UNIX_TIMESTAMP 0)
   endif()
endif()

find_path(GMP_INCLUDE_DIR NAMES gmp.h)
find_library(GMP_LIBRARY gmp)
if(NOT GMP_LIBRARY MATCHES ${CMAKE_SHARED_LIBRARY_SUFFIX})
   message(FATAL_ERROR "GMP shared library not found")
endif()
message(STATUS "GMP: ${GMP_LIBRARY}, ${GMP_INCLUDE_DIR}")
add_library(GMP::gmp SHARED IMPORTED)
set_target_properties(
   GMP::gmp PROPERTIES
   IMPORTED_LOCATION ${GMP_LIBRARY}
   INTERFACE_INCLUDE_DIRECTORIES ${GMP_INCLUDE_DIR}
)

set(
   FORGE_SOURCE_INCLUDE_DIRS
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/core/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/chrono/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/exceptions/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/reflect/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/schema/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/variant/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/raw/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/compression/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/chain/core/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/chain/protocol/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/chain/transaction/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/vm/wasm/interpret/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/ids/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/core/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/mdbx/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/rocksdb/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/object/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/blob/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/revision/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/db/authenticated/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/codec/base32/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/codec/base64/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/codec/base58/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/codec/hex/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/codec/json/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/config/core/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/codec/yaml/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/codec/xml/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/config/program_options/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/config/env/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/core/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/auth/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/http/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/stream/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/transport/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/quic/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/websocket/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/api/p2p/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/log/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/otlp/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/rocksdb/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/core/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/digest/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/symmetric/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/asymmetric/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/signer/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/keystore/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/pki/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/math/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/bls/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/bls/signer/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/crypto/bn256/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/multiformats/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/asio/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/cli/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/http/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/s3/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/websocket/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/transport/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/tcp/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/tls/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/yamux/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/quic/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/stcp/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/p2p/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/net/pnet/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/auth/oauth2/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/auth/workload/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/auth/appauth/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/auth/oidc_agent/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/auth/pairing/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/auth/session/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/auth/http/include"
   "${CMAKE_CURRENT_SOURCE_DIR}/libraries/app/include"
)

if(FORGE_ENABLE_TOOLING)
   find_package(Clang 22.1 CONFIG REQUIRED)
   list(APPEND
      FORGE_SOURCE_INCLUDE_DIRS
      "${CMAKE_CURRENT_SOURCE_DIR}/libraries/tooling/abi/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/libraries/tooling/attributes/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/libraries/tooling/validation/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/libraries/tooling/manifest/include"
      "${CMAKE_CURRENT_SOURCE_DIR}/libraries/tooling/testing/include"
   )
endif()

set(FORGE_PLUGINS_CHAIN_SIGNER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/chain/signer/include")
set(FORGE_PLUGINS_CRYPTO_SECRETS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/crypto/secrets/include")
set(FORGE_PLUGINS_NET_P2P_NODE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/net/p2p/node/include")
set(FORGE_PLUGINS_NET_P2P_RESOLVER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/net/p2p/resolver/include")
set(FORGE_PLUGINS_NET_P2P_DIAGNOSTICS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/net/p2p/diagnostics/include")
set(FORGE_PLUGINS_NET_P2P_PUBSUB_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/net/p2p/pubsub/include")
set(FORGE_PLUGINS_NET_HTTP_SERVER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/net/http/server/include")
set(FORGE_PLUGINS_LOG_OTLP_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/log/otlp/include")
set(FORGE_PLUGINS_DB_STORE_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/plugins/db/store/include")

if(FORGE_ENABLE_TUI AND FORGE_TUI_NOTCURSES_TARGET)
   list(APPEND FORGE_SOURCE_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/libraries/tui/include")
endif()

set(FORGE_PUBLIC_INCLUDE_DIRS)
foreach(_forge_include_dir IN LISTS FORGE_SOURCE_INCLUDE_DIRS)
   list(APPEND FORGE_PUBLIC_INCLUDE_DIRS "$<BUILD_INTERFACE:${_forge_include_dir}>")
endforeach()
list(APPEND FORGE_PUBLIC_INCLUDE_DIRS "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")

function(forge_target_modules_at target module_path)
   file(
      GLOB _forge_target_module_files CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/include/forge/${module_path}/*.cppm"
   )
   if(NOT _forge_target_module_files)
      message(FATAL_ERROR "No public FORGE module interfaces found for ${target} in ${CMAKE_CURRENT_SOURCE_DIR}/include/forge/${module_path}")
   endif()

   string(REPLACE "/" "_" _forge_module_var "${module_path}")
   foreach(_forge_disabled_module IN LISTS FORGE_${_forge_module_var}_DISABLED_MODULES)
      list(FILTER _forge_target_module_files EXCLUDE REGEX "/${_forge_disabled_module}\\.cppm$")
   endforeach()

   target_sources(
      ${target}
      PUBLIC
         FILE_SET CXX_MODULES
         BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
         FILES ${_forge_target_module_files}
   )
   target_compile_features(${target} PUBLIC cxx_std_23)
endfunction()

function(forge_target_modules target lib)
   forge_target_modules_at(${target} ${lib})
endfunction()


add_library(secp256k1-internal INTERFACE)
target_include_directories(
   secp256k1-internal INTERFACE
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/secp256k1/src>"
)
target_compile_definitions(
   secp256k1-internal INTERFACE
      ENABLE_MODULE_RECOVERY=1
      COMB_BLOCKS=43
      COMB_TEETH=6
      ECMULT_WINDOW_SIZE=15
      SECP256K1_STATIC=1
)
if(CMAKE_SYSTEM_PROCESSOR STREQUAL x86_64)
   target_compile_definitions(secp256k1-internal INTERFACE USE_ASM_X86_64=1)
endif()
target_compile_options(secp256k1-internal INTERFACE -Wno-unused-function)

add_library(
   secp256k1 STATIC
   vendor/secp256k1/src/secp256k1.c
   vendor/secp256k1/src/precomputed_ecmult.c
   vendor/secp256k1/src/precomputed_ecmult_gen.c
)
target_include_directories(
   secp256k1 PUBLIC
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/secp256k1>"
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/secp256k1/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)
target_link_libraries(secp256k1 PRIVATE secp256k1-internal)

set(FORGE_HAS_MDBX ${FORGE_ENABLE_MDBX})
if(FORGE_ENABLE_MDBX)
   add_library(mdbx-internal STATIC vendor/libmdbx/mdbx.c)
   set_target_properties(
      mdbx-internal PROPERTIES
      C_STANDARD 11
      C_STANDARD_REQUIRED ON
      C_EXTENSIONS OFF
      EXPORT_NAME db_mdbx_internal
      POSITION_INDEPENDENT_CODE ON
   )
   target_include_directories(
      mdbx-internal PRIVATE
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libmdbx"
   )
   target_compile_definitions(
      mdbx-internal PRIVATE
         MDBX_BUILD_SHARED_LIBRARY=0
         MDBX_BUILD_TIMESTAMP="2026-05-14T18:24:33Z"
         MDBX_BUILD_TARGET="forge-vendored"
         MDBX_BUILD_TYPE="vendored"
         MDBX_BUILD_FLAGS="Forge-CMake"
         MDBX_BUILD_METADATA="forge-v0.14.2"
   )
   target_link_libraries(mdbx-internal PUBLIC Threads::Threads)
endif()

# c-ares remains a private, static implementation detail of forge_net_dns.
# Disable every upstream install, tool, test and shared-library surface so the
# Forge package never depends on a system c-ares package or exports its C API.
if(NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   cmake_policy(PUSH)
   if(POLICY CMP0077)
      cmake_policy(SET CMP0077 NEW)
   endif()

   # c-ares 1.34.8 resets ordinary CARES_* variables under its legacy policy.
   # Cache values make this private, static-only integration deterministic.
   set(CARES_STATIC ON CACHE BOOL "Build Forge's private static c-ares target" FORCE)
   set(CARES_SHARED OFF CACHE BOOL "Disable shared c-ares targets" FORCE)
   set(CARES_INSTALL OFF CACHE BOOL "Disable c-ares installation rules" FORCE)
   set(CARES_BUILD_TESTS OFF CACHE BOOL "Disable c-ares tests" FORCE)
   set(CARES_BUILD_TOOLS OFF CACHE BOOL "Disable c-ares tools" FORCE)
   set(CARES_SYMBOL_HIDING ON CACHE BOOL "Hide c-ares symbols" FORCE)
   add_subdirectory(vendor/c-ares EXCLUDE_FROM_ALL)
   cmake_policy(POP)
   set_target_properties(
      c-ares PROPERTIES
      EXPORT_NAME net_dns_cares_internal
      C_VISIBILITY_PRESET hidden
      POSITION_INDEPENDENT_CODE ON
      INTERFACE_INCLUDE_DIRECTORIES ""
   )
endif()

# Keep the XSalsa20 subset private to forge_crypto_symmetric. The upstream
# sources remain unmodified under vendor/libsodium/upstream; the only Forge C
# source is the narrow integration shim that prevents the full libsodium API
# from entering the target graph.
add_library(
   forge_crypto_symmetric_xsalsa20_vendor OBJECT
   vendor/libsodium/integration/xsalsa20_support.c
   vendor/libsodium/upstream/src/libsodium/crypto_stream/xsalsa20/stream_xsalsa20.c
   vendor/libsodium/upstream/src/libsodium/crypto_stream/salsa20/ref/salsa20_ref.c
   vendor/libsodium/upstream/src/libsodium/crypto_core/salsa/ref/core_salsa_ref.c
   vendor/libsodium/upstream/src/libsodium/crypto_core/hsalsa20/core_hsalsa20.c
   vendor/libsodium/upstream/src/libsodium/crypto_core/hsalsa20/ref2/core_hsalsa20_ref2.c
)
set_target_properties(
   forge_crypto_symmetric_xsalsa20_vendor PROPERTIES
   C_STANDARD 11
   C_STANDARD_REQUIRED ON
   C_EXTENSIONS OFF
   C_VISIBILITY_PRESET hidden
   POSITION_INDEPENDENT_CODE ON
)
target_include_directories(
   forge_crypto_symmetric_xsalsa20_vendor PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libsodium/upstream/src/libsodium/include/sodium"
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libsodium/upstream/src/libsodium/crypto_stream/salsa20"
)
target_compile_definitions(
   forge_crypto_symmetric_xsalsa20_vendor PRIVATE
      CONFIGURED=1
      SODIUM_STATIC=1
      crypto_core_hsalsa20=forge_xsalsa20_private_crypto_core_hsalsa20
      crypto_core_hsalsa20_constbytes=forge_xsalsa20_private_crypto_core_hsalsa20_constbytes
      crypto_core_hsalsa20_inputbytes=forge_xsalsa20_private_crypto_core_hsalsa20_inputbytes
      crypto_core_hsalsa20_keybytes=forge_xsalsa20_private_crypto_core_hsalsa20_keybytes
      crypto_core_hsalsa20_outputbytes=forge_xsalsa20_private_crypto_core_hsalsa20_outputbytes
      crypto_core_salsa20=forge_xsalsa20_private_crypto_core_salsa20
      crypto_core_salsa20_constbytes=forge_xsalsa20_private_crypto_core_salsa20_constbytes
      crypto_core_salsa20_inputbytes=forge_xsalsa20_private_crypto_core_salsa20_inputbytes
      crypto_core_salsa20_keybytes=forge_xsalsa20_private_crypto_core_salsa20_keybytes
      crypto_core_salsa20_outputbytes=forge_xsalsa20_private_crypto_core_salsa20_outputbytes
      crypto_core_salsa2012=forge_xsalsa20_private_crypto_core_salsa2012
      crypto_core_salsa2012_constbytes=forge_xsalsa20_private_crypto_core_salsa2012_constbytes
      crypto_core_salsa2012_inputbytes=forge_xsalsa20_private_crypto_core_salsa2012_inputbytes
      crypto_core_salsa2012_keybytes=forge_xsalsa20_private_crypto_core_salsa2012_keybytes
      crypto_core_salsa2012_outputbytes=forge_xsalsa20_private_crypto_core_salsa2012_outputbytes
      crypto_core_salsa208=forge_xsalsa20_private_crypto_core_salsa208
      crypto_core_salsa208_constbytes=forge_xsalsa20_private_crypto_core_salsa208_constbytes
      crypto_core_salsa208_inputbytes=forge_xsalsa20_private_crypto_core_salsa208_inputbytes
      crypto_core_salsa208_keybytes=forge_xsalsa20_private_crypto_core_salsa208_keybytes
      crypto_core_salsa208_outputbytes=forge_xsalsa20_private_crypto_core_salsa208_outputbytes
      crypto_stream_salsa20=forge_xsalsa20_private_crypto_stream_salsa20
      crypto_stream_salsa20_ref_implementation=forge_xsalsa20_private_crypto_stream_salsa20_ref_implementation
      crypto_stream_salsa20_xor_ic=forge_xsalsa20_private_crypto_stream_salsa20_xor_ic
      crypto_stream_xsalsa20=forge_xsalsa20_private_crypto_stream_xsalsa20
      crypto_stream_xsalsa20_keybytes=forge_xsalsa20_private_crypto_stream_xsalsa20_keybytes
      crypto_stream_xsalsa20_keygen=forge_xsalsa20_private_crypto_stream_xsalsa20_keygen
      crypto_stream_xsalsa20_messagebytes_max=forge_xsalsa20_private_crypto_stream_xsalsa20_messagebytes_max
      crypto_stream_xsalsa20_noncebytes=forge_xsalsa20_private_crypto_stream_xsalsa20_noncebytes
      crypto_stream_xsalsa20_xor=forge_xsalsa20_private_crypto_stream_xsalsa20_xor
      crypto_stream_xsalsa20_xor_ic=forge_xsalsa20_private_crypto_stream_xsalsa20_xor_ic
      randombytes_buf=forge_xsalsa20_private_randombytes_buf
      sodium_memzero=forge_xsalsa20_private_sodium_memzero
)

if(FORGE_ENABLE_VENDOR_TESTS)
   add_executable(secp256k1-bench vendor/secp256k1/src/bench.c)
   target_link_libraries(secp256k1-bench secp256k1 secp256k1-internal)

   add_executable(secp256k1-tests vendor/secp256k1/src/tests.c)
   target_link_libraries(secp256k1-tests secp256k1 secp256k1-internal)
   add_test(NAME secp256k1-tests COMMAND secp256k1-tests)

   add_executable(secp256k1-exhaustive-tests vendor/secp256k1/src/tests_exhaustive.c)
   target_link_libraries(secp256k1-exhaustive-tests secp256k1 secp256k1-internal)
   add_test(NAME secp256k1-exhaustive-tests COMMAND secp256k1-exhaustive-tests)
endif()

add_subdirectory(vendor/bn256/src)
add_subdirectory(vendor/bls12-381 EXCLUDE_FROM_ALL)
add_subdirectory(vendor/softfloat EXCLUDE_FROM_ALL)
set_target_properties(
   softfloat PROPERTIES
   EXPORT_NAME vm_wasm_interpret_softfloat_internal
   INTERFACE_INCLUDE_DIRECTORIES
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/softfloat/source/include>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/softfloat/source/8086-SSE>;$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/softfloat/build/Linux-x86_64-GCC>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_LIBDIR}/forge/internal/vm_wasm_interpret/softfloat>"
)
set_property(
   TARGET bls12-381 PROPERTY INTERFACE_INCLUDE_DIRECTORIES
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/vendor/bls12-381/include>"
      "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
)

set(
   FORGE_VENDORED_IMPLEMENTATION_TARGETS
   secp256k1
   bn256
   bls12-381
   softfloat
   forge_crypto_symmetric_xsalsa20_vendor
)
if(TARGET c-ares)
   list(APPEND FORGE_VENDORED_IMPLEMENTATION_TARGETS c-ares)
endif()
foreach(_forge_vendor_target IN LISTS FORGE_VENDORED_IMPLEMENTATION_TARGETS)
   forge_apply_vendored_implementation_policy(${_forge_vendor_target})
endforeach()
if(TARGET mdbx-internal)
   forge_apply_vendored_implementation_policy(mdbx-internal DISABLED_SANITIZERS alignment)
endif()

if(NOT FORGE_ENABLE_VENDOR_TESTS)
   file(
      WRITE "${CMAKE_CURRENT_BINARY_DIR}/CTestCustom.cmake"
      "set(CTEST_CUSTOM_TESTS_IGNORE bls12-381 bls12-381-nobmi2)\n"
   )
endif()

function(forge_contract_register_guest_component)
   cmake_parse_arguments(
      ARG
      "FOUNDATION"
      "ID;TARGET;ARCHIVE"
      "MODULES;MODULE_NAMES;PUBLIC_LIBRARIES"
      ${ARGN}
   )
   if(
      ARG_UNPARSED_ARGUMENTS
      OR NOT ARG_ID
      OR NOT ARG_MODULES
      OR NOT ARG_MODULE_NAMES
   )
      message(FATAL_ERROR "invalid Forge Contract guest component declaration")
   endif()
   list(LENGTH ARG_MODULES _module_count)
   list(LENGTH ARG_MODULE_NAMES _module_name_count)
   if(NOT _module_count EQUAL _module_name_count)
      message(FATAL_ERROR "Forge Contract guest component ${ARG_ID} has mismatched module metadata")
   endif()
   string(SHA256 _key "${ARG_ID}")
   get_property(_existing GLOBAL PROPERTY "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_ID")
   if(_existing)
      message(FATAL_ERROR "duplicate Forge Contract guest component ID: ${ARG_ID}")
   endif()
   set_property(GLOBAL PROPERTY "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_ID" "${ARG_ID}")
   set_property(
      GLOBAL PROPERTY "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_TARGET"
      "${ARG_TARGET}"
   )
   set_property(
      GLOBAL PROPERTY "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_MODULE_NAMES"
      "${ARG_MODULE_NAMES}"
   )
   set_property(
      GLOBAL PROPERTY "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_DEPENDENCIES"
      "${ARG_PUBLIC_LIBRARIES}"
   )
   set_property(
      GLOBAL PROPERTY "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_FOUNDATION"
      "${ARG_FOUNDATION}"
   )
endfunction()

include("${CMAKE_CURRENT_SOURCE_DIR}/guest/cmake/ForgeContractGuestComponents.cmake")

function(forge_target_contract_guest_component target)
   cmake_parse_arguments(ARG "" "ID" "" ${ARGN})
   if(ARG_UNPARSED_ARGUMENTS OR NOT ARG_ID)
      message(FATAL_ERROR "forge_target_contract_guest_component(${target}) requires ID")
   endif()
   if(NOT TARGET "${target}")
      message(FATAL_ERROR "Forge guest component target does not exist: ${target}")
   endif()
   if(NOT ARG_ID MATCHES "^forge\\.[a-z0-9_]+(\\.[a-z0-9_]+)*$")
      message(FATAL_ERROR "Forge guest component ID is not canonical: ${ARG_ID}")
   endif()
   get_target_property(_existing "${target}" FORGE_CONTRACT_GUEST_COMPONENT_ID)
   if(_existing AND NOT _existing STREQUAL ARG_ID)
      message(FATAL_ERROR "Forge target ${target} already has guest component ID ${_existing}")
   endif()
   get_target_property(_exported "${target}" EXPORT_PROPERTIES)
   if(_exported STREQUAL "_exported-NOTFOUND")
      set(_exported)
   endif()
   string(SHA256 _key "${ARG_ID}")
   get_property(_descriptor_id GLOBAL PROPERTY "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_ID")
   get_property(
      _descriptor_target GLOBAL PROPERTY
      "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_TARGET"
   )
   get_property(
      _module_names GLOBAL PROPERTY
      "FORGE_CONTRACT_GUEST_DESCRIPTOR_${_key}_MODULE_NAMES"
   )
   if(
      NOT _descriptor_id STREQUAL ARG_ID
      OR NOT _descriptor_target STREQUAL target
      OR NOT _module_names
   )
      message(FATAL_ERROR "Forge target ${target} has no guest component descriptor for ${ARG_ID}")
   endif()
   if(TARGET "Forge::${target}")
      get_target_property(_alias_target "Forge::${target}" ALIASED_TARGET)
      if(NOT "${_alias_target}" STREQUAL "${target}")
         message(FATAL_ERROR "Forge build-tree target Forge::${target} is already owned by ${_alias_target}")
      endif()
   else()
      add_library("Forge::${target}" ALIAS "${target}")
   endif()
   list(
      APPEND _exported
      FORGE_CONTRACT_GUEST_COMPONENT_ID
   )
   list(REMOVE_DUPLICATES _exported)
   set_target_properties(
      "${target}"
      PROPERTIES
         FORGE_CONTRACT_GUEST_COMPONENT_ID "${ARG_ID}"
         EXPORT_PROPERTIES "${_exported}"
   )
endfunction()

add_subdirectory(libraries/core)
add_subdirectory(libraries/chrono)
add_subdirectory(libraries/exceptions)
add_subdirectory(libraries/codec/base32)
add_subdirectory(libraries/codec/base64)
add_subdirectory(libraries/codec/base58)
add_subdirectory(libraries/codec/hex)
add_subdirectory(libraries/reflect)
add_subdirectory(libraries/schema)
add_subdirectory(libraries/variant)
add_subdirectory(libraries/raw)
add_subdirectory(libraries/compression)
add_subdirectory(libraries/chain)
add_subdirectory(libraries/vm)
add_subdirectory(libraries/db/ids)
add_subdirectory(libraries/db/core)
add_subdirectory(libraries/db/object)
add_subdirectory(libraries/config/core)
add_subdirectory(libraries/codec/json)
add_subdirectory(libraries/crypto)
add_subdirectory(libraries/asio)

set(
   FORGE_LEAF_TARGETS
   forge_core
   forge_chrono
   forge_exceptions
   forge_reflect
   forge_schema
   forge_variant
   forge_raw
   forge_compression
   forge_chain_core
   forge_chain_savanna_values
   forge_chain_protocol
   forge_chain_transaction
   forge_chain_quorum
   forge_chain_fork
   forge_chain_savanna
   forge_vm_wasm_interpret
   forge_db_ids
   forge_db_core
   forge_db_object
   forge_config_core
   forge_codec_base32
   forge_codec_base64
   forge_codec_base58
   forge_codec_hex
   forge_codec_json
   forge_crypto_core
   forge_crypto_digest
   forge_crypto_symmetric
   forge_crypto_asymmetric_values
   forge_crypto_asymmetric
   forge_crypto_signer
   forge_crypto_keystore
   forge_crypto_pki
   forge_crypto_math
   forge_crypto_bls_values
   forge_crypto_bls
   forge_crypto_bn256
   forge_asio
)

if(NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   add_subdirectory(libraries/db/blob)
   add_subdirectory(libraries/db/revision)
   add_subdirectory(libraries/db/authenticated)
   add_subdirectory(libraries/config/env)
   add_subdirectory(libraries/config/program_options)
   add_subdirectory(libraries/codec/yaml)
   add_subdirectory(libraries/codec/xml)
   add_subdirectory(libraries/api)
   add_subdirectory(libraries/log)
   add_subdirectory(libraries/multiformats)
   add_subdirectory(libraries/otlp)
   if(FORGE_HAS_ROCKSDB)
      add_subdirectory(libraries/rocksdb)
      add_subdirectory(libraries/db/rocksdb)
   endif()
   if(FORGE_HAS_MDBX)
      add_subdirectory(libraries/db/mdbx)
   endif()
   add_subdirectory(libraries/net)
   add_subdirectory(libraries/auth)
   add_subdirectory(libraries/cli)
   add_subdirectory(libraries/app)
   add_subdirectory(plugins)
   if(FORGE_ENABLE_TUI AND FORGE_TUI_NOTCURSES_TARGET)
      add_subdirectory(libraries/tui)
   endif()

   list(
      APPEND FORGE_LEAF_TARGETS
      forge_db_blob
      forge_db_revision
      forge_db_authenticated
      forge_chain_api
      forge_codec_yaml
      forge_codec_xml
      forge_config_program_options
      forge_config_env
      forge_api_core
      forge_api_auth
      forge_api_stream
      forge_api_transport
      forge_api_quic
      forge_api_websocket
      forge_api_p2p
      forge_log
      forge_otlp
      forge_multiformats
      forge_net_tls
      forge_net_http
      forge_auth_pairing
      forge_auth_oauth2
      forge_auth_workload
      forge_auth_session
      forge_auth_http
      forge_api_http
      forge_net_websocket
      forge_net_transport
      forge_net_tcp
      forge_net_dns
      forge_net_stcp
      forge_net_yamux
      forge_net_quic
      forge_net_pnet
      forge_net_p2p
      forge_cli
      forge_app
      forge_crypto_bls_signer
      forge_plugins_chain_signer
      forge_plugins_crypto_secrets
      forge_plugins_crypto_wallet
      forge_crypto_wallet
      forge_plugins_net_p2p_node
      forge_plugins_net_p2p_resolver
      forge_plugins_net_p2p_diagnostics
      forge_plugins_net_p2p_pubsub
      forge_plugins_net_http_server
      forge_plugins_log_otlp
      forge_plugins_db_store
      forge_plugins
   )
   if(TARGET forge_auth_appauth)
      list(APPEND FORGE_LEAF_TARGETS forge_auth_appauth)
   endif()
   if(TARGET forge_auth_oidc_agent)
      list(APPEND FORGE_LEAF_TARGETS forge_auth_oidc_agent)
   endif()
endif()

if(FORGE_ENABLE_TOOLING)
   add_subdirectory(libraries/tooling)
endif()

if(TARGET forge_rocksdb)
   list(APPEND FORGE_LEAF_TARGETS forge_rocksdb)
endif()

if(TARGET forge_net_s3)
   list(APPEND FORGE_LEAF_TARGETS forge_net_s3)
endif()

if(TARGET forge_db_rocksdb)
   list(APPEND FORGE_LEAF_TARGETS forge_db_rocksdb)
endif()

if(TARGET forge_db_mdbx)
   list(APPEND FORGE_LEAF_TARGETS forge_db_mdbx)
endif()

if(TARGET forge_tui)
   list(APPEND FORGE_LEAF_TARGETS forge_tui)
endif()

if(FORGE_ENABLE_TOOLING)
   list(
      APPEND FORGE_LEAF_TARGETS
      forge_tooling_abi
      forge_tooling_attributes
      forge_tooling_validation
      forge_tooling_manifest
      forge_tooling_testing
   )
endif()

foreach(_forge_leaf_target IN LISTS FORGE_LEAF_TARGETS)
   if(TARGET "Forge::${_forge_leaf_target}")
      get_target_property(_forge_alias_target "Forge::${_forge_leaf_target}" ALIASED_TARGET)
      if(NOT "${_forge_alias_target}" STREQUAL "${_forge_leaf_target}")
         message(FATAL_ERROR
            "Forge build-tree target Forge::${_forge_leaf_target} is already owned by ${_forge_alias_target}"
         )
      endif()
   else()
      add_library("Forge::${_forge_leaf_target}" ALIAS "${_forge_leaf_target}")
   endif()
endforeach()

set(
   FORGE_BUILT_COMPONENTS
   core
   chrono
   exceptions
   reflect
   schema
   variant
   raw
   db_ids
   compression
   chain_core
   chain_savanna_values
   chain_protocol
   chain_transaction
   chain_quorum
   chain_fork
   chain_savanna
   vm_wasm_interpret
   db_core
   db_object
   config_core
   codec_base32
   codec_base64
   codec_base58
   codec_hex
   codec_json
   crypto_core
   crypto_digest
   crypto_symmetric
   crypto_asymmetric_values
   crypto_asymmetric
   crypto_signer
   crypto_keystore
   crypto_pki
   crypto_math
   crypto_bls_values
   crypto_bls
   crypto_bn256
   asio
)
if(NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   list(
      APPEND FORGE_BUILT_COMPONENTS
      db_blob
      db_revision
      db_authenticated
      chain_api
      codec_yaml
      codec_xml
      config_program_options
      config_env
      api_core
      api_auth
      api_stream
      api_transport
      api_quic
      api_websocket
      api_p2p
      log
      otlp
      multiformats
      net_transport
      net_tcp
      net_dns
      net_tls
      net_stcp
      net_yamux
      auth_oauth2
      auth_workload
      auth_pairing
      auth_session
      auth_http
      app
      net_http
      api_http
      net_websocket
      net_quic
      net_pnet
      net_p2p
      cli
      crypto_bls_signer
      crypto_wallet
      plugins_chain_signer
      plugins_crypto_secrets
      plugins_crypto_wallet
      plugins_net_p2p_node
      plugins_net_p2p_resolver
      plugins_net_p2p_diagnostics
      plugins_net_p2p_pubsub
      plugins_net_http_server
      plugins_log_otlp
      plugins_db_store
      plugins
   )
   if(TARGET forge_auth_appauth)
      list(APPEND FORGE_BUILT_COMPONENTS auth_appauth)
   endif()
   if(TARGET forge_auth_oidc_agent)
      list(APPEND FORGE_BUILT_COMPONENTS auth_oidc_agent)
   endif()
endif()
if(TARGET forge_rocksdb)
   list(APPEND FORGE_BUILT_COMPONENTS rocksdb)
endif()

if(TARGET forge_net_s3)
   list(APPEND FORGE_BUILT_COMPONENTS net_s3)
endif()
if(TARGET forge_db_rocksdb)
   list(APPEND FORGE_BUILT_COMPONENTS db_rocksdb)
endif()
if(TARGET forge_db_mdbx)
   list(APPEND FORGE_BUILT_COMPONENTS db_mdbx)
endif()
if(TARGET forge_plugins_db_rocksdb)
   list(APPEND FORGE_BUILT_COMPONENTS plugins_db_rocksdb)
endif()
if(TARGET forge_tui)
   list(APPEND FORGE_BUILT_COMPONENTS tui)
endif()
if(FORGE_ENABLE_TOOLING)
   list(
      APPEND FORGE_BUILT_COMPONENTS
      tooling_abi
      tooling_attributes
      tooling_validation
      tooling_manifest
      tooling_testing
   )
endif()

# forge_chain_api owns the contract-table projection verifier used by the portable client.
set(FORGE_HAS_PORTABLE_VERIFIED_CLIENT OFF)
if(TARGET forge_chain_savanna AND TARGET forge_chain_api)
   set(FORGE_HAS_PORTABLE_VERIFIED_CLIENT ON)
endif()

add_library(forge STATIC forge.cpp)
add_library(Forge::forge ALIAS forge)

target_include_directories(forge PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge PUBLIC
      ${FORGE_LEAF_TARGETS}
)

# Imported targets are SYSTEM by default. Preserve Forge's include precedence so
# an unrelated machine-wide installation cannot shadow headers from this package.
set_property(TARGET forge ${FORGE_LEAF_TARGETS} PROPERTY EXPORT_NO_SYSTEM TRUE)

if(DEFINED BUILD_TESTING AND BUILD_TESTING AND NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   add_subdirectory(tests)
endif()

install(
   TARGETS forge ${FORGE_LEAF_TARGETS}
   EXPORT ForgeTargets
   LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} COMPONENT dev EXCLUDE_FROM_ALL
   FILE_SET CXX_MODULES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL
)

set(FORGE_INTERNAL_TARGETS secp256k1-internal secp256k1 bn256 bls12-381 softfloat)
if(TARGET appauth-internal)
   list(APPEND FORGE_INTERNAL_TARGETS appauth-internal)
endif()
if(TARGET mdbx-internal)
   list(APPEND FORGE_INTERNAL_TARGETS mdbx-internal)
endif()
if(TARGET c-ares)
   list(APPEND FORGE_INTERNAL_TARGETS c-ares)
endif()

install(
   TARGETS ${FORGE_INTERNAL_TARGETS}
   EXPORT ForgeTargets
   LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT dev EXCLUDE_FROM_ALL
   PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/bls12-381 COMPONENT dev EXCLUDE_FROM_ALL
)

set(
   FORGE_INSTALL_INCLUDE_DIRS
   libraries/core/include/forge
   libraries/chrono/include/forge
   libraries/exceptions/include/forge
   libraries/reflect/include/forge
   libraries/schema/include/forge
   libraries/variant/include/forge
   libraries/raw/include/forge
   libraries/db/ids/include/forge
   libraries/compression/include/forge
   libraries/chain/core/include/forge
   libraries/chain/protocol/include/forge
   libraries/chain/transaction/include/forge
   libraries/vm/wasm/interpret/include/forge
   libraries/db/core/include/forge
   libraries/db/object/include/forge
   libraries/codec/base32/include/forge
   libraries/codec/base64/include/forge
   libraries/codec/base58/include/forge
   libraries/codec/hex/include/forge
   libraries/codec/json/include/forge
   libraries/config/core/include/forge
   libraries/crypto/core/include/forge
   libraries/crypto/digest/include/forge
   libraries/crypto/symmetric/include/forge
   libraries/crypto/asymmetric/include/forge
   libraries/crypto/signer/include/forge
   libraries/crypto/keystore/include/forge
   libraries/crypto/pki/include/forge
   libraries/crypto/math/include/forge
   libraries/crypto/bls/include/forge
   libraries/crypto/bn256/include/forge
   libraries/asio/include/forge
)
if(NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
   list(
      APPEND FORGE_INSTALL_INCLUDE_DIRS
      libraries/db/blob/include/forge
      libraries/db/revision/include/forge
      libraries/db/authenticated/include/forge
      libraries/chain/api/include/forge
      libraries/codec/yaml/include/forge
      libraries/codec/xml/include/forge
      libraries/config/program_options/include/forge
      libraries/config/env/include/forge
      libraries/api/core/include/forge
      libraries/api/auth/include/forge
      libraries/api/http/include/forge
      libraries/api/stream/include/forge
      libraries/api/transport/include/forge
      libraries/api/quic/include/forge
      libraries/api/websocket/include/forge
      libraries/api/p2p/include/forge
      libraries/log/include/forge
      libraries/otlp/include/forge
      libraries/multiformats/include/forge
      libraries/net/http/include/forge
      libraries/net/websocket/include/forge
      libraries/net/transport/include/forge
      libraries/net/tcp/include/forge
      libraries/net/dns/include/forge
      libraries/net/tls/include/forge
      libraries/net/yamux/include/forge
      libraries/net/quic/include/forge
      libraries/net/stcp/include/forge
      libraries/net/pnet/include/forge
      libraries/crypto/bls/signer/include/forge
      libraries/auth/oauth2/include/forge
      libraries/auth/workload/include/forge
      libraries/auth/pairing/include/forge
      libraries/auth/session/include/forge
      libraries/auth/http/include/forge
      libraries/net/p2p/include/forge
      libraries/cli/include/forge
      libraries/app/include/forge
      plugins/chain/signer/include/forge
      plugins/crypto/secrets/include/forge
      plugins/net/p2p/node/include/forge
      plugins/net/p2p/resolver/include/forge
      plugins/net/p2p/diagnostics/include/forge
      plugins/net/p2p/pubsub/include/forge
      plugins/net/http/server/include/forge
      plugins/log/otlp/include/forge
      plugins/db/store/include/forge
   )
   if(TARGET forge_auth_appauth)
      list(APPEND FORGE_INSTALL_INCLUDE_DIRS libraries/auth/appauth/include/forge)
   endif()
   if(TARGET forge_auth_oidc_agent)
      list(APPEND FORGE_INSTALL_INCLUDE_DIRS libraries/auth/oidc_agent/include/forge)
   endif()
endif()
if(TARGET forge_net_s3)
   list(APPEND FORGE_INSTALL_INCLUDE_DIRS libraries/net/s3/include/forge)
endif()
install(
   DIRECTORY ${FORGE_INSTALL_INCLUDE_DIRS}
   DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL
)

if(FORGE_ENABLE_TOOLING)
   install(
      DIRECTORY libraries/tooling/abi/include/forge
                libraries/tooling/attributes/include/forge
                libraries/tooling/validation/include/forge
                libraries/tooling/manifest/include/forge
                libraries/tooling/testing/include/forge
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL
   )
endif()

if(TARGET forge_rocksdb)
   install(
      DIRECTORY libraries/rocksdb/include/forge
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL
   )
endif()

if(TARGET forge_db_rocksdb)
   install(
      DIRECTORY libraries/db/rocksdb/include/forge
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL
   )
endif()

if(TARGET forge_db_mdbx)
   install(
      DIRECTORY libraries/db/mdbx/include/forge
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL
   )
endif()

if(TARGET forge_tui)
   install(
      DIRECTORY libraries/tui/include/forge
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT dev EXCLUDE_FROM_ALL
   )
endif()

install(
   DIRECTORY vendor/secp256k1/include/
   DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
   COMPONENT dev
   EXCLUDE_FROM_ALL
)
install(
   DIRECTORY vendor/bls12-381/include/
   DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

install(
   DIRECTORY vendor/bn256/include/
   DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

install(
   DIRECTORY vendor/softfloat/source/include/
   DESTINATION ${CMAKE_INSTALL_LIBDIR}/forge/internal/vm_wasm_interpret/softfloat
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
      "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE"
      "${CMAKE_CURRENT_SOURCE_DIR}/AUTHORS"
      "${CMAKE_CURRENT_SOURCE_DIR}/PROVENANCE.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/THIRD_PARTY_LICENSES"
      "${CMAKE_CURRENT_SOURCE_DIR}/libraries/vm/wasm/interpret/LICENSE.eos-vm"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge"
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

if(TARGET mdbx-internal)
   install(
      FILES
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libmdbx/LICENSE"
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libmdbx/NOTICE"
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libmdbx/COPYRIGHT"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/libmdbx"
      COMPONENT dev
      EXCLUDE_FROM_ALL
   )
endif()

if(TARGET c-ares)
   install(
      FILES
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/c-ares/LICENSE.md"
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/c-ares/AUTHORS"
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/c-ares/MANIFEST.md"
         "${CMAKE_CURRENT_SOURCE_DIR}/vendor/c-ares/TREE.SHA256"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/c-ares"
      COMPONENT dev
      EXCLUDE_FROM_ALL
   )
endif()

install(
   FILES
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libsodium/upstream/LICENSE"
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libsodium/MANIFEST.md"
      "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libsodium/TREE.SHA256"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/libsodium"
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

install(
   FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/CLI11/LICENSE"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/CLI11"
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

if(TARGET appauth-internal)
   install(
      FILES "${CMAKE_CURRENT_SOURCE_DIR}/vendor/appauth/LICENSE"
      DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/AppAuth"
      COMPONENT dev
      EXCLUDE_FROM_ALL
   )
endif()

install(
   EXPORT ForgeTargets
   FILE ForgeTargets.cmake
   NAMESPACE Forge::
   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Forge
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

# Keep the configured build-tree package usable by focused component consumers
# without requiring a full install of every Forge leaf.
export(
   EXPORT ForgeTargets
   FILE "${CMAKE_CURRENT_BINARY_DIR}/ForgeTargets.cmake"
   NAMESPACE Forge::
)

configure_package_config_file(
   "${CMAKE_CURRENT_SOURCE_DIR}/cmake/ForgeConfig.cmake.in"
   "${CMAKE_CURRENT_BINARY_DIR}/ForgeConfig.cmake"
   INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Forge"
)
write_basic_package_version_file(
   "${CMAKE_CURRENT_BINARY_DIR}/ForgeConfigVersion.cmake"
   VERSION ${PROJECT_VERSION}
   COMPATIBILITY SameMajorVersion
)
install(
   FILES
      "${CMAKE_CURRENT_BINARY_DIR}/ForgeConfig.cmake"
      "${CMAKE_CURRENT_BINARY_DIR}/ForgeConfigVersion.cmake"
   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Forge
   COMPONENT dev
   EXCLUDE_FROM_ALL
)

if(FORGE_ENABLE_TOOLING)
   get_property(_forge_tooling_testing_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
   string(REPLACE ";" "\\;" _forge_tooling_testing_configuration_types "${CMAKE_CONFIGURATION_TYPES}")
   set(_forge_tooling_testing_package_prefix "${CMAKE_BINARY_DIR}/forge-tooling-testing-package")
   set(_forge_tooling_testing_relocated_prefix "${CMAKE_BINARY_DIR}/forge-tooling-testing-relocated")
   set(_forge_tooling_testing_consumer_build "${CMAKE_BINARY_DIR}/forge-tooling-testing-consumer")
   set(
      _forge_tooling_testing_package_test
      "${CMAKE_COMMAND}"
      "-DFORGE_PACKAGE_TEST_INSTALL_PREFIX=${_forge_tooling_testing_package_prefix}"
      "-DFORGE_PACKAGE_TEST_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}"
      "-DFORGE_PACKAGE_TEST_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}"
      "-DFORGE_PACKAGE_TEST_RELOCATED_PREFIX=${_forge_tooling_testing_relocated_prefix}"
      "-DFORGE_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/tests/package_tooling_testing_component"
      "-DFORGE_PACKAGE_TEST_BINARY_DIR=${_forge_tooling_testing_consumer_build}"
      "-DFORGE_PACKAGE_TEST_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DFORGE_PACKAGE_TEST_BUILD_ROOT=${CMAKE_BINARY_DIR}"
      "-DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}"
      "-DFORGE_PACKAGE_TEST_CONFIG=$<CONFIG>"
      "-DFORGE_PACKAGE_TEST_MULTI_CONFIG=${_forge_tooling_testing_multi_config}"
      "-DFORGE_PACKAGE_TEST_CONFIGURATION_TYPES=${_forge_tooling_testing_configuration_types}"
      "-DFORGE_PACKAGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DFORGE_PACKAGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DFORGE_PACKAGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/package_tooling_testing_component/verify_relocation.cmake"
   )
   add_custom_target(
      test_forge_package_tooling_testing_component
      COMMAND ${_forge_tooling_testing_package_test}
      DEPENDS forge
      VERBATIM
   )
   if(BUILD_TESTING AND NOT FORGE_CONTRACT_SDK_HOST_PROFILE)
      add_test(
         NAME test_forge_package_tooling_testing_component
         COMMAND ${_forge_tooling_testing_package_test}
      )
      set_tests_properties(test_forge_package_tooling_testing_component PROPERTIES RUN_SERIAL TRUE)
   endif()
endif()
install(
   DIRECTORY cmake/
   DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Forge/modules
   COMPONENT dev
   EXCLUDE_FROM_ALL
   FILES_MATCHING
      PATTERN "*.cmake"
      PATTERN "ForgeConfig.cmake.in" EXCLUDE
)
