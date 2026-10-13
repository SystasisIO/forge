cmake_minimum_required(VERSION 3.31)

project(ForgeGuestFoundation LANGUAGES C CXX)

include(GNUInstallDirs)

foreach(_required FORGE_SOURCE_DIR FORGE_GUEST_INCLUDE_DIR FORGE_GUEST_LIBC_SOURCE_DIR FORGE_GUEST_BOOST_INCLUDE_DIR)
   if(NOT DEFINED ${_required} OR NOT IS_DIRECTORY "${${_required}}")
      message(FATAL_ERROR "${_required} must name an existing directory")
   endif()
endforeach()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES ON)
set(
   CMAKE_CXX_STANDARD_INCLUDE_DIRECTORIES
   "${CMAKE_SYSROOT}/include/c++/v1"
)

set(_forge_guest_compile_options
    -fno-exceptions
    -fno-rtti
    -fno-threadsafe-statics
    -ffreestanding
    -fvisibility=hidden
    -fno-ident
    -mcpu=mvp)

function(forge_guest_configure_target target)
   target_include_directories(
      ${target}
      PRIVATE
         "${FORGE_SOURCE_DIR}/libraries/exceptions/include"
         "${FORGE_GUEST_INCLUDE_DIR}"
         "${FORGE_GUEST_BOOST_INCLUDE_DIR}"
   )
   target_compile_definitions(${target} PRIVATE FORGE_CONTRACT_GUEST=1)
   target_compile_options(${target} PRIVATE ${_forge_guest_compile_options})
endfunction()

function(forge_guest_add_implementation_archive name)
   cmake_parse_arguments(ARG "" "OUTPUT_NAME" "MODULE_DIRS;MODULES;SOURCES;DEPENDENCIES" ${ARGN})

   add_library(${name}_modules OBJECT)
   target_sources(
      ${name}_modules
      PUBLIC
         FILE_SET CXX_MODULES
         BASE_DIRS ${ARG_MODULE_DIRS}
         FILES ${ARG_MODULES}
   )
   forge_guest_configure_target(${name}_modules)
   if(ARG_DEPENDENCIES)
      target_link_libraries(${name}_modules PRIVATE ${ARG_DEPENDENCIES})
   endif()

   add_library(${name}_implementation OBJECT ${ARG_SOURCES})
   forge_guest_configure_target(${name}_implementation)
   target_link_libraries(${name}_implementation PRIVATE ${name}_modules ${ARG_DEPENDENCIES})

   add_library(${name} STATIC $<TARGET_OBJECTS:${name}_implementation>)
   if(ARG_OUTPUT_NAME)
      set_target_properties(${name} PROPERTIES OUTPUT_NAME "${ARG_OUTPUT_NAME}")
   endif()
   install(TARGETS ${name} ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
endfunction()

set(_raw "${FORGE_SOURCE_DIR}/libraries/raw")
forge_guest_add_implementation_archive(
   forge_guest_raw
   MODULE_DIRS "${_raw}/include"
   MODULES
      "${_raw}/include/forge/raw/stream.cppm"
      "${_raw}/include/forge/raw/varint_value.cppm"
      "${_raw}/include/forge/raw/codec.cppm"
   SOURCES
      "${_raw}/stream.cpp"
      "${_raw}/codec.cpp"
)

set(_db_ids "${FORGE_SOURCE_DIR}/libraries/db/ids")
add_library(forge_guest_db_ids_modules OBJECT)
target_sources(
   forge_guest_db_ids_modules
   PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${_db_ids}/include"
      FILES "${_db_ids}/include/forge/db/ids/typed_id.cppm"
)
forge_guest_configure_target(forge_guest_db_ids_modules)
target_link_libraries(forge_guest_db_ids_modules PRIVATE forge_guest_raw_modules)

foreach(_codec IN ITEMS base64 base58 hex)
   set(_codec_source "${FORGE_SOURCE_DIR}/libraries/codec/${_codec}")
   forge_guest_add_implementation_archive(
      forge_guest_codec_${_codec}
      MODULE_DIRS "${_codec_source}/include"
      MODULES
         "${_codec_source}/include/forge/codec/${_codec}/exceptions.cppm"
         "${_codec_source}/include/forge/codec/${_codec}/${_codec}.cppm"
      SOURCES "${_codec_source}/${_codec}.cpp"
   )
endforeach()

set(_crypto_digest "${FORGE_SOURCE_DIR}/libraries/crypto/digest/include")
set(_crypto_asymmetric "${FORGE_SOURCE_DIR}/libraries/crypto/asymmetric/include")
set(_crypto_bls "${FORGE_SOURCE_DIR}/libraries/crypto/bls/include")
add_library(forge_guest_crypto_modules OBJECT)
target_sources(
   forge_guest_crypto_modules
   PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${_crypto_digest}" "${_crypto_asymmetric}" "${_crypto_bls}"
      FILES
         "${_crypto_digest}/forge/crypto/digest/sha256_value.cppm"
         "${_crypto_digest}/forge/crypto/digest/sha256.cppm"
         "${_crypto_digest}/forge/crypto/digest/sha512_value.cppm"
         "${_crypto_digest}/forge/crypto/digest/sha512.cppm"
         "${_crypto_digest}/forge/crypto/digest/ripemd160_value.cppm"
         "${_crypto_digest}/forge/crypto/digest/ripemd160.cppm"
         "${_crypto_asymmetric}/forge/crypto/asymmetric/values.cppm"
         "${_crypto_asymmetric}/forge/crypto/asymmetric/asymmetric.cppm"
         "${_crypto_bls}/forge/crypto/bls/bls_values.cppm"
)
forge_guest_configure_target(forge_guest_crypto_modules)
target_link_libraries(forge_guest_crypto_modules PRIVATE forge_guest_raw_modules)

set(_savanna_values "${FORGE_SOURCE_DIR}/libraries/chain/savanna/values")
add_library(forge_guest_chain_savanna_values_modules OBJECT)
target_sources(
   forge_guest_chain_savanna_values_modules
   PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${_savanna_values}/include"
      FILES "${_savanna_values}/include/forge/chain/savanna/values.cppm"
)
forge_guest_configure_target(forge_guest_chain_savanna_values_modules)
target_link_libraries(
   forge_guest_chain_savanna_values_modules PRIVATE
      forge_guest_raw_modules
      forge_guest_crypto_modules
)

set(_protocol "${FORGE_SOURCE_DIR}/libraries/chain/protocol")
forge_guest_add_implementation_archive(
   forge_guest_chain_protocol
   MODULE_DIRS "${_protocol}/include"
   MODULES
      "${_protocol}/include/forge/chain/protocol/values.cppm"
      "${_protocol}/include/forge/chain/protocol/time.cppm"
      "${_protocol}/include/forge/chain/protocol/types_value.cppm"
      "${_protocol}/include/forge/chain/protocol/types.cppm"
      "${_protocol}/include/forge/chain/protocol/fixed_key_value.cppm"
      "${_protocol}/include/forge/chain/protocol/fixed_key.cppm"
      "${_protocol}/include/forge/chain/protocol/action_value.cppm"
      "${_protocol}/include/forge/chain/protocol/action.cppm"
      "${_protocol}/include/forge/chain/protocol/transaction_value.cppm"
      "${_protocol}/include/forge/chain/protocol/transaction.cppm"
      "${_protocol}/include/forge/chain/protocol/authority_value.cppm"
      "${_protocol}/include/forge/chain/protocol/authority.cppm"
      "${_protocol}/include/forge/chain/protocol/producer_schedule_value.cppm"
      "${_protocol}/include/forge/chain/protocol/producer_schedule.cppm"
      "${_protocol}/include/forge/chain/protocol/producer_authority_value.cppm"
      "${_protocol}/include/forge/chain/protocol/producer_authority.cppm"
      "${_protocol}/include/forge/chain/protocol/system_value.cppm"
      "${_protocol}/include/forge/chain/protocol/system.cppm"
      "${_protocol}/include/forge/chain/protocol/code_hash_result.cppm"
      "${_protocol}/include/forge/chain/protocol/blockchain_parameters.cppm"
      "${_protocol}/include/forge/chain/protocol/chain_config.cppm"
      "${_protocol}/include/forge/chain/protocol/wasm_parameters.cppm"
      "${_protocol}/include/forge/chain/protocol/ratio.cppm"
      "${_protocol}/include/forge/chain/protocol/elastic_limit_parameters.cppm"
      "${_protocol}/include/forge/chain/protocol/usage_accumulator.cppm"
      "${_protocol}/include/forge/chain/protocol/activated_protocol_feature.cppm"
      "${_protocol}/include/forge/chain/protocol/float64_value.cppm"
      "${_protocol}/include/forge/chain/protocol/float64.cppm"
      "${_protocol}/include/forge/chain/protocol/float128_value.cppm"
      "${_protocol}/include/forge/chain/protocol/float128.cppm"
      "${_protocol}/include/forge/chain/protocol/native_ids.cppm"
      "${_protocol}/include/forge/chain/protocol/entity_selector.cppm"
      "${_protocol}/include/forge/chain/protocol/code.cppm"
      "${_protocol}/include/forge/chain/protocol/table.cppm"
      "${_protocol}/include/forge/chain/protocol/currency_stats.cppm"
      "${_protocol}/include/forge/chain/protocol/kv_parameters.cppm"
      "${_protocol}/include/forge/chain/protocol/finalizer_authority.cppm"
      "${_protocol}/include/forge/chain/protocol/finalizer_policy_value.cppm"
      "${_protocol}/include/forge/chain/protocol/finalizer_policy.cppm"
      "${_protocol}/include/forge/chain/protocol/hash_id.cppm"
      "${_protocol}/include/forge/chain/protocol/call_access_mode.cppm"
      "${_protocol}/include/forge/chain/protocol/call_data_header.cppm"
   SOURCES
      "${_protocol}/values.cpp"
      "${_protocol}/time.cpp"
   DEPENDENCIES
      forge_guest_raw_modules
      forge_guest_db_ids_modules
      forge_guest_crypto_modules
      forge_guest_chain_savanna_values_modules
)

set(_contract "${FORGE_SOURCE_DIR}/guest/libraries/contract")
forge_guest_add_implementation_archive(
   forge_guest_contract
   MODULE_DIRS "${_contract}/include"
   MODULES
      "${_contract}/include/forge/contract/intrinsics.cppm"
      "${_contract}/include/forge/contract/contract.cppm"
      "${_contract}/include/forge/contract/datastream.cppm"
      "${_contract}/include/forge/contract/varint.cppm"
      "${_contract}/include/forge/contract/fixed_bytes.cppm"
      "${_contract}/include/forge/contract/binary_extension.cppm"
      "${_contract}/include/forge/contract/ignore.cppm"
      "${_contract}/include/forge/contract/hash_id.cppm"
      "${_contract}/include/forge/contract/action.cppm"
      "${_contract}/include/forge/contract/transaction.cppm"
      "${_contract}/include/forge/contract/system.cppm"
      "${_contract}/include/forge/contract/deferred_transaction.cppm"
      "${_contract}/include/forge/contract/authorization.cppm"
      "${_contract}/include/forge/contract/bitset.cppm"
      "${_contract}/include/forge/contract/call.cppm"
      "${_contract}/include/forge/contract/crypto.cppm"
      "${_contract}/include/forge/contract/crypto_bls_ext.cppm"
      "${_contract}/include/forge/contract/crypto_ext.cppm"
      "${_contract}/include/forge/contract/instant_finality.cppm"
      "${_contract}/include/forge/contract/key.cppm"
      "${_contract}/include/forge/contract/powers.cppm"
      "${_contract}/include/forge/contract/print.cppm"
      "${_contract}/include/forge/contract/privileged.cppm"
      "${_contract}/include/forge/contract/producer_schedule.cppm"
      "${_contract}/include/forge/contract/rope.cppm"
      "${_contract}/include/forge/contract/string.cppm"
      "${_contract}/include/forge/contract/dispatcher.cppm"
      "${_contract}/include/forge/contract/multi_index.cppm"
      "${_contract}/include/forge/contract/singleton.cppm"
      "${_contract}/include/forge/contract/compatibility_name.cppm"
      "${_contract}/include/forge/contract/compatibility_asset.cppm"
   SOURCES
      "${_contract}/action.cpp"
      "${_contract}/authorization.cpp"
      "${_contract}/bitset.cpp"
      "${_contract}/call.cpp"
      "${_contract}/compatibility_asset.cpp"
      "${_contract}/crypto.cpp"
      "${_contract}/crypto_bls_ext.cpp"
      "${_contract}/crypto_ext.cpp"
      "${_contract}/deferred_transaction.cpp"
      "${_contract}/dispatcher.cpp"
      "${_contract}/instant_finality.cpp"
      "${_contract}/intrinsics.cpp"
      "${_contract}/multi_index.cpp"
      "${_contract}/print.cpp"
      "${_contract}/privileged.cpp"
      "${_contract}/producer_schedule.cpp"
      "${_contract}/rope.cpp"
      "${_contract}/system.cpp"
      "${_contract}/transaction.cpp"
   DEPENDENCIES
      forge_guest_raw_modules
      forge_guest_codec_base64_modules
      forge_guest_codec_base58_modules
      forge_guest_codec_hex_modules
      forge_guest_crypto_modules
      forge_guest_chain_savanna_values_modules
      forge_guest_chain_protocol_modules
)

set(_runtime "${FORGE_SOURCE_DIR}/guest/libraries/runtime")
# Use the same canonical runtime declaration as the standalone library build.
# It owns forge_guest_runtime and forge_guest_math, including pinned libc inputs.
add_subdirectory("${_runtime}" runtime)
forge_guest_configure_target(forge_guest_runtime)
