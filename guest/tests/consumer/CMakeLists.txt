cmake_minimum_required(VERSION 3.31)

project(ForgeContractConsumerTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(
   FORGE_CONTRACT_SOURCE_ROOT
   "${CMAKE_CURRENT_SOURCE_DIR}/.."
   CACHE PATH
   "Canonical source root for the installed Contract SDK consumer fixtures"
)

find_package(ForgeContract CONFIG REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_library(
   forge_contract_host_intrinsic_fixture
   OBJECT
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/host_intrinsics.cpp"
)
target_include_directories(
   forge_contract_host_intrinsic_fixture
   PRIVATE
      "${ForgeContract_DATA_DIR}/include"
)

forge_add_contract(
   hello
   SOURCES hello.cpp
)

forge_add_contract(
   minimal
   SOURCES minimal.cpp
)

forge_add_contract(
   guestmacro
   SOURCES guest_macro_contract.cpp asset_source_compat.cpp
   DISPATCH_SOURCE guest_macro_contract.cpp
   COMPILE_CHECKS wire_aliases.cpp
   COMPILE_DEFINITIONS
      CONSUMER_DECLARATION_VALUE=42
      "CONSUMER_DECLARATION_TEXT=\"quoted \\\"value\\\" with space\""
)

forge_add_contract(
   recordtest
   SOURCES record_contract.cpp
)

forge_add_contract(
   equivalent
   SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.."
   SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/equivalent_struct.cpp"
)

forge_add_contract(
   multisource
   SOURCES multi_source_helper.cpp multi_source.cpp
   DISPATCH_SOURCE multi_source.cpp
)

forge_add_contract(
   allocatortst
   SOURCES allocator_contract.cpp
)

forge_add_contract(
   donorcompat
   SOURCES donor_compatibility.cpp
)

forge_add_contract(
   dispatchshdw
   SOURCES dispatch_shadow.cpp dispatch_shadow_helper.cpp
   DISPATCH_SOURCE dispatch_shadow.cpp
)

forge_add_contract(
   legacyhello
   SOURCES legacy_hello.cpp
)

forge_add_contract(
   legacynotify
   SOURCES legacy_notification.cpp
)

forge_add_contract(
   dbhost
   SOURCES db_host_contract.cpp
)

forge_add_contract(
   oracle
   SOURCES oracle_contract.cpp
)

forge_add_contract(
   recovery
   SOURCES recovery_contract.cpp
)

forge_add_contract(
   looping
   SOURCES looping_contract.cpp
)

file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/sdk_surface_manifest.json" _sdk_surface_json)
set(_surface_source_root "${CMAKE_CURRENT_BINARY_DIR}/surface-source")
set(_surface_generated_dir "${_surface_source_root}/checks")
file(MAKE_DIRECTORY "${_surface_generated_dir}")
configure_file(
   "${CMAKE_CURRENT_SOURCE_DIR}/surface_contract.cpp"
   "${_surface_source_root}/surface_contract.cpp"
   COPYONLY
)
set(
   _surface_sources
   asset_source_compat.cpp
   eosio_datastream_adl.cpp
   eosio_internal_db_header.cpp
   wire_aliases.cpp
)
set(_surface_staged_sources)
foreach(_source IN LISTS _surface_sources)
   configure_file(
      "${CMAKE_CURRENT_SOURCE_DIR}/${_source}"
      "${_surface_generated_dir}/${_source}"
      COPYONLY
   )
   list(APPEND _surface_staged_sources "checks/${_source}")
endforeach()

string(JSON _surface_header_count LENGTH "${_sdk_surface_json}" donor_cpp_headers)
math(EXPR _surface_header_last "${_surface_header_count} - 1")
foreach(_index RANGE 0 ${_surface_header_last})
   string(JSON SURFACE_HEADER GET "${_sdk_surface_json}" donor_cpp_headers ${_index})
   string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _stem "${SURFACE_HEADER}")
   set(_output "${_surface_generated_dir}/header_${_stem}.cpp")
   configure_file(surface_header.cpp.in "${_output}" @ONLY)
   list(APPEND _surface_staged_sources "checks/header_${_stem}.cpp")
endforeach()

string(JSON _surface_module_count LENGTH "${_sdk_surface_json}" modern_modules)
math(EXPR _surface_module_last "${_surface_module_count} - 1")
foreach(_index RANGE 0 ${_surface_module_last})
   string(JSON _leaf GET "${_sdk_surface_json}" modern_modules ${_index})
   if(_leaf STREQUAL "contract")
      set(SURFACE_MODULE "forge.contract")
   else()
      set(SURFACE_MODULE "forge.contract.${_leaf}")
   endif()
   string(REPLACE "." "_" _stem "${SURFACE_MODULE}")
   set(_output "${_surface_generated_dir}/module_${_stem}.cpp")
   configure_file(surface_module.cpp.in "${_output}" @ONLY)
   list(APPEND _surface_staged_sources "checks/module_${_stem}.cpp")
endforeach()

forge_add_contract(
   surface
   SOURCE_ROOT "${_surface_source_root}"
   SOURCES surface_contract.cpp
   COMPILE_CHECKS ${_surface_staged_sources}
   COMPILE_DEFINITIONS
      CONSUMER_DECLARATION_VALUE=42
      "CONSUMER_DECLARATION_TEXT=\"quoted \\\"value\\\" with space\""
)

forge_add_contract(
   multiidx
   SOURCES multi_index_contract.cpp
)

forge_add_contract(
   legacymidx
   SOURCES legacy_multi_index_contract.cpp
)

forge_add_contract(
   midxerrors
   SOURCES multi_index_errors_contract.cpp
)

forge_add_contract(
   legmidxerr
   SOURCES legacy_multi_index_errors_contract.cpp
)

forge_add_contract(
   midxextend
   SOURCES multi_index_extended_contract.cpp
)

forge_add_contract(
   legmidxext
   SOURCES legacy_multi_index_extended_contract.cpp
)

add_custom_target(
   forge_contract_multi_index_fixtures ALL
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/run_multi_index_fixtures.py"
      --mapping "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/multi_index_donor_mapping.json"
      --abi-pair "$<TARGET_GENEX_EVAL:multiidx,$<TARGET_PROPERTY:multiidx,FORGE_CONTRACT_ABI_FILE>>" "$<TARGET_GENEX_EVAL:legacymidx,$<TARGET_PROPERTY:legacymidx,FORGE_CONTRACT_ABI_FILE>>"
      --abi-pair "$<TARGET_GENEX_EVAL:midxerrors,$<TARGET_PROPERTY:midxerrors,FORGE_CONTRACT_ABI_FILE>>" "$<TARGET_GENEX_EVAL:legmidxerr,$<TARGET_PROPERTY:legmidxerr,FORGE_CONTRACT_ABI_FILE>>"
      --abi-pair "$<TARGET_GENEX_EVAL:midxextend,$<TARGET_PROPERTY:midxextend,FORGE_CONTRACT_ABI_FILE>>" "$<TARGET_GENEX_EVAL:legmidxext,$<TARGET_PROPERTY:legmidxext,FORGE_CONTRACT_ABI_FILE>>"
   DEPENDS multiidx legacymidx midxerrors legmidxerr midxextend legmidxext
   VERBATIM
)

set(_db_intrinsics_wasm "${CMAKE_CURRENT_BINARY_DIR}/dbintrinsics.wasm")
add_custom_target(
   forge_contract_database_intrinsic_fixtures ALL
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/run_intrinsic_fixtures.py"
      --manifest "${ForgeContract_DATA_DIR}/registry/intrinsics.json"
      --golden "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/spring_db_intrinsics.txt"
      --full-golden "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/spring_intrinsics.txt"
      --eosio-db-header "${ForgeContract_DATA_DIR}/include/eosio/db.h"
      --cdt-fixture "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/cdt/capi/db.c"
   COMMAND
      "${ForgeContract_CLANG}"
      --target=wasm32
      --sysroot=${ForgeContract_SYSROOT}
      -x c
      -ffreestanding
      -nostdlib
      -fno-ident
      -mcpu=mvp
      -I${ForgeContract_DATA_DIR}/include
      -Wl,--no-entry
      -Wl,--export=test_db
      -Wl,--allow-undefined
      -Wl,--strip-all
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/cdt/capi/db.c"
      -o "${_db_intrinsics_wasm}"
   BYPRODUCTS "${_db_intrinsics_wasm}"
   DEPENDS forge_contract_host_intrinsic_fixture
   VERBATIM
)

set(_tooling_output "${CMAKE_CURRENT_BINARY_DIR}/tooling")
set(_tooling_build_dir "${CMAKE_CURRENT_BINARY_DIR}")
add_custom_target(
   forge_contract_tooling_fixtures ALL
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/run_abigen_fixtures.py"
      --abigen "${ForgeContract_ABIGEN}"
      --plugin "${ForgeContract_ATTR_PLUGIN}"
      --sysroot "${ForgeContract_SYSROOT}"
      --include "${ForgeContract_DATA_DIR}/include"
      --modules "${ForgeContract_DATA_DIR}/modules"
      --build-dir "${_tooling_build_dir}"
      --fixtures "${CMAKE_CURRENT_SOURCE_DIR}/../tooling"
      --output "${_tooling_output}"
   DEPENDS hello
   VERBATIM
)

add_custom_target(
   forge_contract_checker_fixtures ALL
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/run_checker_fixtures.py"
      --checker "${ForgeContract_CHECK}"
      --manifest-tool "${ForgeContract_MANIFEST}"
      --clang "${ForgeContract_CLANG}"
      --wasm "$<TARGET_GENEX_EVAL:hello,$<TARGET_PROPERTY:hello,FORGE_CONTRACT_WASM_FILE>>"
      --abi "$<TARGET_GENEX_EVAL:hello,$<TARGET_PROPERTY:hello,FORGE_CONTRACT_ABI_FILE>>"
      --imports "${ForgeContract_DATA_DIR}/registry/intrinsics.json"
      --manifest "$<TARGET_GENEX_EVAL:hello,$<TARGET_PROPERTY:hello,FORGE_CONTRACT_MANIFEST_FILE>>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/checker/$<CONFIG>"
   DEPENDS hello_artifacts
   VERBATIM
)

add_custom_target(
   forge_contract_sdk_surface ALL
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/check_sdk_surface.py"
      --manifest "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/sdk_surface_manifest.json"
      --registry "${ForgeContract_DATA_DIR}/registry/intrinsics.json"
      --golden "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/spring_intrinsics.txt"
      --data-dir "${ForgeContract_DATA_DIR}"
      --source-root "${CMAKE_CURRENT_SOURCE_DIR}/../../.."
   DEPENDS surface
   VERBATIM
)

add_custom_target(
   forge_contract_reproducibility ALL
   COMMAND
      "${Python3_EXECUTABLE}"
      "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/run_reproducibility.py"
      --cmake "${CMAKE_COMMAND}"
      --package "${ForgeContract_DIR}"
      --source "${CMAKE_CURRENT_SOURCE_DIR}/../tooling/reproducibility"
      --output "${CMAKE_CURRENT_BINARY_DIR}/reproducibility"
   DEPENDS hello
   VERBATIM
)
