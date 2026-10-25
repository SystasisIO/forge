include(CMakePackageConfigHelpers)

foreach(
   _required
   FORGE_CONTRACT_CONFIG_TEMPLATE
   FORGE_CONTRACT_PATHS_TEMPLATE
   FORGE_CONTRACT_TOOLCHAIN_TEMPLATE
   FORGE_CONTRACT_FUNCTIONS
   FORGE_CONTRACT_LIBRARIES
   FORGE_CONTRACT_BUILD
   FORGE_CONTRACT_GUEST_COMPONENTS
   FORGE_CONTRACT_TEST_ROOT
)
   if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
      message(FATAL_ERROR "${_required} is required")
   endif()
endforeach()

set(PROJECT_VERSION 0.0.0)
set(FORGE_CONTRACT_PROFILE developer)
set(FORGE_CONTRACT_REPRODUCIBLE false)
set(FORGE_CONTRACT_MANIFEST_LLVM_VERSION test)
set(FORGE_CONTRACT_MANIFEST_LLVM_COMMIT test)
set(FORGE_CONTRACT_SYSROOT_SCHEMA_VERSION 1)
set(FORGE_CONTRACT_INTRINSIC_VERSION 1)
set(CMAKE_INSTALL_DATADIR share)
set(CMAKE_INSTALL_LIBDIR "lib/x86_64-linux-gnu")
set(CMAKE_SHARED_MODULE_SUFFIX ".so")

set(_prefix "${FORGE_CONTRACT_TEST_ROOT}/prefix")
set(_config_dir "${_prefix}/${CMAKE_INSTALL_LIBDIR}/cmake/ForgeContract")
set(_prefix_anchor "/__forge_contract_prefix")
file(
   RELATIVE_PATH
   FORGE_CONTRACT_PREFIX_FROM_CONFIG_DIR
   "${_prefix_anchor}/${CMAKE_INSTALL_LIBDIR}/cmake/ForgeContract"
   "${_prefix_anchor}"
)

file(REMOVE_RECURSE "${FORGE_CONTRACT_TEST_ROOT}")
file(MAKE_DIRECTORY "${_config_dir}")
configure_package_config_file(
   "${FORGE_CONTRACT_CONFIG_TEMPLATE}"
   "${_config_dir}/ForgeContractConfig.cmake"
   INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ForgeContract"
   INSTALL_PREFIX "${_prefix}"
   NO_SET_AND_CHECK_MACRO
   NO_CHECK_REQUIRED_COMPONENTS_MACRO
)
configure_file(
   "${FORGE_CONTRACT_PATHS_TEMPLATE}"
   "${_config_dir}/ForgeContractPaths.cmake"
   @ONLY
)
configure_file(
   "${FORGE_CONTRACT_TOOLCHAIN_TEMPLATE}"
   "${_config_dir}/ForgeContractToolchain.cmake"
   @ONLY
)
configure_file("${FORGE_CONTRACT_FUNCTIONS}" "${_config_dir}/ForgeContractFunctions.cmake" COPYONLY)
configure_file("${FORGE_CONTRACT_LIBRARIES}" "${_config_dir}/ForgeContractLibraries.cmake" COPYONLY)
configure_file("${FORGE_CONTRACT_BUILD}" "${_config_dir}/ForgeContractBuild.cmake" COPYONLY)
configure_file(
   "${FORGE_CONTRACT_GUEST_COMPONENTS}"
   "${_config_dir}/ForgeContractGuestComponents.cmake"
   COPYONLY
)

file(MAKE_DIRECTORY "${_prefix}/bin")
foreach(_tool clang++ wasm-ld abigen contract-check contract-manifest)
   file(WRITE "${_prefix}/bin/${_tool}" "")
endforeach()
file(MAKE_DIRECTORY "${_prefix}/sysroot/lib")
foreach(_archive IN ITEMS
   libforge_guest_runtime.a
   libforge_guest_raw.a
   libforge_guest_net_http_values.a
   libforge_guest_api_wasm.a
   libforge_guest_codec_base64.a
   libforge_guest_codec_base58.a
   libforge_guest_codec_hex.a
   libforge_guest_chain_protocol.a
   libforge_guest_crypto_exchange.a
   libforge_guest_contract.a
   libm.a
)
   file(WRITE "${_prefix}/sysroot/lib/${_archive}" "")
endforeach()
file(MAKE_DIRECTORY "${_prefix}/${CMAKE_INSTALL_DATADIR}/forge-contract")
file(WRITE "${_prefix}/${CMAKE_INSTALL_DATADIR}/forge-contract/sysroot.sha256" "test\n")
file(READ "${FORGE_CONTRACT_GUEST_COMPONENTS}" _guest_components)
string(
   REGEX MATCHALL
   "[A-Za-z0-9_./-]+\\.cppm"
   _guest_component_modules
   "${_guest_components}"
)
foreach(_module IN LISTS _guest_component_modules)
   get_filename_component(_module_directory "${_module}" DIRECTORY)
   file(
      MAKE_DIRECTORY
      "${_prefix}/${CMAKE_INSTALL_DATADIR}/forge-contract/modules/${_module_directory}"
   )
   file(
      WRITE
      "${_prefix}/${CMAKE_INSTALL_DATADIR}/forge-contract/modules/${_module}"
      "export module fixture;\n"
   )
endforeach()
foreach(_module IN ITEMS
   forge/db/ids/typed_id.cppm
   forge/chain/protocol/native_ids.cppm
   forge/chain/protocol/code.cppm
   forge/chain/protocol/table.cppm
   forge/chain/protocol/currency_stats.cppm
)
   if(NOT EXISTS "${_prefix}/${CMAKE_INSTALL_DATADIR}/forge-contract/modules/${_module}")
      message(FATAL_ERROR "guest package prefix is missing ${_module}")
   endif()
endforeach()
if(EXISTS "${_prefix}/${CMAKE_INSTALL_DATADIR}/forge-contract/modules/forge/chain/protocol/generated_transaction.cppm")
   message(FATAL_ERROR "host-only generated_transaction leaked into the guest package prefix")
endif()
set(_foundation_manifest "{\n  \"version\": 1,\n  \"archives\": [")
set(_separator "")
foreach(_archive IN ITEMS
   libforge_guest_runtime.a
   libforge_guest_raw.a
   libforge_guest_net_http_values.a
   libforge_guest_api_wasm.a
   libforge_guest_codec_base64.a
   libforge_guest_codec_base58.a
   libforge_guest_codec_hex.a
   libforge_guest_chain_protocol.a
   libforge_guest_crypto_exchange.a
   libforge_guest_contract.a
   libm.a
)
   file(SHA256 "${_prefix}/sysroot/lib/${_archive}" _archive_sha256)
   string(APPEND _foundation_manifest
      "${_separator}\n    {\"name\": \"${_archive}\", \"sha256\": \"${_archive_sha256}\"}")
   set(_separator ",")
endforeach()
string(APPEND _foundation_manifest "\n  ]\n}\n")
file(WRITE "${_prefix}/${CMAKE_INSTALL_DATADIR}/forge-contract/foundation.json" "${_foundation_manifest}")
file(MAKE_DIRECTORY "${_prefix}/${CMAKE_INSTALL_LIBDIR}/forge-contract")
file(WRITE "${_prefix}/${CMAKE_INSTALL_LIBDIR}/forge-contract/attr-plugin${CMAKE_SHARED_MODULE_SUFFIX}" "")

set(_consumer "${FORGE_CONTRACT_TEST_ROOT}/consumer")
set(_repeated_dependency "${FORGE_CONTRACT_TEST_ROOT}/repeated-dependency")
file(MAKE_DIRECTORY "${_consumer}")
file(MAKE_DIRECTORY "${_repeated_dependency}")
file(
   WRITE
   "${_repeated_dependency}/RepeatedDependencyConfig.cmake"
   [=[
include(CMakeFindDependencyMacro)
find_dependency(ForgeContract CONFIG)
]=]
)
file(
   WRITE
   "${_consumer}/CMakeLists.txt"
   [=[
cmake_minimum_required(VERSION 3.31)
project(ForgeContractPackagePrefixTest NONE)
set(FORGE_CONTRACT_GUEST OFF)
find_package(ForgeContract CONFIG REQUIRED)
find_package(RepeatedDependency CONFIG REQUIRED)

if(NOT "${ForgeContract_PREFIX}" STREQUAL "${EXPECTED_PREFIX}")
   message(FATAL_ERROR "ForgeContractConfig resolved the wrong prefix: ${ForgeContract_PREFIX}")
endif()
if(NOT "${ForgeContract_ATTR_PLUGIN}" STREQUAL "${EXPECTED_PLUGIN}")
   message(FATAL_ERROR "ForgeContractConfig resolved the wrong plugin: ${ForgeContract_ATTR_PLUGIN}")
endif()
if(NOT "${CMAKE_CXX_COMPILER}" STREQUAL "${EXPECTED_PREFIX}/bin/clang++")
   message(FATAL_ERROR "ForgeContractToolchain resolved the wrong compiler: ${CMAKE_CXX_COMPILER}")
endif()
foreach(_archive_variable IN ITEMS
   ForgeContract_RUNTIME_ARCHIVE
   ForgeContract_CODEC_BASE64_ARCHIVE
   ForgeContract_CODEC_BASE58_ARCHIVE
   ForgeContract_CODEC_HEX_ARCHIVE
)
   if(DEFINED ${_archive_variable})
      message(FATAL_ERROR "ForgeContractConfig exposes internal archive variable ${_archive_variable}")
   endif()
endforeach()
]=]
)

execute_process(
   COMMAND
      "${CMAKE_COMMAND}"
      -S "${_consumer}"
      -B "${FORGE_CONTRACT_TEST_ROOT}/build"
      -DForgeContract_DIR=${_config_dir}
      -DRepeatedDependency_DIR=${_repeated_dependency}
      -DCMAKE_TOOLCHAIN_FILE=${_config_dir}/ForgeContractToolchain.cmake
      -DEXPECTED_PREFIX=${_prefix}
      -DEXPECTED_PLUGIN=${_prefix}/${CMAKE_INSTALL_LIBDIR}/forge-contract/attr-plugin${CMAKE_SHARED_MODULE_SUFFIX}
   RESULT_VARIABLE _configure_result
)
if(NOT _configure_result EQUAL 0)
   message(FATAL_ERROR "nested-libdir ForgeContract consumer configuration failed")
endif()

set(_source_helper_consumer "${FORGE_CONTRACT_TEST_ROOT}/source-helper-consumer")
file(MAKE_DIRECTORY "${_source_helper_consumer}/guest")
file(WRITE "${_source_helper_consumer}/guest/CMakeLists.txt" "cmake_minimum_required(VERSION 3.31)\nproject(guest NONE)\n")
set(_source_helper_cmake [=[
cmake_minimum_required(VERSION 3.31)
project(ForgeContractSourceHelperPrefixTest NONE)
set(ForgeContract_DIR "@_config_dir@")
include("@FORGE_CONTRACT_FUNCTIONS@")
forge_add_contract_project(
   fixture_guest
   SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/guest"
   BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/guest-build"
   CONTRACT fixture
)
get_target_property(_resolved_prefix fixture_guest FORGE_CONTRACT_SDK_PREFIX)
if(NOT _resolved_prefix STREQUAL "@_prefix@")
   message(FATAL_ERROR "source helper resolved the wrong SDK prefix: ${_resolved_prefix}")
endif()
]=])
string(CONFIGURE "${_source_helper_cmake}" _source_helper_cmake @ONLY)
file(WRITE "${_source_helper_consumer}/CMakeLists.txt" "${_source_helper_cmake}")
execute_process(
   COMMAND
      "${CMAKE_COMMAND}"
      -S "${_source_helper_consumer}"
      -B "${FORGE_CONTRACT_TEST_ROOT}/source-helper-build"
   RESULT_VARIABLE _source_helper_result
)
if(NOT _source_helper_result EQUAL 0)
   message(FATAL_ERROR "source helper failed to resolve the nested-libdir SDK prefix")
endif()

foreach(_tampered_archive IN ITEMS libforge_guest_raw.a libforge_guest_net_http_values.a libforge_guest_api_wasm.a libforge_guest_crypto_exchange.a)
   file(APPEND "${_prefix}/sysroot/lib/${_tampered_archive}" "tampered")
   execute_process(
      COMMAND
         "${CMAKE_COMMAND}"
         -S "${_consumer}"
         -B "${FORGE_CONTRACT_TEST_ROOT}/tampered-${_tampered_archive}-build"
         -DForgeContract_DIR=${_config_dir}
         -DRepeatedDependency_DIR=${_repeated_dependency}
         -DCMAKE_TOOLCHAIN_FILE=${_config_dir}/ForgeContractToolchain.cmake
         -DEXPECTED_PREFIX=${_prefix}
         -DEXPECTED_PLUGIN=${_prefix}/${CMAKE_INSTALL_LIBDIR}/forge-contract/attr-plugin${CMAKE_SHARED_MODULE_SUFFIX}
      RESULT_VARIABLE _tampered_result
      OUTPUT_VARIABLE _tampered_output
      ERROR_VARIABLE _tampered_error
   )
   if(_tampered_result EQUAL 0)
      message(FATAL_ERROR "ForgeContractConfig accepted a foundation archive with a mismatched checksum")
   endif()
   string(FIND "${_tampered_output}${_tampered_error}" "archive checksum failed" _checksum_error)
   if(_checksum_error EQUAL -1)
      message(FATAL_ERROR "ForgeContractConfig did not report the foundation checksum failure")
   endif()
   file(WRITE "${_prefix}/sysroot/lib/${_tampered_archive}" "")
endforeach()
