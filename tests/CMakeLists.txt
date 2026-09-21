find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(FORGE_VM_WASM_INTERPRET_DONOR_ROOT "" CACHE PATH "Pinned EOS VM checkout used to verify the native interpreter test port")
add_subdirectory(vm_wasm)

set(FORGE_STRUCTURE_CHECK ${CMAKE_CURRENT_SOURCE_DIR}/structure/check_structure.py)
add_custom_target(
   test_forge_structure
   COMMAND ${Python3_EXECUTABLE} ${FORGE_STRUCTURE_CHECK} ${CMAKE_SOURCE_DIR}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
   VERBATIM
)
add_test(
   NAME test_forge_structure
   COMMAND ${Python3_EXECUTABLE} ${FORGE_STRUCTURE_CHECK} ${CMAKE_SOURCE_DIR}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_test(
   NAME test_forge_contract_sdk_cmake_runtime_policy
   COMMAND
      ${CMAKE_COMMAND}
      -DFORGE_CONTRACT_RUNTIME_POLICY=${CMAKE_SOURCE_DIR}/guest/cmake/ContractSDKRuntimeDependencies.cmake
      -P ${CMAKE_SOURCE_DIR}/guest/tests/tooling/check_runtime_bundle_policy.cmake
)
add_test(
   NAME test_forge_contract_sdk_relocation_runtime_policy
   COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/guest/tests/tooling/test_runtime_bundle_policy.py
)
add_test(
   NAME test_forge_contract_sdk_dependencies
   COMMAND
      ${CMAKE_COMMAND}
      -DFORGE_CONTRACT_SDK_CMAKE=${CMAKE_SOURCE_DIR}/guest/CMakeLists.txt
      -P ${CMAKE_SOURCE_DIR}/guest/tests/tooling/check_sdk_dependencies.cmake
)
add_test(
   NAME test_forge_contract_sdk_guest_runtime_archive
   COMMAND
      ${CMAKE_COMMAND}
      -DFORGE_CONTRACT_STAGE_SYSROOT=${CMAKE_SOURCE_DIR}/guest/cmake/stage_sysroot.cmake
      -DFORGE_CONTRACT_SDK_CMAKE=${CMAKE_SOURCE_DIR}/guest/CMakeLists.txt
      -DFORGE_CONTRACT_LIBRARIES_CMAKE=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractLibraries.cmake
      -DFORGE_CONTRACT_BUILD_CMAKE=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractBuild.cmake
      -DFORGE_CONTRACT_FOUNDATION_CMAKE=${CMAKE_SOURCE_DIR}/guest/cmake/foundation-build/CMakeLists.txt
      -DFORGE_CONTRACT_CONFIG_TEMPLATE=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractConfig.cmake.in
      -DFORGE_CONTRACT_GUEST_COMPONENTS=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractGuestComponents.cmake
      -P ${CMAKE_SOURCE_DIR}/guest/tests/tooling/check_guest_runtime_archive.cmake
)
add_test(
   NAME test_forge_contract_sdk_package_prefix
   COMMAND
      ${CMAKE_COMMAND}
      -DFORGE_CONTRACT_CONFIG_TEMPLATE=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractConfig.cmake.in
      -DFORGE_CONTRACT_PATHS_TEMPLATE=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractPaths.cmake.in
      -DFORGE_CONTRACT_TOOLCHAIN_TEMPLATE=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractToolchain.cmake.in
      -DFORGE_CONTRACT_FUNCTIONS=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractFunctions.cmake
      -DFORGE_CONTRACT_LIBRARIES=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractLibraries.cmake
      -DFORGE_CONTRACT_BUILD=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractBuild.cmake
      -DFORGE_CONTRACT_GUEST_COMPONENTS=${CMAKE_SOURCE_DIR}/guest/cmake/ForgeContractGuestComponents.cmake
      -DFORGE_CONTRACT_TEST_ROOT=${CMAKE_BINARY_DIR}/tests/contract-package-prefix
      -P ${CMAKE_SOURCE_DIR}/guest/tests/tooling/check_package_prefix.cmake
)
add_test(
   NAME test_forge_contract_sdk_developer_toolchain
   COMMAND
      ${CMAKE_COMMAND}
      -DFORGE_CONTRACT_TOOLCHAIN_SELECTOR=${CMAKE_SOURCE_DIR}/guest/cmake/select_developer_toolchain.cmake
      -DFORGE_CONTRACT_TEST_ROOT=${CMAKE_BINARY_DIR}/tests/contract-developer-toolchain
      -P ${CMAKE_SOURCE_DIR}/guest/tests/tooling/check_developer_toolchain.cmake
)
get_property(_forge_test_generator_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
set(_forge_vendor_compile_policy_configuration_options)
if(_forge_test_generator_is_multi_config)
   list(APPEND _forge_vendor_compile_policy_configuration_options --multi-config)
endif()

add_test(
   NAME test_forge_vendor_compile_policy
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/vendor_compile_policy/check_compile_policy.py
      --cmake ${CMAKE_COMMAND}
      --source ${CMAKE_SOURCE_DIR}/tests/vendor_compile_policy
      --binary ${CMAKE_BINARY_DIR}/tests/vendor-compile-policy
      --forge-source ${CMAKE_SOURCE_DIR}
      --generator ${CMAKE_GENERATOR}
      ${_forge_vendor_compile_policy_configuration_options}
      --c-compiler ${CMAKE_C_COMPILER}
      --cxx-compiler ${CMAKE_CXX_COMPILER}
)
add_test(
   NAME test_forge_vendor_compile_policy_multi_config
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_SOURCE_DIR}/tests/vendor_compile_policy/check_compile_policy.py
      --cmake ${CMAKE_COMMAND}
      --source ${CMAKE_SOURCE_DIR}/tests/vendor_compile_policy
      --binary ${CMAKE_BINARY_DIR}/tests/vendor-compile-policy-multi-config
      --forge-source ${CMAKE_SOURCE_DIR}
      --generator "Ninja Multi-Config"
      --multi-config
      --c-compiler ${CMAKE_C_COMPILER}
      --cxx-compiler ${CMAKE_CXX_COMPILER}
)

add_executable(
   test_forge
   unit/main.cpp
   core/test_escape_str.cpp
   variant/test_static_variant.cpp
   variant/test_variant.cpp
   variant/test_variant_dynamic_bitset.cpp
   variant/test_variant_estimated_size.cpp
   raw/test_raw.cpp
   json/test_json.cpp
   crypto/test_base32.cpp
   crypto/test_blake2.cpp
   crypto/test_bls_legacy.cpp
   crypto/test_contract_primitives.cpp
   crypto/test_cypher_suites.cpp
   crypto/test_hash_functions.cpp
   crypto/test_secp256k1_recover.cpp
   crypto/test_modular_arithmetic.cpp
   crypto/test_symmetric.cpp
   crypto/test_webauthn.cpp
)
target_sources(
   test_forge
   PRIVATE
      FILE_SET CXX_MODULES
      FILES json/exact_types.cppm
)

target_link_libraries(test_forge forge Boost::included_unit_test_framework)
add_test(NAME test_forge COMMAND test_forge WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_chrono unit/main.cpp chrono/test_chrono.cpp)
target_link_libraries(test_forge_chrono forge_chrono Boost::included_unit_test_framework)
add_test(NAME test_forge_chrono COMMAND test_forge_chrono WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_variant
   unit/main.cpp
   variant/test_static_variant.cpp
   variant/test_variant.cpp
   variant/test_variant_dynamic_bitset.cpp
   variant/test_variant_estimated_size.cpp
)
target_link_libraries(
   test_forge_variant
   forge_codec_base64
   forge_variant
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_variant COMMAND test_forge_variant WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_exceptions
   unit/main.cpp
   exceptions/exception_tests.cpp
)
target_link_libraries(test_forge_exceptions forge Boost::included_unit_test_framework)
add_test(NAME test_forge_exceptions COMMAND test_forge_exceptions WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_log
   unit/main.cpp
   log/log_tests.cpp
)
target_link_libraries(test_forge_log forge_log Boost::included_unit_test_framework)
add_test(NAME test_forge_log COMMAND test_forge_log WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_otlp
   unit/main.cpp
   otlp/otlp_tests.cpp
)
target_link_libraries(test_forge_otlp forge Boost::included_unit_test_framework)
add_executable(forge_otlp_crash_helper otlp/crash_helper.cpp)
target_link_libraries(forge_otlp_crash_helper forge)
add_dependencies(test_forge_otlp forge_otlp_crash_helper)
target_compile_definitions(test_forge_otlp PRIVATE FORGE_OTLP_CRASH_HELPER="$<TARGET_FILE:forge_otlp_crash_helper>")
add_test(NAME test_forge_otlp COMMAND test_forge_otlp WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_codec_json
   unit/main.cpp
   json/test_json.cpp
)
target_sources(
   test_forge_codec_json
   PRIVATE
      FILE_SET CXX_MODULES
      FILES json/exact_types.cppm
)
target_link_libraries(test_forge_codec_json forge Boost::included_unit_test_framework)
add_test(NAME test_forge_codec_json COMMAND test_forge_codec_json WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

foreach(_codec IN ITEMS base64 base58 hex)
   add_executable(test_forge_codec_${_codec} unit/main.cpp codec/${_codec}_tests.cpp)
   target_link_libraries(test_forge_codec_${_codec} forge_codec_${_codec} Boost::included_unit_test_framework)
   add_test(
      NAME test_forge_codec_${_codec}
      COMMAND test_forge_codec_${_codec}
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
   )
endforeach()

add_executable(
   test_forge_raw
   unit/main.cpp
   raw/test_raw.cpp
)
target_link_libraries(
   test_forge_raw
   forge_codec_hex
   forge_crypto_digest
   forge_raw
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_raw COMMAND test_forge_raw WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_db_ids
   unit/main.cpp
   db_ids/db_ids_tests.cpp
)
target_link_libraries(test_forge_db_ids forge_db_ids forge Boost::included_unit_test_framework)
add_test(NAME test_forge_db_ids COMMAND test_forge_db_ids WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_chain_core
   unit/main.cpp
   chain_core/merkle_tests.cpp
)
target_link_libraries(test_forge_chain_core forge_chain_core forge Boost::included_unit_test_framework)
add_test(NAME test_forge_chain_core COMMAND test_forge_chain_core WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_chain_protocol
   unit/main.cpp
   chain_protocol/producer_schedule_tests.cpp
   chain_protocol/protocol_tests.cpp
)
target_link_libraries(
   test_forge_chain_protocol
   forge_chain_protocol
   forge_codec_json
   forge
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_chain_protocol COMMAND test_forge_chain_protocol WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_chain_protocol_query
   unit/main.cpp
   chain_protocol/query_tests.cpp
)
target_link_libraries(
   test_forge_chain_protocol_query
   forge_chain_protocol
   forge_codec_json
   Boost::included_unit_test_framework
)
add_test(
   NAME test_forge_chain_protocol_query
   COMMAND test_forge_chain_protocol_query
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(
   test_forge_chain_transaction
   unit/main.cpp
   chain_transaction/transaction_tests.cpp
)
target_link_libraries(
   test_forge_chain_transaction
   forge_asio
   forge_chain_transaction
   forge_codec_hex
   forge_raw
   Boost::included_unit_test_framework
)
add_test(
   NAME test_forge_chain_transaction
   COMMAND test_forge_chain_transaction
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(
   test_forge_chain_api
   unit/main.cpp
   chain_api/abi_tests.cpp
   chain_api/chain_api_tests.cpp
)
target_link_libraries(
   test_forge_chain_api
   forge_chain_api
   forge_api_http
   forge_codec_xml
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_chain_api COMMAND test_forge_chain_api WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_chain_portable_verified_client
   unit/main.cpp
   chain_api/portable_verified_client_tests.cpp
   chain_api/contract_commitment_compatibility_tests.cpp
)
target_link_libraries(
   test_forge_chain_portable_verified_client
   forge_chain_api
   forge_codec_hex
   forge_db_authenticated
   forge_raw
   Boost::included_unit_test_framework
)
add_test(
   NAME test_forge_chain_portable_verified_client
   COMMAND test_forge_chain_portable_verified_client
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
add_test(
   NAME test_forge_chain_api_spring_manifest
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/chain_api/check_spring_api_manifest.py
      ${CMAKE_SOURCE_DIR}
      --manifest-only
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
set_tests_properties(test_forge_chain_api_spring_manifest PROPERTIES SKIP_RETURN_CODE 125)
add_test(
   NAME test_forge_chain_api_typed_state_surface
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/chain_api/check_typed_state_surface.py
      ${CMAKE_SOURCE_DIR}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_test(
   NAME test_forge_chain_api_portable_verified_client
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/chain_api/check_portable_verified_client.py
      ${CMAKE_SOURCE_DIR}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_test(
   NAME test_forge_chain_api_typed_control_surfaces
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/chain_api/check_typed_control_surfaces.py
      ${CMAKE_SOURCE_DIR}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)

add_executable(
   test_forge_plugins_http_server
   unit/main.cpp
   http_server_auth/bearer_auth_tests.cpp
)
target_link_libraries(
   test_forge_plugins_http_server
   forge_plugins_http_server
   Boost::included_unit_test_framework
)
add_test(
   NAME test_forge_plugins_http_server
   COMMAND test_forge_plugins_http_server
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(
   test_forge_plugins_descriptor_surfaces
   package_plugins_descriptor_surfaces/main.cpp
)
target_link_libraries(
   test_forge_plugins_descriptor_surfaces PRIVATE
      forge_plugins_crypto_secrets
      forge_plugins_chain_signer
      forge_plugins_db_store
      forge_plugins_http_server
      forge_plugins_p2p_node
      forge_plugins_p2p_pubsub
      forge_plugins_p2p_resolver
)
add_test(
   NAME test_forge_plugins_descriptor_surfaces
   COMMAND test_forge_plugins_descriptor_surfaces
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(
   test_forge_chain_quorum
   unit/main.cpp
   chain_quorum/quorum_tests.cpp
)
target_link_libraries(test_forge_chain_quorum forge_chain_quorum forge Boost::included_unit_test_framework)
add_test(NAME test_forge_chain_quorum COMMAND test_forge_chain_quorum WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_chain_fork
   unit/main.cpp
   chain_fork/fork_tests.cpp
)
target_link_libraries(test_forge_chain_fork forge_chain_fork forge Boost::included_unit_test_framework)
add_test(NAME test_forge_chain_fork COMMAND test_forge_chain_fork WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_chain_savanna
   unit/main.cpp
   chain_savanna/savanna_tests.cpp
   chain_savanna/hardening_tests.cpp
)
target_link_libraries(
   test_forge_chain_savanna
   forge_chain_savanna
   forge_raw
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_chain_savanna COMMAND test_forge_chain_savanna WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(FORGE_ENABLE_TOOLING)
   add_executable(
      test_forge_tooling
      unit/main.cpp
      tooling/contract_tests.cpp
   )
   target_link_libraries(
      test_forge_tooling
      forge_tooling_abi
      forge_tooling_attributes
      forge_tooling_validation
      forge_tooling_manifest
      Boost::included_unit_test_framework
   )
   add_test(NAME test_forge_tooling COMMAND test_forge_tooling WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

add_executable(
   test_forge_db_core
   unit/main.cpp
   db_core/db_tests.cpp
)
target_link_libraries(test_forge_db_core forge_db_core forge Boost::included_unit_test_framework)
add_test(NAME test_forge_db_core COMMAND test_forge_db_core WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_compression
   unit/main.cpp
   compression/zlib_tests.cpp
)
target_link_libraries(test_forge_compression forge_compression forge Boost::included_unit_test_framework)
add_test(NAME test_forge_compression COMMAND test_forge_compression WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_db_object
   unit/main.cpp
   db_object/db_object_tests.cpp
)
target_link_libraries(test_forge_db_object forge_db_object forge Boost::included_unit_test_framework)
target_compile_definitions(test_forge_db_object PRIVATE FORGE_HAS_ROCKSDB=$<BOOL:${FORGE_HAS_ROCKSDB}>)
if(FORGE_HAS_ROCKSDB)
   target_link_libraries(test_forge_db_object forge_db_rocksdb)
endif()

if(FORGE_HAS_MDBX)
   add_executable(
      test_forge_db_mdbx
      unit/main.cpp
      db_mdbx/mdbx_crash_tests.cpp
      db_mdbx/mdbx_tests.cpp
      db_mdbx/mdbx_layers_tests.cpp
      auth/auth_mdbx_integration_tests.cpp
   )
   add_executable(
      forge_db_mdbx_crash_helper
      db_mdbx/mdbx_crash_helper.cpp
   )
   target_link_libraries(forge_db_mdbx_crash_helper forge_db_mdbx)
   add_dependencies(test_forge_db_mdbx forge_db_mdbx_crash_helper)
   target_compile_definitions(
      test_forge_db_mdbx PRIVATE
      FORGE_DB_MDBX_CRASH_HELPER="$<TARGET_FILE:forge_db_mdbx_crash_helper>"
   )
   target_link_libraries(
      test_forge_db_mdbx
      forge_db_mdbx
      forge_db_core
      forge_db_object
      forge_db_blob
      forge_db_revision
      forge_auth_pairing
      Boost::included_unit_test_framework
   )
   add_test(NAME test_forge_db_mdbx COMMAND test_forge_db_mdbx WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

   add_executable(
      test_forge_db_authenticated
      unit/main.cpp
      db_authenticated/authenticated_benchmark_options_tests.cpp
      db_authenticated/authenticated_crash_tests.cpp
      db_authenticated/db_authenticated_tests.cpp
      db_authenticated/ranked_range_proof_adversarial_tests.cpp
   )
   add_executable(
      forge_db_authenticated_crash_helper
      db_authenticated/authenticated_crash_helper.cpp
   )
   target_link_libraries(
      forge_db_authenticated_crash_helper
      forge_asio
      forge_db_authenticated
      forge_db_mdbx
      forge_db_object
      forge_db_revision
   )
   add_dependencies(test_forge_db_authenticated forge_db_authenticated_crash_helper)
   target_compile_definitions(
      test_forge_db_authenticated PRIVATE
      FORGE_DB_AUTHENTICATED_CRASH_HELPER="$<TARGET_FILE:forge_db_authenticated_crash_helper>"
   )
   target_link_libraries(
      test_forge_db_authenticated
      forge_asio
      forge_db_authenticated
      forge_db_mdbx
      forge_db_object
      forge_db_revision
      Boost::included_unit_test_framework
   )
   add_test(NAME test_forge_db_authenticated COMMAND test_forge_db_authenticated WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

   add_executable(
      benchmark_forge_db_authenticated
      db_authenticated/authenticated_benchmark.cpp
   )
   target_link_libraries(
      benchmark_forge_db_authenticated
      forge_asio
      forge_db_authenticated
      forge_db_mdbx
   )
   add_test(
      NAME test_forge_db_authenticated_benchmark_cli
      COMMAND
         ${Python3_EXECUTABLE}
         ${CMAKE_CURRENT_SOURCE_DIR}/db_authenticated/check_authenticated_benchmark.py
         $<TARGET_FILE:benchmark_forge_db_authenticated>
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
   )
   set_tests_properties(
      test_forge_db_authenticated_benchmark_cli
      PROPERTIES
         LABELS "db_authenticated;benchmark;smoke"
         TIMEOUT 60
   )

   option(
      FORGE_DB_AUTHENTICATED_ENABLE_1M_PERFORMANCE_TEST
      "Register the provisional authenticated DB 1M-key performance gate"
      OFF
   )
   option(
      FORGE_DB_AUTHENTICATED_ENABLE_10M_PERFORMANCE_TEST
      "Register the provisional authenticated DB 10M-key performance gate"
      OFF
   )
   if(FORGE_DB_AUTHENTICATED_ENABLE_1M_PERFORMANCE_TEST)
      add_test(
         NAME test_forge_db_authenticated_performance_1m
         COMMAND
            $<TARGET_FILE:benchmark_forge_db_authenticated>
            --baseline 1m
            --machine-label ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-$<CONFIG>
      )
      set_tests_properties(
         test_forge_db_authenticated_performance_1m
         PROPERTIES
            LABELS "db_authenticated;performance;provisional"
            RESOURCE_LOCK forge_db_authenticated_performance
            RUN_SERIAL TRUE
            TIMEOUT 5400
      )
   endif()
   if(FORGE_DB_AUTHENTICATED_ENABLE_10M_PERFORMANCE_TEST)
      add_test(
         NAME test_forge_db_authenticated_performance_10m
         COMMAND
            $<TARGET_FILE:benchmark_forge_db_authenticated>
            --baseline 10m
            --machine-label ${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}-$<CONFIG>
      )
      set_tests_properties(
         test_forge_db_authenticated_performance_10m
         PROPERTIES
            LABELS "db_authenticated;performance;provisional"
            RESOURCE_LOCK forge_db_authenticated_performance
            RUN_SERIAL TRUE
            TIMEOUT 43200
      )
   endif()

   option(FORGE_DB_AUTHENTICATED_ENABLE_FUZZ_TESTS "Build authenticated proof libFuzzer target" OFF)
   if(FORGE_DB_AUTHENTICATED_ENABLE_FUZZ_TESTS)
      add_executable(forge_db_authenticated_fuzz db_authenticated/fuzz_driver.cpp)
      target_link_libraries(forge_db_authenticated_fuzz PRIVATE forge_db_authenticated)
      target_compile_options(
         forge_db_authenticated_fuzz
         PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
      )
      target_link_options(forge_db_authenticated_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
   endif()

   if(FORGE_HAS_ROCKSDB)
      add_executable(
         benchmark_forge_db_backends
         db_mdbx/mdbx_rocksdb_benchmark.cpp
      )
      target_link_libraries(
         benchmark_forge_db_backends
         forge_db_mdbx
         forge_db_rocksdb
         forge_db_object
      )
   endif()
endif()

option(FORGE_DB_AUTHENTICATED_ENABLE_GO_VECTOR_TESTS "Run authenticated proof cross-language Go vectors" OFF)
if(FORGE_DB_AUTHENTICATED_ENABLE_GO_VECTOR_TESTS)
   find_program(FORGE_GO_EXECUTABLE NAMES go REQUIRED)
   add_test(
      NAME test_forge_db_authenticated_go_vectors
      COMMAND
         ${CMAKE_COMMAND} -E env GOWORK=off
         ${FORGE_GO_EXECUTABLE} test -mod=readonly ./...
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/db_authenticated/ics23_harness
   )
   set_tests_properties(
      test_forge_db_authenticated_go_vectors
      PROPERTIES LABELS "db_authenticated;cross_language"
   )
endif()

add_test(NAME test_forge_db_object COMMAND test_forge_db_object WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_db_blob
   unit/main.cpp
   db_blob/db_blob_tests.cpp
)
target_link_libraries(test_forge_db_blob forge_db_blob forge_db_object forge_db_core forge Boost::included_unit_test_framework)
add_test(NAME test_forge_db_blob COMMAND test_forge_db_blob WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_db_revision
   unit/main.cpp
   db_revision/db_revision_tests.cpp
)
target_link_libraries(
   test_forge_db_revision
   forge_db_revision
   forge_db_blob
   forge_db_object
   forge_db_core
   forge
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_db_revision COMMAND test_forge_db_revision WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(FORGE_HAS_ROCKSDB)
   add_executable(
      test_forge_db_rocksdb
      unit/main.cpp
      db_rocksdb/rocksdb_tests.cpp
   )
   target_link_libraries(test_forge_db_rocksdb forge_db_rocksdb forge_rocksdb forge_db_core forge Boost::included_unit_test_framework)
   add_test(NAME test_forge_db_rocksdb COMMAND test_forge_db_rocksdb WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
endif()

add_executable(test_forge_codec_base32 unit/main.cpp crypto/test_base32.cpp)
target_link_libraries(test_forge_codec_base32 forge_codec_base32 Boost::included_unit_test_framework)
add_test(NAME test_forge_codec_base32 COMMAND test_forge_codec_base32 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_crypto_core unit/main.cpp crypto/test_core.cpp)
target_link_libraries(test_forge_crypto_core forge_crypto_core Boost::included_unit_test_framework)
add_test(NAME test_forge_crypto_core COMMAND test_forge_crypto_core WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_auth_pairing unit/main.cpp auth/pairing_tests.cpp)
target_link_libraries(test_forge_auth_pairing forge_auth_pairing Boost::included_unit_test_framework)
add_test(NAME test_forge_auth_pairing COMMAND test_forge_auth_pairing WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_auth_session unit/main.cpp auth/session_tests.cpp)
target_link_libraries(test_forge_auth_session forge_auth_session Boost::included_unit_test_framework)
add_test(NAME test_forge_auth_session COMMAND test_forge_auth_session WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_auth_http unit/main.cpp auth/http_auth_tests.cpp)
target_link_libraries(test_forge_auth_http forge_auth_http Boost::included_unit_test_framework)
add_test(NAME test_forge_auth_http COMMAND test_forge_auth_http WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_client_auth unit/main.cpp auth/client_auth_tests.cpp)
target_link_libraries(
   test_forge_client_auth
   forge_auth_oauth2
   forge_auth_workload
   Boost::included_unit_test_framework
)
if(TARGET forge_auth_appauth)
   target_sources(test_forge_client_auth PRIVATE auth/appauth_request_tests.mm)
   set_source_files_properties(
      auth/appauth_request_tests.mm PROPERTIES
      COMPILE_OPTIONS "-fobjc-arc"
   )
   target_link_libraries(test_forge_client_auth forge_auth_appauth AppAuth::AppAuth)
endif()
add_test(NAME test_forge_client_auth COMMAND test_forge_client_auth WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(APPLE)
   add_test(
      NAME test_forge_auth_oidc_agent_rejects_explicit_on
      COMMAND
         ${CMAKE_COMMAND}
         -DFORGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
         -DFORGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/oidc-agent-explicit-on
         -DFORGE_TEST_GENERATOR=${CMAKE_GENERATOR}
         -DFORGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}
         -DFORGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
         -DFORGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
         -P ${CMAKE_CURRENT_SOURCE_DIR}/auth/oidc_agent_platform_config_test.cmake
   )
endif()

add_test(
   NAME test_forge_auth_oidc_agent_rejects_non_linux_unix
   COMMAND
      ${CMAKE_COMMAND}
      -DFORGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      -DFORGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/tests/oidc-agent-freebsd-explicit-on
      -DFORGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DFORGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}
      -DFORGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DFORGE_TEST_SYSTEM_NAME=FreeBSD
      -DFORGE_TEST_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY
      -P ${CMAKE_CURRENT_SOURCE_DIR}/auth/oidc_agent_platform_config_test.cmake
)

if(TARGET forge_auth_oidc_agent)
   add_executable(test_forge_auth_oidc_agent unit/main.cpp auth/oidc_agent_tests.cpp)
   target_link_libraries(
      test_forge_auth_oidc_agent
      forge_auth_oidc_agent
      OIDCAgent::client
      Boost::included_unit_test_framework
   )
   add_test(
      NAME test_forge_auth_oidc_agent
      COMMAND test_forge_auth_oidc_agent
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
   )
endif()

add_executable(test_forge_chain_signer unit/main.cpp chain_signer/signer_tests.cpp)
target_link_libraries(
   test_forge_chain_signer
   forge_plugins_chain_signer
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_chain_signer COMMAND test_forge_chain_signer WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_crypto_signer unit/main.cpp crypto/test_signer.cpp)
target_link_libraries(
   test_forge_crypto_signer
   forge_asio
   forge_crypto_signer
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_crypto_signer COMMAND test_forge_crypto_signer WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_crypto_digest
   unit/main.cpp
   crypto/test_blake2.cpp
   crypto/test_hash_functions.cpp
)
target_link_libraries(test_forge_crypto_digest forge_crypto_digest Boost::included_unit_test_framework)
add_test(NAME test_forge_crypto_digest COMMAND test_forge_crypto_digest WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_crypto_symmetric unit/main.cpp crypto/test_symmetric.cpp)
target_link_libraries(
   test_forge_crypto_symmetric
   forge_crypto_symmetric
   forge_crypto_digest
   forge_raw
   Boost::included_unit_test_framework
)
target_include_directories(test_forge_crypto_symmetric PRIVATE ${CMAKE_SOURCE_DIR}/libraries/crypto/symmetric)
add_test(NAME test_forge_crypto_symmetric COMMAND test_forge_crypto_symmetric WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(
   NAME test_forge_crypto_symmetric_xsalsa20_structure
   COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/crypto/check_xsalsa20_structure.py
      --source ${CMAKE_SOURCE_DIR}
      --archive $<TARGET_FILE:forge_crypto_symmetric>
)

add_executable(test_forge_crypto_asymmetric_values unit/main.cpp crypto/test_asymmetric_values.cpp)
target_link_libraries(
   test_forge_crypto_asymmetric_values
   forge_crypto_asymmetric_values
   forge_raw
   Boost::included_unit_test_framework
)
add_test(
   NAME test_forge_crypto_asymmetric_values
   COMMAND test_forge_crypto_asymmetric_values
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(
   test_forge_crypto_asymmetric
   unit/main.cpp
   crypto/test_asymmetric_serialization.cpp
   crypto/test_cypher_suites.cpp
   crypto/test_secp256k1_recover.cpp
   crypto/test_webauthn.cpp
)
target_link_libraries(
   test_forge_crypto_asymmetric
   forge_codec_json
   forge_crypto_asymmetric
   forge_crypto_symmetric
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_crypto_asymmetric COMMAND test_forge_crypto_asymmetric WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_crypto_keystore unit/main.cpp crypto/test_keystore.cpp)
target_link_libraries(
   test_forge_crypto_keystore
   forge_asio
   forge_crypto_keystore
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_crypto_keystore COMMAND test_forge_crypto_keystore WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_crypto_pki unit/main.cpp crypto/test_pki.cpp)
target_link_libraries(test_forge_crypto_pki forge_crypto_pki Boost::included_unit_test_framework)
add_test(NAME test_forge_crypto_pki COMMAND test_forge_crypto_pki WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_crypto_math unit/main.cpp crypto/test_modular_arithmetic.cpp)
target_link_libraries(test_forge_crypto_math forge_crypto_math Boost::included_unit_test_framework)
add_test(NAME test_forge_crypto_math COMMAND test_forge_crypto_math WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_crypto_bls
   unit/main.cpp
   crypto/test_bls.cpp
   crypto/test_bls_legacy.cpp
   crypto/test_contract_primitives.cpp
)
target_link_libraries(
   test_forge_crypto_bls
   forge_codec_hex
   forge_codec_json
   forge_crypto_bls
   bls12-381
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_crypto_bls COMMAND test_forge_crypto_bls WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(test_forge_crypto_bn256 unit/main.cpp crypto/test_bn256.cpp)
target_link_libraries(
   test_forge_crypto_bn256
   forge_codec_hex
   forge_crypto_bn256
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_crypto_bn256 COMMAND test_forge_crypto_bn256 WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_multiformats
   unit/main.cpp
   multiformats/multiformats_tests.cpp
)
target_link_libraries(test_forge_multiformats forge Boost::included_unit_test_framework)
add_test(NAME test_forge_multiformats COMMAND test_forge_multiformats WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_asio
   unit/main.cpp
   asio/affine_tests.cpp
   asio/compute_tests.cpp
   asio/gate_tests.cpp
   asio/notification_tests.cpp
   asio/task_tests.cpp
)
target_link_libraries(test_forge_asio forge_asio Boost::included_unit_test_framework)
add_test(NAME test_forge_asio COMMAND test_forge_asio WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_cli
   unit/main.cpp
   cli/cli_tests.cpp
)
target_link_libraries(test_forge_cli forge_cli forge_codec_json Boost::included_unit_test_framework)
add_test(NAME test_forge_cli COMMAND test_forge_cli WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
add_test(
   NAME test_forge_cli_boundary
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/cli/check_cli_boundary.py
      ${CMAKE_SOURCE_DIR}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
option(FORGE_CLI_ENABLE_FUZZ_TESTS "Build the Forge CLI argv libFuzzer target" OFF)
if(FORGE_CLI_ENABLE_FUZZ_TESTS)
   if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "FORGE_CLI_ENABLE_FUZZ_TESTS requires Clang libFuzzer")
   endif()
   add_executable(forge_cli_fuzz cli/fuzz_driver.cpp)
   target_link_libraries(forge_cli_fuzz PRIVATE forge_cli)
   target_compile_options(forge_cli_fuzz PRIVATE -fsanitize=fuzzer,address -fno-omit-frame-pointer)
   target_link_options(forge_cli_fuzz PRIVATE -fsanitize=fuzzer,address)
endif()

add_executable(
   test_forge_transport
   unit/main.cpp
   transport/transport_tests.cpp
)
target_link_libraries(test_forge_transport forge Boost::included_unit_test_framework)
add_test(NAME test_forge_transport COMMAND test_forge_transport WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_tcp
   unit/main.cpp
   tcp/tcp_tests.cpp
)
target_link_libraries(test_forge_tcp forge Boost::included_unit_test_framework)
add_test(NAME test_forge_tcp COMMAND test_forge_tcp WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_stcp
   unit/main.cpp
   stcp/stcp_tests.cpp
)
target_link_libraries(test_forge_stcp forge Boost::included_unit_test_framework OpenSSL::SSL)
add_test(NAME test_forge_stcp COMMAND test_forge_stcp WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_tls
   unit/main.cpp
   tls/tls_tests.cpp
   tls/http_server_tests.cpp
)
target_link_libraries(test_forge_tls forge Boost::included_unit_test_framework OpenSSL::Crypto)
add_test(NAME test_forge_tls COMMAND test_forge_tls WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_yamux
   unit/main.cpp
   yamux/yamux_tests.cpp
)
target_link_libraries(test_forge_yamux forge Boost::included_unit_test_framework)
add_test(NAME test_forge_yamux COMMAND test_forge_yamux WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_quic
   unit/main.cpp
   quic/quic_transport_tests.cpp
)
target_link_libraries(test_forge_quic forge Boost::included_unit_test_framework)
add_test(NAME test_forge_quic COMMAND test_forge_quic WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_app
   unit/main.cpp
   app/app_tests.cpp
)
target_link_libraries(test_forge_app forge_app Boost::included_unit_test_framework)
add_test(NAME test_forge_app COMMAND test_forge_app WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_schema
   unit/main.cpp
   schema/schema_tests.cpp
)
target_link_libraries(test_forge_schema forge Boost::included_unit_test_framework)
add_test(NAME test_forge_schema COMMAND test_forge_schema WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_config_core
   unit/main.cpp
   config/config_tests.cpp
)
target_link_libraries(test_forge_config_core forge Boost::included_unit_test_framework)
add_test(NAME test_forge_config_core COMMAND test_forge_config_core WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_codec_yaml
   unit/main.cpp
   yaml/yaml_tests.cpp
)
target_sources(
   test_forge_codec_yaml
   PRIVATE
      FILE_SET CXX_MODULES
      FILES yaml/schema_types.cppm
)
target_link_libraries(test_forge_codec_yaml forge Boost::included_unit_test_framework)
add_test(NAME test_forge_codec_yaml COMMAND test_forge_codec_yaml WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_codec_xml
   unit/main.cpp
   xml/xml_tests.cpp
)
target_link_libraries(test_forge_codec_xml forge_codec_xml Boost::included_unit_test_framework)
add_test(NAME test_forge_codec_xml COMMAND test_forge_codec_xml WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_config_program_options
   unit/main.cpp
   program_options/program_options_tests.cpp
)
target_link_libraries(test_forge_config_program_options forge Boost::included_unit_test_framework)
add_test(NAME test_forge_config_program_options COMMAND test_forge_config_program_options WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_config_env
   unit/main.cpp
   env/env_tests.cpp
)
target_link_libraries(test_forge_config_env forge Boost::included_unit_test_framework)
add_test(NAME test_forge_config_env COMMAND test_forge_config_env WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_api_core
   unit/main.cpp
   api/api_tests.cpp
   api/api_module_smoke_tests.cpp
)
target_link_libraries(test_forge_api_core forge Boost::included_unit_test_framework)
add_test(NAME test_forge_api_core COMMAND test_forge_api_core WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_api_transport
   unit/main.cpp
   api/transport_api_tests.cpp
)
target_link_libraries(test_forge_api_transport forge Boost::included_unit_test_framework)
add_test(NAME test_forge_api_transport COMMAND test_forge_api_transport WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_http_websocket
   unit/main.cpp
   http_websocket/api_live_stream_tests.cpp
   http_websocket/network_tests.cpp
)
target_link_libraries(test_forge_http_websocket forge Boost::included_unit_test_framework)
add_test(NAME test_forge_http_websocket COMMAND test_forge_http_websocket WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_quic_p2p
   unit/main.cpp
   fixtures/local_dns_server.cpp
   fixtures/local_dns_server.hxx
   quic_p2p/libp2p_identity_fixture.cpp
   quic_p2p/dht_record_store_fixture.cpp
   quic_p2p/dht_record_store_tests.cpp
   quic_p2p/dht_profile_tests.cpp
   quic_p2p/dht_provider_registry_tests.cpp
   quic_p2p/dht_query_tests.cpp
   quic_p2p/dht_routing_refresh_tests.cpp
   quic_p2p/dht_time_tests.cpp
   quic_p2p/ipns_tests.cpp
   quic_p2p/quic_tests.cpp
   quic_p2p/connection_gater_tests.cpp
   quic_p2p/resource_manager_tests.cpp
   quic_p2p/p2p_lifecycle_tests.cpp
   quic_p2p/host_event_tests.cpp
   quic_p2p/reachability_state_tests.cpp
   quic_p2p/reachability_manager_tests.cpp
   quic_p2p/node_reachability_tests.cpp
   quic_p2p/node_autonat_service_tests.cpp
   quic_p2p/autonat_exchange_tests.cpp
   quic_p2p/net_p2p_observed_address_tests.cpp
   p2p_reachability_codec_tests.cpp
   quic_p2p/p2p_tests.cpp
   quic_p2p/direct_transport_tests.cpp
   quic_p2p/address_resolution_tests.cpp
   quic_p2p/dialing_tests.cpp
   quic_p2p/dial_scheduler_tests.cpp
   quic_p2p/session_teardown_tests.cpp
   quic_p2p/node_session_tests.cpp
   quic_p2p/node_addressing_tests.cpp
   quic_p2p/node_bootstrap_addressing_tests.cpp
   quic_p2p/topology_tests.cpp
)
target_sources(
   test_forge_quic_p2p
   PRIVATE
      FILE_SET CXX_MODULES
      FILES quic_p2p/dht_record_store_fixture.cppm
)
target_link_libraries(
   test_forge_quic_p2p
   forge_api_p2p
   forge_api_transport
   forge_net_p2p
   Boost::included_unit_test_framework
)
target_compile_definitions(test_forge_quic_p2p PRIVATE FORGE_HAS_ROCKSDB=$<BOOL:${FORGE_HAS_ROCKSDB}>)
add_test(NAME test_forge_quic_p2p COMMAND test_forge_quic_p2p WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(test_forge_quic_p2p PROPERTIES TIMEOUT 300)

add_executable(
   test_forge_net_pnet
   unit/main.cpp
   net_pnet/pnet_tests.cpp
)
target_link_libraries(
   test_forge_net_pnet
   forge_asio
   forge_net_pnet
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_net_pnet COMMAND test_forge_net_pnet WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_net_dns
   unit/main.cpp
   fixtures/local_dns_server.cpp
   fixtures/local_dns_server.hxx
   net_dns/dns_tests.cpp
   net_dns/resolver_socket_registry_tests.cpp
   net_dns/resolver_query_failure_tests.cpp
)
target_link_libraries(
   test_forge_net_dns
   forge_asio
   forge_net_dns
   Boost::included_unit_test_framework
)
target_include_directories(
   test_forge_net_dns
   PRIVATE
      "${PROJECT_SOURCE_DIR}/libraries/net/dns"
      "${PROJECT_SOURCE_DIR}/vendor/c-ares"
      "${PROJECT_SOURCE_DIR}/vendor/c-ares/include"
      "${CMAKE_BINARY_DIR}/vendor/c-ares"
)
add_test(NAME test_forge_net_dns COMMAND test_forge_net_dns WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

set(FORGE_CARES_STRUCTURE_CHECK ${CMAKE_CURRENT_SOURCE_DIR}/net_dns/check_cares_structure.py)
add_custom_target(
   test_forge_net_dns_cares_structure
   COMMAND ${Python3_EXECUTABLE} ${FORGE_CARES_STRUCTURE_CHECK} --source ${CMAKE_SOURCE_DIR}
)
add_test(
   NAME test_forge_net_dns_cares_structure
   COMMAND ${Python3_EXECUTABLE} ${FORGE_CARES_STRUCTURE_CHECK} --source ${CMAKE_SOURCE_DIR}
)

set(FORGE_INTEROP_PROVENANCE ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/provenance.py)
set(FORGE_INTEROP_ARTIFACT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/libp2p_interop)
get_property(_forge_interop_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_forge_interop_multi_config)
   set(FORGE_INTEROP_ARTIFACT_DIRECTORY ${FORGE_INTEROP_ARTIFACT_DIRECTORY}/$<CONFIG>)
   set(FORGE_INTEROP_BUILD_PROFILE "$<CONFIG>")
elseif(CMAKE_BUILD_TYPE)
   set(FORGE_INTEROP_BUILD_PROFILE "${CMAKE_BUILD_TYPE}")
else()
   set(FORGE_INTEROP_BUILD_PROFILE "default")
endif()
set(FORGE_INTEROP_BUILD_INFO_DIRECTORY ${FORGE_INTEROP_ARTIFACT_DIRECTORY})
set(FORGE_INTEROP_BUILD_INFO_HEADER ${FORGE_INTEROP_BUILD_INFO_DIRECTORY}/forge_interop_build_info.hxx)
set(FORGE_INTEROP_BUILD_INFO_STAMP ${FORGE_INTEROP_BUILD_INFO_DIRECTORY}/forge_interop_build_info.json)
add_custom_target(
   forge_interop_fixture_build_info
   COMMAND ${CMAKE_COMMAND} -E make_directory ${FORGE_INTEROP_BUILD_INFO_DIRECTORY}
   COMMAND ${Python3_EXECUTABLE}
      ${FORGE_INTEROP_PROVENANCE}
      emit-build-info
      --forge-root ${CMAKE_SOURCE_DIR}
      --output-header ${FORGE_INTEROP_BUILD_INFO_HEADER}
      --output-stamp ${FORGE_INTEROP_BUILD_INFO_STAMP}
      --compiler-path ${CMAKE_CXX_COMPILER}
      --compiler-id ${CMAKE_CXX_COMPILER_ID}
      --compiler-version ${CMAKE_CXX_COMPILER_VERSION}
      --build-profile ${FORGE_INTEROP_BUILD_PROFILE}
   BYPRODUCTS ${FORGE_INTEROP_BUILD_INFO_HEADER} ${FORGE_INTEROP_BUILD_INFO_STAMP}
   DEPENDS ${FORGE_INTEROP_PROVENANCE}
   VERBATIM
)
add_executable(
   forge_interop_fixture
   libp2p_interop/forge_interop_fixture.cpp
   libp2p_interop/forge_autonat_fixture.cpp
   libp2p_interop/forge_connection_fixture.cpp
)
add_dependencies(forge_interop_fixture forge_interop_fixture_build_info)
target_include_directories(forge_interop_fixture PRIVATE ${FORGE_INTEROP_BUILD_INFO_DIRECTORY})
target_link_libraries(forge_interop_fixture forge_net_p2p forge_codec_hex forge_crypto_digest)

add_test(
   NAME test_forge_libp2p_wire_fixture
   COMMAND forge_interop_fixture --self-test
)
set_tests_properties(test_forge_libp2p_wire_fixture PROPERTIES TIMEOUT 45)

set(_forge_libp2p_interop_enabled 0)
if(FORGE_ENABLE_LIBP2P_INTEROP)
   set(_forge_libp2p_interop_enabled 1)
endif()

add_custom_target(
   test_forge_libp2p_interop
   COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/runner.py
      --enabled ${_forge_libp2p_interop_enabled}
      --forge-fixture $<TARGET_FILE:forge_interop_fixture>
      --source-dir ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
      --build-dir ${FORGE_INTEROP_ARTIFACT_DIRECTORY}
      --forge-root ${CMAKE_SOURCE_DIR}
      --donors-root ${FORGE_LIBP2P_DONORS_ROOT}
      --acceptance-manifest ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_donor_capabilities.json
   DEPENDS forge_interop_fixture
)
add_test(
   NAME test_forge_libp2p_interop
   COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/runner.py
      --enabled ${_forge_libp2p_interop_enabled}
      --forge-fixture $<TARGET_FILE:forge_interop_fixture>
      --source-dir ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
      --build-dir ${FORGE_INTEROP_ARTIFACT_DIRECTORY}
      --forge-root ${CMAKE_SOURCE_DIR}
      --donors-root ${FORGE_LIBP2P_DONORS_ROOT}
      --acceptance-manifest ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_donor_capabilities.json
)
set_tests_properties(test_forge_libp2p_interop PROPERTIES SKIP_REGULAR_EXPRESSION "^SKIP:")

add_test(
   NAME test_forge_libp2p_fixture_provenance
   COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/runner.py
      --enabled ${_forge_libp2p_interop_enabled}
      --provenance-only
      --forge-fixture $<TARGET_FILE:forge_interop_fixture>
      --source-dir ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
      --build-dir ${FORGE_INTEROP_ARTIFACT_DIRECTORY}
      --forge-root ${CMAKE_SOURCE_DIR}
      --donors-root ${FORGE_LIBP2P_DONORS_ROOT}
      --acceptance-manifest ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_donor_capabilities.json
)

add_test(
   NAME test_forge_libp2p_worktree_fingerprint
   COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/test_provenance.py
)

add_test(
   NAME test_forge_libp2p_donor_matrix
   COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/check_donor_matrix.py
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/donor_cases.json
      ${FORGE_LIBP2P_DONORS_ROOT}
)

set(FORGE_P2P_INVENTORY_CHECK ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/check_p2p_feature_inventory.py)
set(FORGE_P2P_STAGE6_ACCEPTANCE_CHECK
   ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/check_stage6_acceptance.py
)
set(FORGE_P2P_STAGE6_PROMOTION_WRAPPER
   ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/promote_stage6_acceptance.py
)
set(
   FORGE_P2P_STAGE6_PROMOTION_DIRECTORY
   "${CMAKE_CURRENT_BINARY_DIR}/libp2p_interop-stage6-promotion"
   CACHE PATH
   "Build directory created by the CMake-owned Stage 6 canonical runner promotion"
)
if(_forge_interop_multi_config)
   set(
      FORGE_P2P_STAGE6_PROMOTION_DIRECTORY
      ${FORGE_P2P_STAGE6_PROMOTION_DIRECTORY}/$<CONFIG>
   )
endif()
add_custom_target(
   test_forge_p2p_inventory
   COMMAND ${Python3_EXECUTABLE}
      ${FORGE_P2P_INVENTORY_CHECK}
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_feature_inventory.json
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_donor_capabilities.json
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/donor_cases.json
      ${FORGE_LIBP2P_DONORS_ROOT}
   COMMAND ${Python3_EXECUTABLE} ${FORGE_P2P_STAGE6_ACCEPTANCE_CHECK} --self-test
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
   VERBATIM
)
add_test(
   NAME test_forge_p2p_inventory
   COMMAND ${Python3_EXECUTABLE}
      ${FORGE_P2P_INVENTORY_CHECK}
      ${CMAKE_SOURCE_DIR}
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_feature_inventory.json
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_donor_capabilities.json
      ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/donor_cases.json
      ${FORGE_LIBP2P_DONORS_ROOT}
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_test(
   NAME test_forge_p2p_stage6_acceptance_checker
   COMMAND ${Python3_EXECUTABLE} ${FORGE_P2P_STAGE6_ACCEPTANCE_CHECK} --self-test
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
)
add_test(
   NAME test_forge_libp2p_dns_fixture
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_dns_fixture
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_dns_fixture PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_process_lifecycle
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_process_lifecycle
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_process_lifecycle PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_autonat_network
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_autonat_network
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_autonat_network PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_autonat_evidence
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_autonat_evidence
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_autonat_evidence PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_autonat_cases
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_autonat_cases
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_autonat_cases PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_autonat_acceptance
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_autonat_acceptance
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_autonat_acceptance PROPERTIES TIMEOUT 120)
add_test(
   NAME test_forge_libp2p_provider_evidence
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_provider_evidence
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_provider_evidence PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_identify_evidence
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_identify_evidence
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_identify_evidence PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_upgrade_evidence
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_upgrade_evidence
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_upgrade_evidence PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_rust_upgrade_evidence
   COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/test_rust_upgrade_evidence.py
)
set_tests_properties(test_forge_libp2p_rust_upgrade_evidence PROPERTIES TIMEOUT 10)
add_test(
   NAME test_forge_libp2p_relay_evidence
   COMMAND ${Python3_EXECUTABLE} -B -m unittest test_relay_evidence
   WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
)
set_tests_properties(test_forge_libp2p_relay_evidence PROPERTIES TIMEOUT 10)

execute_process(
   COMMAND git -C ${CMAKE_SOURCE_DIR} rev-parse HEAD
   RESULT_VARIABLE _forge_p2p_stage6_git_result
   OUTPUT_VARIABLE _forge_p2p_stage6_expected_head
   OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT _forge_p2p_stage6_git_result EQUAL 0)
   message(FATAL_ERROR "Cannot resolve exact git HEAD for Stage 6 acceptance artifact validation")
endif()
add_custom_target(
   test_forge_p2p_stage6_acceptance
   COMMAND ${Python3_EXECUTABLE}
      ${FORGE_P2P_STAGE6_PROMOTION_WRAPPER}
      --runner ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/runner.py
      --forge-fixture $<TARGET_FILE:forge_interop_fixture>
      --source-dir ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
      --build-dir ${FORGE_P2P_STAGE6_PROMOTION_DIRECTORY}
      --forge-root ${CMAKE_SOURCE_DIR}
      --donors-root ${FORGE_LIBP2P_DONORS_ROOT}
      --acceptance-manifest ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_donor_capabilities.json
      --expected-head ${_forge_p2p_stage6_expected_head}
   DEPENDS forge_interop_fixture test_forge_p2p_inventory
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
   VERBATIM
)

add_custom_target(
   test_forge_p2p_autonat_acceptance
   COMMAND ${Python3_EXECUTABLE}
      ${FORGE_P2P_STAGE6_PROMOTION_WRAPPER}
      --suite autonat
      --runner ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/runner.py
      --forge-fixture $<TARGET_FILE:forge_interop_fixture>
      --source-dir ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop
      --build-dir ${FORGE_P2P_STAGE6_PROMOTION_DIRECTORY}/autonat
      --forge-root ${CMAKE_SOURCE_DIR}
      --donors-root ${FORGE_LIBP2P_DONORS_ROOT}
      --acceptance-manifest ${CMAKE_CURRENT_SOURCE_DIR}/libp2p_interop/p2p_donor_capabilities.json
      --expected-head ${_forge_p2p_stage6_expected_head}
   DEPENDS forge_interop_fixture test_forge_p2p_inventory
   WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
   VERBATIM
)

add_executable(
   test_forge_plugins
   unit/main.cpp
   quic_p2p/libp2p_identity_fixture.cpp
   plugins/plugins_tests.cpp
   plugins/resolver_publication_race.cpp
   plugins/log_otlp_tests.cpp
   plugins/details/resolver_publication_race.hxx
)
target_link_libraries(
   test_forge_plugins
   forge_app
   forge_api_http
   forge_api_p2p
   forge_api_transport
   forge_config_env
   forge_config_program_options
   forge_crypto_bls
   forge_plugins_crypto_secrets
   forge_plugins_http_server
   forge_plugins_log_otlp
   forge_plugins_p2p_diagnostics
   forge_plugins_p2p_node
   forge_plugins_p2p_pubsub
   forge_plugins_p2p_resolver
   Boost::included_unit_test_framework
)
add_test(NAME test_forge_plugins COMMAND test_forge_plugins WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_p2p_managed_remote
   unit/main.cpp
   quic_p2p/libp2p_identity_fixture.cpp
   plugins/p2p_managed_remote_tests.cpp
)
target_link_libraries(
   test_forge_p2p_managed_remote
   forge_plugins_p2p_resolver
   forge_plugins_p2p_node
   forge_plugins_crypto_secrets
   forge_asio
   Boost::included_unit_test_framework
)
target_include_directories(test_forge_p2p_managed_remote PRIVATE ${CMAKE_SOURCE_DIR}/plugins/p2p/resolver)
add_test(
   NAME test_forge_p2p_managed_remote
   COMMAND test_forge_p2p_managed_remote
   WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)

add_executable(
   test_forge_plugins_db_store
   unit/main.cpp
   quic_p2p/libp2p_identity_fixture.cpp
   plugins_db_store/db_store_tests.cpp
   plugins_db_store/p2p_peer_state_persistence_tests.cpp
)
target_link_libraries(
   test_forge_plugins_db_store
   forge_plugins_db_store
   forge_plugins_crypto_secrets
   forge_plugins_p2p_node
   Boost::included_unit_test_framework
)
target_include_directories(test_forge_plugins_db_store PRIVATE ${PROJECT_SOURCE_DIR}/plugins/p2p/node)
target_compile_definitions(test_forge_plugins_db_store PRIVATE FORGE_HAS_ROCKSDB=$<BOOL:${FORGE_HAS_ROCKSDB}>)
target_compile_definitions(test_forge_plugins_db_store PRIVATE FORGE_HAS_MDBX=$<BOOL:${FORGE_HAS_MDBX}>)
if(FORGE_HAS_ROCKSDB)
   target_link_libraries(test_forge_plugins_db_store forge_db_rocksdb)
endif()
if(FORGE_HAS_MDBX)
   target_link_libraries(test_forge_plugins_db_store forge_db_mdbx)
endif()
add_test(NAME test_forge_plugins_db_store COMMAND test_forge_plugins_db_store WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

add_executable(
   test_forge_crypto_secrets
   unit/main.cpp
   plugins/crypto/secrets_tests.cpp
)
target_link_libraries(test_forge_crypto_secrets forge Boost::included_unit_test_framework)
target_include_directories(test_forge_crypto_secrets PRIVATE ${PROJECT_SOURCE_DIR}/plugins/crypto/secrets)
add_test(NAME test_forge_crypto_secrets COMMAND test_forge_crypto_secrets WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

if(FORGE_HAS_ROCKSDB)
   add_executable(
      test_forge_rocksdb
      unit/main.cpp
      rocksdb/rocksdb_tests.cpp
   )
   target_link_libraries(test_forge_rocksdb forge_rocksdb forge Boost::included_unit_test_framework)
   add_test(NAME test_forge_rocksdb COMMAND test_forge_rocksdb WORKING_DIRECTORY ${CMAKE_BINARY_DIR})

else()
   message(STATUS "Skipping test_forge_rocksdb: RocksDB is not available")
endif()

if(TARGET forge_tui)
   add_executable(
      test_forge_tui
      unit/main.cpp
      tui/tui_tests.cpp
   )
   target_link_libraries(test_forge_tui forge forge_tui Boost::included_unit_test_framework)
   add_test(NAME test_forge_tui COMMAND test_forge_tui WORKING_DIRECTORY ${CMAKE_BINARY_DIR})
else()
   message(STATUS "Skipping test_forge_tui: forge_tui is not available")
endif()

set(FORGE_PACKAGE_INSTALL_PREFIX "${CMAKE_BINARY_DIR}/forge-package-install")
set(FORGE_PACKAGE_DIR "${FORGE_PACKAGE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/cmake/Forge")
set(FORGE_PACKAGE_CONSUMER_PREFIX_PATH ${CMAKE_PREFIX_PATH})
list(REMOVE_DUPLICATES FORGE_PACKAGE_CONSUMER_PREFIX_PATH)
set(FORGE_PACKAGE_CONSUMER_CACHE "${CMAKE_BINARY_DIR}/tests/ForgePackageConsumerCache.cmake")
string(
   CONCAT FORGE_PACKAGE_CONSUMER_CACHE_CONTENT
   "set(Forge_DIR [==[${FORGE_PACKAGE_DIR}]==] CACHE PATH \"Exact Forge test package\" FORCE)\n"
   "set(CMAKE_PREFIX_PATH [==[${FORGE_PACKAGE_CONSUMER_PREFIX_PATH}]==] CACHE STRING \"Package dependency prefixes\" FORCE)\n"
   "set(glaze_DIR [==[${glaze_DIR}]==] CACHE PATH \"Exact Glaze package\" FORCE)\n"
   "set(FORGE_EXPECTED_PACKAGE_DIR [==[${FORGE_PACKAGE_DIR}]==] CACHE PATH \"Expected Forge test package\" FORCE)\n"
   "set(FORGE_EXPECTED_GLAZE_DIR [==[${glaze_DIR}]==] CACHE PATH \"Expected Glaze package\" FORCE)\n"
)
file(
   GENERATE
   OUTPUT "${FORGE_PACKAGE_CONSUMER_CACHE}"
   CONTENT "${FORGE_PACKAGE_CONSUMER_CACHE_CONTENT}"
)

set(FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_PREFIX_PATH ${CMAKE_PREFIX_PATH})
list(REMOVE_DUPLICATES FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_PREFIX_PATH)
set(
   FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_CACHE
   "${CMAKE_BINARY_DIR}/tests/ForgePackageExplicitGlazeDirCache.cmake"
)
string(
   CONCAT FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_CACHE_CONTENT
   "set(Forge_DIR [==[${FORGE_PACKAGE_DIR}]==] CACHE PATH \"Exact Forge test package\" FORCE)\n"
   "set(CMAKE_PREFIX_PATH [==[${FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_PREFIX_PATH}]==] CACHE STRING \"Shared dependency prefixes\" FORCE)\n"
   "set(glaze_DIR [==[${glaze_DIR}]==] CACHE PATH \"Explicit Glaze package hint\" FORCE)\n"
   "set(FORGE_EXPECTED_PREFIX_PATH [==[${FORGE_PACKAGE_CONSUMER_PREFIX_PATH}]==] CACHE STRING \"Expected shared dependency prefixes\" FORCE)\n"
   "set(FORGE_EXPECTED_PACKAGE_DIR [==[${FORGE_PACKAGE_DIR}]==] CACHE PATH \"Expected Forge test package\" FORCE)\n"
   "set(FORGE_EXPECTED_GLAZE_DIR [==[${glaze_DIR}]==] CACHE PATH \"Expected Glaze package\" FORCE)\n"
)
file(
   GENERATE
   OUTPUT "${FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_CACHE}"
   CONTENT "${FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_CACHE_CONTENT}"
)
set(
   FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS
   -C${FORGE_PACKAGE_CONSUMER_CACHE}
   -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
   -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
   -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
)

if(TARGET forge_auth_oidc_agent)
   add_library(
      forge_test_oidc_agent_package_stub STATIC
      package_auth_oidc_agent/oidc_agent_stub.cpp
   )
   target_link_libraries(forge_test_oidc_agent_package_stub PRIVATE OIDCAgent::client)
   set_target_properties(
      forge_test_oidc_agent_package_stub
      PROPERTIES
         ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/oidc-agent-package-lib"
   )
   get_target_property(
      FORGE_OIDC_AGENT_PACKAGE_INCLUDE_DIRS
      OIDCAgent::client
      INTERFACE_INCLUDE_DIRECTORIES
   )
   list(GET FORGE_OIDC_AGENT_PACKAGE_INCLUDE_DIRS 0 FORGE_OIDC_AGENT_PACKAGE_INCLUDE_DIR)
   set(FORGE_OIDC_AGENT_PACKAGE_CONFIG_DIR "${CMAKE_BINARY_DIR}/tests/oidc-agent-pkgconfig")
   file(MAKE_DIRECTORY "${FORGE_OIDC_AGENT_PACKAGE_CONFIG_DIR}")
   file(
      GENERATE
      OUTPUT "${FORGE_OIDC_AGENT_PACKAGE_CONFIG_DIR}/liboidc-agent.pc"
      CONTENT
         "Name: liboidc-agent\nDescription: Forge package-test oidc-agent ABI\nVersion: 5.3.8\nLibs: $<TARGET_FILE:forge_test_oidc_agent_package_stub>\nCflags: -I${FORGE_OIDC_AGENT_PACKAGE_INCLUDE_DIR}\n"
   )
endif()

add_test(
   NAME test_forge_package_explicit_glaze_dir
   COMMAND
      ${CMAKE_COMMAND}
      --fresh
      -S ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_json_component
      -B ${CMAKE_BINARY_DIR}/forge-package-codec-json-explicit-glaze-dir
      -G ${CMAKE_GENERATOR}
      -C${FORGE_PACKAGE_EXPLICIT_GLAZE_DIR_CACHE}
      -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
      -DCMAKE_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
)
set_tests_properties(
   test_forge_package_explicit_glaze_dir
   PROPERTIES DEPENDS test_forge_package_install
)

add_custom_target(
   test_forge_package_install
   COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${FORGE_PACKAGE_INSTALL_PREFIX} --component dev
   DEPENDS forge
)
add_custom_target(
   test_forge_package_consumer
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_consumer
      ${CMAKE_BINARY_DIR}/forge-package-consumer
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_consumer
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_core_lightweight
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_core_lightweight
      ${CMAKE_BINARY_DIR}/forge-package-core-lightweight
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_core_lightweight
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_asio_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_asio_component
      ${CMAKE_BINARY_DIR}/forge-package-asio-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_asio_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_cli_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_cli_component
      ${CMAKE_BINARY_DIR}/forge-package-cli-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_cli_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_config_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_config_core_component
      ${CMAKE_BINARY_DIR}/forge-package-config-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_config_core_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_config_env_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_config_env_component
      ${CMAKE_BINARY_DIR}/forge-package-config-env-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_config_env_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_config_program_options_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_config_program_options_component
      ${CMAKE_BINARY_DIR}/forge-package-config-program-options-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_config_program_options_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_api_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_core_component
      ${CMAKE_BINARY_DIR}/forge-package-api-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_api_core_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_transport_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_transport_component
      ${CMAKE_BINARY_DIR}/forge-package-net-transport-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_transport_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_tcp_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_tcp_component
      ${CMAKE_BINARY_DIR}/forge-package-net-tcp-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_tcp_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_tls_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_tls_component
      ${CMAKE_BINARY_DIR}/forge-package-net-tls-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_tls_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_stcp_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_stcp_component
      ${CMAKE_BINARY_DIR}/forge-package-net-stcp-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_stcp_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_yamux_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_yamux_component
      ${CMAKE_BINARY_DIR}/forge-package-net-yamux-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_yamux_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_http_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_http_component
      ${CMAKE_BINARY_DIR}/forge-package-net-http-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_http_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_websocket_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_websocket_component
      ${CMAKE_BINARY_DIR}/forge-package-net-websocket-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_websocket_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_quic_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_quic_component
      ${CMAKE_BINARY_DIR}/forge-package-net-quic-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_quic_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_p2p_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_p2p_component
      ${CMAKE_BINARY_DIR}/forge-package-net-p2p-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_p2p_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_pnet_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_pnet_component
      ${CMAKE_BINARY_DIR}/forge-package-net-pnet-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_pnet_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_net_dns_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_dns_component
      ${CMAKE_BINARY_DIR}/forge-package-net-dns-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_dns_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_otlp_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_otlp_component
      ${CMAKE_BINARY_DIR}/forge-package-otlp-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_otlp_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_api_http_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_http_component
      ${CMAKE_BINARY_DIR}/forge-package-api-http-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_http_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_api_stream_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_stream_component
      ${CMAKE_BINARY_DIR}/forge-package-api-stream-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_stream_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_api_quic_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_quic_component
      ${CMAKE_BINARY_DIR}/forge-package-api-quic-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_quic_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_api_websocket_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_websocket_component
      ${CMAKE_BINARY_DIR}/forge-package-api-websocket-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_websocket_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_api_p2p_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_p2p_component
      ${CMAKE_BINARY_DIR}/forge-package-api-p2p-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_p2p_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_db_ids_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_ids_component
      ${CMAKE_BINARY_DIR}/forge-package-db-ids-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_ids_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_base32_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_base32_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-base32-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_codec_base32_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chrono_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chrono_component
      ${CMAKE_BINARY_DIR}/forge-package-chrono-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_chrono_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_core_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_core_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_auth_pairing_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_pairing_component
      ${CMAKE_BINARY_DIR}/forge-package-auth-pairing-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_auth_pairing_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_auth_session_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_session_component
      ${CMAKE_BINARY_DIR}/forge-package-auth-session-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_auth_session_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_auth_http_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_http_component
      ${CMAKE_BINARY_DIR}/forge-package-auth-http-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_auth_http_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_digest_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_digest_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-digest-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command ${CMAKE_CTEST_COMMAND} --output-on-failure
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_symmetric_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_symmetric_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-symmetric-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_sodium=ON
      --test-command forge_package_crypto_symmetric_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_signer_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_signer_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-signer-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_signer_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_keystore_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_keystore_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-keystore-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_keystore_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_asymmetric_values_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_asymmetric_values_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-asymmetric-values-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_crypto_asymmetric_values_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_asymmetric_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_asymmetric_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-asymmetric-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_asymmetric_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_pki_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_pki_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-pki-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_pki_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_math_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_math_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-math-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_math_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_bls_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_bls_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-bls-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_bls_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_bn256_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_bn256_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-bn256-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_crypto_bn256_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_crypto_removed_component
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-crypto-removed-component
      -DFORGE_PACKAGE_TEST_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_removed_component/expect_configure_failure.cmake
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_compression_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_compression_component
      ${CMAKE_BINARY_DIR}/forge-package-compression-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_compression_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_core_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_core_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_protocol_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_protocol_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-protocol-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_protocol_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_transaction_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_transaction_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-transaction-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_transaction_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_api_component
   COMMAND
      ${CMAKE_COMMAND} -E env CMAKE_BUILD_PARALLEL_LEVEL=4
      ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_api_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-api-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_api_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_quorum_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_quorum_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-quorum-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_quorum_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_fork_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_fork_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-fork-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_fork_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_savanna_values_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_savanna_values_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-savanna-values-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_savanna_values_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_chain_savanna_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_savanna_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-savanna-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_savanna_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_vm_wasm_interpret_component
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_INSTALL_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
      -DFORGE_PACKAGE_TEST_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}
      -DFORGE_PACKAGE_TEST_RELOCATED_PREFIX=${CMAKE_BINARY_DIR}/forge-package-vm-wasm-interpret-relocated
      -DFORGE_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_vm_wasm_interpret_component
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-vm-wasm-interpret-component
      -DFORGE_PACKAGE_TEST_SOURCE_ROOT=${PROJECT_SOURCE_DIR}
      -DFORGE_PACKAGE_TEST_BUILD_ROOT=${CMAKE_BINARY_DIR}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DFORGE_PACKAGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DFORGE_PACKAGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_vm_wasm_interpret_component/verify_relocation.cmake
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_db_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_core_component
      ${CMAKE_BINARY_DIR}/forge-package-db-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_core_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_db_authenticated_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_authenticated_component
      ${CMAKE_BINARY_DIR}/forge-package-db-authenticated-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_authenticated_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_db_object_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_object_component
      ${CMAKE_BINARY_DIR}/forge-package-db-object-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_object_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_db_blob_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_blob_component
      ${CMAKE_BINARY_DIR}/forge-package-db-blob-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_blob_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_db_revision_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_revision_component
      ${CMAKE_BINARY_DIR}/forge-package-db-revision-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_revision_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_base64_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_base64_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-base64-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_base64_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_base58_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_base58_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-base58-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_base58_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_hex_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_hex_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-hex-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_hex_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_json_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_json_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-json-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_json_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_yaml_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_yaml_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-yaml-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_yaml_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_xml_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_xml_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-xml-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
      --test-command forge_package_codec_xml_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_codec_xml_add_subdirectory
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_xml_add_subdirectory
      ${CMAKE_BINARY_DIR}/forge-package-codec-xml-add-subdirectory
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DFORGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      --build-target forge_codec_xml_add_subdirectory
      --test-command forge_codec_xml_add_subdirectory
)
add_custom_target(
   test_forge_package_api_transport_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_transport_component
      ${CMAKE_BINARY_DIR}/forge-package-api-transport-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_transport_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_descriptor_surfaces
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_descriptor_surfaces
      ${CMAKE_BINARY_DIR}/forge-package-plugins-descriptor-surfaces
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_descriptor_surfaces
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_p2p_node
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_p2p_node
      ${CMAKE_BINARY_DIR}/forge-package-plugins-p2p-node
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_p2p_node
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_p2p_resolver
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_p2p_resolver
      ${CMAKE_BINARY_DIR}/forge-package-plugins-p2p-resolver
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_p2p_resolver
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_chain_signer
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_chain_signer
      ${CMAKE_BINARY_DIR}/forge-package-plugins-chain-signer
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_chain_signer
   DEPENDS test_forge_package_install
)
if(TARGET forge_auth_appauth)
   add_custom_target(
      test_forge_package_auth_clients
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_clients
         ${CMAKE_BINARY_DIR}/forge-package-auth-clients
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_auth_clients
      DEPENDS test_forge_package_install
   )
endif()
if(TARGET forge_auth_oidc_agent)
   add_custom_target(
      test_forge_package_auth_oidc_agent
      COMMAND
         ${CMAKE_COMMAND} -E env
         "PKG_CONFIG_PATH=${FORGE_OIDC_AGENT_PACKAGE_CONFIG_DIR}"
         ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_oidc_agent
         ${CMAKE_BINARY_DIR}/forge-package-auth-oidc-agent
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_auth_oidc_agent
      DEPENDS test_forge_package_install forge_test_oidc_agent_package_stub
   )
endif()
add_custom_target(
   test_forge_package_plugins_crypto_secrets
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_crypto_secrets
      ${CMAKE_BINARY_DIR}/forge-package-plugins-crypto-secrets
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_crypto_secrets
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_http_server
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_http_server
      ${CMAKE_BINARY_DIR}/forge-package-plugins-http-server
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_http_server
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_log_otlp
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_log_otlp
      ${CMAKE_BINARY_DIR}/forge-package-plugins-log-otlp
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_log_otlp
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_db_store
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_db_store
      ${CMAKE_BINARY_DIR}/forge-package-plugins-db-store
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_db_store
   DEPENDS test_forge_package_install
)
if(FORGE_HAS_MDBX)
   add_custom_target(
      test_forge_package_db_mdbx_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_db_mdbx_component
         ${CMAKE_BINARY_DIR}/forge-package-db-mdbx-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_db_mdbx_component
      DEPENDS test_forge_package_install
   )
   add_custom_target(
      test_forge_package_db_mdbx_add_subdirectory
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_db_mdbx_add_subdirectory
         ${CMAKE_BINARY_DIR}/forge-package-db-mdbx-add-subdirectory
         --build-generator ${CMAKE_GENERATOR}
         --build-options
            ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
            -DFORGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
         --build-target forge_db_mdbx_add_subdirectory
         --test-command forge_db_mdbx_add_subdirectory
   )
else()
   add_custom_target(
      test_forge_package_db_mdbx_component
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_db_mdbx_component: MDBX is not available"
   )
   add_custom_target(
      test_forge_package_db_mdbx_add_subdirectory
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_db_mdbx_add_subdirectory: MDBX is not available"
   )
endif()
if(FORGE_HAS_ROCKSDB)
   add_custom_target(
      test_forge_package_db_rocksdb_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_db_rocksdb_component
         ${CMAKE_BINARY_DIR}/forge-package-db-rocksdb-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_db_rocksdb_component
      DEPENDS test_forge_package_install
   )

   add_custom_target(
      test_forge_package_rocksdb_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_rocksdb_component
         ${CMAKE_BINARY_DIR}/forge-package-rocksdb-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_rocksdb_component
      DEPENDS test_forge_package_install
   )

else()
   add_custom_target(
      test_forge_package_db_rocksdb_component
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_db_rocksdb_component: RocksDB is not available"
   )
   add_custom_target(
      test_forge_package_rocksdb_component
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_rocksdb_component: RocksDB is not available"
   )
endif()
add_custom_target(
   test_forge_package_plugins_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_component
      ${CMAKE_BINARY_DIR}/forge-package-plugins-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_component
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_all_missing_dependency
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_all_missing_dependency
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-all-missing-dependency
      -DFORGE_PACKAGE_TEST_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DFORGE_PACKAGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DFORGE_PACKAGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}
      -DFORGE_PACKAGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
      -DFORGE_PACKAGE_TEST_CLANG_DIR=${Clang_DIR}
      -DFORGE_PACKAGE_TEST_LLVM_DIR=${LLVM_DIR}
      -DFORGE_PACKAGE_TEST_BUILT_WITH_TUI=${FORGE_HAS_TUI}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_all_missing_dependency/expect_configure_failure.cmake
   DEPENDS test_forge_package_install
)
add_custom_target(
   test_forge_package_plugins_crypto_signer_removed
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_crypto_signer_removed
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-plugins-crypto-signer-removed
      -DFORGE_PACKAGE_TEST_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DFORGE_PACKAGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DFORGE_PACKAGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}
      -DFORGE_PACKAGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
      -DFORGE_PACKAGE_TEST_CLANG_DIR=${Clang_DIR}
      -DFORGE_PACKAGE_TEST_LLVM_DIR=${LLVM_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_crypto_signer_removed/expect_configure_failure.cmake
   DEPENDS test_forge_package_install
)

add_test(
   NAME test_forge_package_install
   COMMAND ${CMAKE_COMMAND} --install ${CMAKE_BINARY_DIR} --prefix ${FORGE_PACKAGE_INSTALL_PREFIX} --component dev
)
add_test(
   NAME test_forge_package_consumer
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_consumer
      ${CMAKE_BINARY_DIR}/forge-package-consumer
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_consumer
)
set_tests_properties(test_forge_package_consumer PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_core_lightweight
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_core_lightweight
      ${CMAKE_BINARY_DIR}/forge-package-core-lightweight
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_core_lightweight
)
set_tests_properties(test_forge_package_core_lightweight PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_asio_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_asio_component
      ${CMAKE_BINARY_DIR}/forge-package-asio-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_asio_component
)
set_tests_properties(test_forge_package_asio_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_cli_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_cli_component
      ${CMAKE_BINARY_DIR}/forge-package-cli-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_cli_component
)
set_tests_properties(test_forge_package_cli_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_config_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_config_core_component
      ${CMAKE_BINARY_DIR}/forge-package-config-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_config_core_component
)
set_tests_properties(test_forge_package_config_core_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_config_env_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_config_env_component
      ${CMAKE_BINARY_DIR}/forge-package-config-env-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_config_env_component
)
set_tests_properties(test_forge_package_config_env_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_config_program_options_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_config_program_options_component
      ${CMAKE_BINARY_DIR}/forge-package-config-program-options-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_config_program_options_component
)
set_tests_properties(test_forge_package_config_program_options_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_api_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_core_component
      ${CMAKE_BINARY_DIR}/forge-package-api-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Ngtcp2=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
         -DCMAKE_DISABLE_FIND_PACKAGE_Notcurses=ON
      --test-command forge_package_api_core_component
)
set_tests_properties(test_forge_package_api_core_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_transport_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_transport_component
      ${CMAKE_BINARY_DIR}/forge-package-net-transport-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_transport_component
)
set_tests_properties(test_forge_package_net_transport_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_tcp_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_tcp_component
      ${CMAKE_BINARY_DIR}/forge-package-net-tcp-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_tcp_component
)
set_tests_properties(test_forge_package_net_tcp_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_tls_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_tls_component
      ${CMAKE_BINARY_DIR}/forge-package-net-tls-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_tls_component
)
set_tests_properties(test_forge_package_net_tls_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_stcp_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_stcp_component
      ${CMAKE_BINARY_DIR}/forge-package-net-stcp-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_stcp_component
)
set_tests_properties(test_forge_package_net_stcp_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_yamux_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_yamux_component
      ${CMAKE_BINARY_DIR}/forge-package-net-yamux-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_yamux_component
)
set_tests_properties(test_forge_package_net_yamux_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_http_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_http_component
      ${CMAKE_BINARY_DIR}/forge-package-net-http-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_http_component
)
set_tests_properties(test_forge_package_net_http_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_websocket_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_websocket_component
      ${CMAKE_BINARY_DIR}/forge-package-net-websocket-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_websocket_component
)
set_tests_properties(test_forge_package_net_websocket_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_quic_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_quic_component
      ${CMAKE_BINARY_DIR}/forge-package-net-quic-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_quic_component
)
set_tests_properties(test_forge_package_net_quic_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_p2p_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_p2p_component
      ${CMAKE_BINARY_DIR}/forge-package-net-p2p-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_p2p_component
)
set_tests_properties(test_forge_package_net_p2p_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_pnet_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_pnet_component
      ${CMAKE_BINARY_DIR}/forge-package-net-pnet-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_pnet_component
)
set_tests_properties(test_forge_package_net_pnet_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_net_dns_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_net_dns_component
      ${CMAKE_BINARY_DIR}/forge-package-net-dns-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_net_dns_component
)
set_tests_properties(test_forge_package_net_dns_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_otlp_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_otlp_component
      ${CMAKE_BINARY_DIR}/forge-package-otlp-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_otlp_component
)
set_tests_properties(test_forge_package_otlp_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_api_http_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_http_component
      ${CMAKE_BINARY_DIR}/forge-package-api-http-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_http_component
)
set_tests_properties(test_forge_package_api_http_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_api_stream_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_stream_component
      ${CMAKE_BINARY_DIR}/forge-package-api-stream-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_stream_component
)
set_tests_properties(test_forge_package_api_stream_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_api_quic_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_quic_component
      ${CMAKE_BINARY_DIR}/forge-package-api-quic-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_quic_component
)
set_tests_properties(test_forge_package_api_quic_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_api_websocket_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_websocket_component
      ${CMAKE_BINARY_DIR}/forge-package-api-websocket-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_websocket_component
)
set_tests_properties(test_forge_package_api_websocket_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_api_p2p_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_p2p_component
      ${CMAKE_BINARY_DIR}/forge-package-api-p2p-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_p2p_component
)
set_tests_properties(test_forge_package_api_p2p_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_db_ids_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_ids_component
      ${CMAKE_BINARY_DIR}/forge-package-db-ids-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_ids_component
)
set_tests_properties(test_forge_package_db_ids_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_base32_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_base32_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-base32-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_codec_base32_component
)
set_tests_properties(test_forge_package_codec_base32_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chrono_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chrono_component
      ${CMAKE_BINARY_DIR}/forge-package-chrono-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_chrono_component
)
set_tests_properties(test_forge_package_chrono_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_core_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_core_component
)
set_tests_properties(test_forge_package_crypto_core_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_auth_pairing_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_pairing_component
      ${CMAKE_BINARY_DIR}/forge-package-auth-pairing-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_auth_pairing_component
)
set_tests_properties(test_forge_package_auth_pairing_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_auth_session_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_session_component
      ${CMAKE_BINARY_DIR}/forge-package-auth-session-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_auth_session_component
)
set_tests_properties(test_forge_package_auth_session_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_auth_http_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_http_component
      ${CMAKE_BINARY_DIR}/forge-package-auth-http-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_auth_http_component
)
set_tests_properties(test_forge_package_auth_http_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_digest_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_digest_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-digest-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command ${CMAKE_CTEST_COMMAND} --output-on-failure
)
set_tests_properties(test_forge_package_crypto_digest_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_symmetric_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_symmetric_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-symmetric-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_sodium=ON
      --test-command forge_package_crypto_symmetric_component
)
set_tests_properties(test_forge_package_crypto_symmetric_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_signer_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_signer_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-signer-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_signer_component
)
set_tests_properties(test_forge_package_crypto_signer_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_keystore_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_keystore_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-keystore-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_keystore_component
)
set_tests_properties(test_forge_package_crypto_keystore_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_asymmetric_values_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_asymmetric_values_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-asymmetric-values-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_crypto_asymmetric_values_component
)
set_tests_properties(test_forge_package_crypto_asymmetric_values_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_asymmetric_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_asymmetric_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-asymmetric-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_asymmetric_component
)
set_tests_properties(test_forge_package_crypto_asymmetric_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_pki_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_pki_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-pki-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_pki_component
)
set_tests_properties(test_forge_package_crypto_pki_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_math_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_math_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-math-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_math_component
)
set_tests_properties(test_forge_package_crypto_math_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_bls_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_bls_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-bls-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_crypto_bls_component
)
set_tests_properties(test_forge_package_crypto_bls_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_bn256_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_bn256_component
      ${CMAKE_BINARY_DIR}/forge-package-crypto-bn256-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=ON
      --test-command forge_package_crypto_bn256_component
)
set_tests_properties(test_forge_package_crypto_bn256_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_crypto_removed_component
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-crypto-removed-component
      -DFORGE_PACKAGE_TEST_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_crypto_removed_component/expect_configure_failure.cmake
)
set_tests_properties(test_forge_package_crypto_removed_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_compression_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_compression_component
      ${CMAKE_BINARY_DIR}/forge-package-compression-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_compression_component
)
set_tests_properties(test_forge_package_compression_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_core_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_core_component
)
set_tests_properties(test_forge_package_chain_core_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_protocol_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_protocol_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-protocol-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_protocol_component
)
set_tests_properties(test_forge_package_chain_protocol_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_transaction_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_transaction_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-transaction-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_transaction_component
)
set_tests_properties(test_forge_package_chain_transaction_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_api_component
   COMMAND
      ${CMAKE_COMMAND} -E env CMAKE_BUILD_PARALLEL_LEVEL=4
      ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_api_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-api-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_api_component
)
set_tests_properties(test_forge_package_chain_api_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_quorum_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_quorum_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-quorum-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_quorum_component
)
set_tests_properties(test_forge_package_chain_quorum_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_fork_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_fork_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-fork-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_fork_component
)
set_tests_properties(test_forge_package_chain_fork_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_savanna_values_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_savanna_values_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-savanna-values-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_savanna_values_component
)
set_tests_properties(test_forge_package_chain_savanna_values_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_chain_savanna_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_chain_savanna_component
      ${CMAKE_BINARY_DIR}/forge-package-chain-savanna-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_chain_savanna_component
)
set_tests_properties(test_forge_package_chain_savanna_component PROPERTIES DEPENDS test_forge_package_install)
if(FORGE_ENABLE_TOOLING)
   add_test(
      NAME test_forge_package_tooling_abi_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_tooling_abi_component
         ${CMAKE_BINARY_DIR}/forge-package-tooling-abi-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options
            ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
            -DClang_DIR=${Clang_DIR}
            -DLLVM_DIR=${LLVM_DIR}
         --test-command forge_package_tooling_abi_component
   )
   set_tests_properties(test_forge_package_tooling_abi_component PROPERTIES DEPENDS test_forge_package_install)
   add_test(
      NAME test_forge_package_tooling_attributes_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_tooling_attributes_component
         ${CMAKE_BINARY_DIR}/forge-package-tooling-attributes-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options
            ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
            -DClang_DIR=${Clang_DIR}
            -DLLVM_DIR=${LLVM_DIR}
            -DCMAKE_DISABLE_FIND_PACKAGE_Boost=TRUE
         --test-command forge_package_tooling_attributes_component
   )
   set_tests_properties(test_forge_package_tooling_attributes_component PROPERTIES DEPENDS test_forge_package_install)
   add_test(
      NAME test_forge_package_tooling_validation_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_tooling_validation_component
         ${CMAKE_BINARY_DIR}/forge-package-tooling-validation-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_tooling_validation_component
   )
   set_tests_properties(test_forge_package_tooling_validation_component PROPERTIES DEPENDS test_forge_package_install)
   add_test(
      NAME test_forge_package_tooling_manifest_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_tooling_manifest_component
         ${CMAKE_BINARY_DIR}/forge-package-tooling-manifest-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_tooling_manifest_component
   )
   set_tests_properties(test_forge_package_tooling_manifest_component PROPERTIES DEPENDS test_forge_package_install)
endif()
add_test(
   NAME test_forge_package_vm_wasm_interpret_component
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_INSTALL_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_INSTALL_LIBDIR=${CMAKE_INSTALL_LIBDIR}
      -DFORGE_PACKAGE_TEST_INCLUDEDIR=${CMAKE_INSTALL_INCLUDEDIR}
      -DFORGE_PACKAGE_TEST_RELOCATED_PREFIX=${CMAKE_BINARY_DIR}/forge-package-vm-wasm-interpret-relocated
      -DFORGE_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_vm_wasm_interpret_component
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-vm-wasm-interpret-component
      -DFORGE_PACKAGE_TEST_SOURCE_ROOT=${PROJECT_SOURCE_DIR}
      -DFORGE_PACKAGE_TEST_BUILD_ROOT=${CMAKE_BINARY_DIR}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DFORGE_PACKAGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DFORGE_PACKAGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_vm_wasm_interpret_component/verify_relocation.cmake
)
set_tests_properties(test_forge_package_vm_wasm_interpret_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_db_core_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_core_component
      ${CMAKE_BINARY_DIR}/forge-package-db-core-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_core_component
)
set_tests_properties(test_forge_package_db_core_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_db_authenticated_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_authenticated_component
      ${CMAKE_BINARY_DIR}/forge-package-db-authenticated-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_authenticated_component
)
set_tests_properties(test_forge_package_db_authenticated_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_db_object_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_object_component
      ${CMAKE_BINARY_DIR}/forge-package-db-object-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_object_component
)
set_tests_properties(test_forge_package_db_object_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_db_blob_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_blob_component
      ${CMAKE_BINARY_DIR}/forge-package-db-blob-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_blob_component
)
set_tests_properties(test_forge_package_db_blob_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_db_revision_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_db_revision_component
      ${CMAKE_BINARY_DIR}/forge-package-db-revision-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_db_revision_component
)
set_tests_properties(test_forge_package_db_revision_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_base64_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_base64_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-base64-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_base64_component
)
set_tests_properties(test_forge_package_codec_base64_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_base58_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_base58_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-base58-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_base58_component
)
set_tests_properties(test_forge_package_codec_base58_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_hex_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_hex_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-hex-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_hex_component
)
set_tests_properties(test_forge_package_codec_hex_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_json_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_json_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-json-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_json_component
)
set_tests_properties(test_forge_package_codec_json_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_yaml_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_yaml_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-yaml-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_codec_yaml_component
)
set_tests_properties(test_forge_package_codec_yaml_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_xml_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_xml_component
      ${CMAKE_BINARY_DIR}/forge-package-codec-xml-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DCMAKE_DISABLE_FIND_PACKAGE_glaze=ON
      --test-command forge_package_codec_xml_component
)
set_tests_properties(test_forge_package_codec_xml_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_codec_xml_add_subdirectory
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_codec_xml_add_subdirectory
      ${CMAKE_BINARY_DIR}/forge-package-codec-xml-add-subdirectory
      --build-generator ${CMAKE_GENERATOR}
      --build-options
         ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         -DFORGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
      --build-target forge_codec_xml_add_subdirectory
      --test-command forge_codec_xml_add_subdirectory
)
add_test(
   NAME test_forge_package_api_transport_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_api_transport_component
      ${CMAKE_BINARY_DIR}/forge-package-api-transport-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_api_transport_component
)
set_tests_properties(test_forge_package_api_transport_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_descriptor_surfaces
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_descriptor_surfaces
      ${CMAKE_BINARY_DIR}/forge-package-plugins-descriptor-surfaces
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_descriptor_surfaces
)
set_tests_properties(test_forge_package_plugins_descriptor_surfaces PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_p2p_node
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_p2p_node
      ${CMAKE_BINARY_DIR}/forge-package-plugins-p2p-node
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_p2p_node
)
set_tests_properties(test_forge_package_plugins_p2p_node PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_p2p_resolver
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_p2p_resolver
      ${CMAKE_BINARY_DIR}/forge-package-plugins-p2p-resolver
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_p2p_resolver
)
set_tests_properties(test_forge_package_plugins_p2p_resolver PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_chain_signer
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_chain_signer
      ${CMAKE_BINARY_DIR}/forge-package-plugins-chain-signer
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_chain_signer
)
set_tests_properties(test_forge_package_plugins_chain_signer PROPERTIES DEPENDS test_forge_package_install)
if(TARGET forge_auth_appauth)
   add_test(
      NAME test_forge_package_auth_clients
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_clients
         ${CMAKE_BINARY_DIR}/forge-package-auth-clients
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_auth_clients
   )
   set_tests_properties(test_forge_package_auth_clients PROPERTIES DEPENDS test_forge_package_install)
endif()
if(TARGET forge_auth_oidc_agent)
   add_test(
      NAME test_forge_package_auth_oidc_agent
      COMMAND
         ${CMAKE_COMMAND} -E env
         "PKG_CONFIG_PATH=${FORGE_OIDC_AGENT_PACKAGE_CONFIG_DIR}"
         ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_auth_oidc_agent
         ${CMAKE_BINARY_DIR}/forge-package-auth-oidc-agent
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_auth_oidc_agent
   )
   set_tests_properties(test_forge_package_auth_oidc_agent PROPERTIES DEPENDS test_forge_package_install)
endif()
add_test(
   NAME test_forge_package_plugins_crypto_secrets
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_crypto_secrets
      ${CMAKE_BINARY_DIR}/forge-package-plugins-crypto-secrets
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_crypto_secrets
)
set_tests_properties(test_forge_package_plugins_crypto_secrets PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_http_server
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_http_server
      ${CMAKE_BINARY_DIR}/forge-package-plugins-http-server
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_http_server
)
set_tests_properties(test_forge_package_plugins_http_server PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_log_otlp
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_log_otlp
      ${CMAKE_BINARY_DIR}/forge-package-plugins-log-otlp
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_log_otlp
)
set_tests_properties(test_forge_package_plugins_log_otlp PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_db_store
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_db_store
      ${CMAKE_BINARY_DIR}/forge-package-plugins-db-store
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_db_store
)
set_tests_properties(test_forge_package_plugins_db_store PROPERTIES DEPENDS test_forge_package_install)
if(FORGE_HAS_MDBX)
   add_test(
      NAME test_forge_package_db_mdbx_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_db_mdbx_component
         ${CMAKE_BINARY_DIR}/forge-package-db-mdbx-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_db_mdbx_component
   )
   set_tests_properties(
      test_forge_package_db_mdbx_component
      PROPERTIES DEPENDS test_forge_package_install
   )
   add_test(
      NAME test_forge_package_db_mdbx_add_subdirectory
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_db_mdbx_add_subdirectory
         ${CMAKE_BINARY_DIR}/forge-package-db-mdbx-add-subdirectory
         --build-generator ${CMAKE_GENERATOR}
         --build-options
            ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
            -DFORGE_SOURCE_DIR=${CMAKE_SOURCE_DIR}
         --build-target forge_db_mdbx_add_subdirectory
         --test-command forge_db_mdbx_add_subdirectory
   )
else()
   add_test(
      NAME test_forge_package_db_mdbx_component
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_db_mdbx_component: MDBX is not available"
   )
   add_test(
      NAME test_forge_package_db_mdbx_add_subdirectory
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_db_mdbx_add_subdirectory: MDBX is not available"
   )
endif()
if(FORGE_HAS_ROCKSDB)
   add_test(
      NAME test_forge_package_db_rocksdb_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_db_rocksdb_component
         ${CMAKE_BINARY_DIR}/forge-package-db-rocksdb-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_db_rocksdb_component
   )
   set_tests_properties(test_forge_package_db_rocksdb_component PROPERTIES DEPENDS test_forge_package_install)

   add_test(
      NAME test_forge_package_rocksdb_component
      COMMAND ${CMAKE_CTEST_COMMAND}
         --build-and-test
         ${CMAKE_CURRENT_SOURCE_DIR}/package_rocksdb_component
         ${CMAKE_BINARY_DIR}/forge-package-rocksdb-component
         --build-generator ${CMAKE_GENERATOR}
         --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
         --test-command forge_package_rocksdb_component
   )
   set_tests_properties(test_forge_package_rocksdb_component PROPERTIES DEPENDS test_forge_package_install)

else()
   add_test(
      NAME test_forge_package_db_rocksdb_component
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_db_rocksdb_component: RocksDB is not available"
   )
   add_test(
      NAME test_forge_package_rocksdb_component
      COMMAND ${CMAKE_COMMAND} -E echo "Skipping test_forge_package_rocksdb_component: RocksDB is not available"
   )
endif()
add_test(
   NAME test_forge_package_plugins_component
   COMMAND ${CMAKE_CTEST_COMMAND}
      --build-and-test
      ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_component
      ${CMAKE_BINARY_DIR}/forge-package-plugins-component
      --build-generator ${CMAKE_GENERATOR}
      --build-options ${FORGE_PACKAGE_CONSUMER_BUILD_OPTIONS}
      --test-command forge_package_plugins_component
)
set_tests_properties(test_forge_package_plugins_component PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_all_missing_dependency
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_all_missing_dependency
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-all-missing-dependency
      -DFORGE_PACKAGE_TEST_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DFORGE_PACKAGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DFORGE_PACKAGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}
      -DFORGE_PACKAGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
      -DFORGE_PACKAGE_TEST_CLANG_DIR=${Clang_DIR}
      -DFORGE_PACKAGE_TEST_LLVM_DIR=${LLVM_DIR}
      -DFORGE_PACKAGE_TEST_BUILT_WITH_TUI=${FORGE_HAS_TUI}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_all_missing_dependency/expect_configure_failure.cmake
)
set_tests_properties(test_forge_package_all_missing_dependency PROPERTIES DEPENDS test_forge_package_install)
add_test(
   NAME test_forge_package_plugins_crypto_signer_removed
   COMMAND ${CMAKE_COMMAND}
      -DFORGE_PACKAGE_TEST_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_crypto_signer_removed
      -DFORGE_PACKAGE_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}/forge-package-plugins-crypto-signer-removed
      -DFORGE_PACKAGE_TEST_PREFIX=${FORGE_PACKAGE_INSTALL_PREFIX}
      -DFORGE_PACKAGE_TEST_GENERATOR=${CMAKE_GENERATOR}
      -DFORGE_PACKAGE_TEST_CXX_COMPILER=${CMAKE_CXX_COMPILER}
      -DFORGE_PACKAGE_TEST_C_COMPILER=${CMAKE_C_COMPILER}
      -DFORGE_PACKAGE_TEST_OSX_SYSROOT=${CMAKE_OSX_SYSROOT}
      -DFORGE_PACKAGE_TEST_CLANG_DIR=${Clang_DIR}
      -DFORGE_PACKAGE_TEST_LLVM_DIR=${LLVM_DIR}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/package_plugins_crypto_signer_removed/expect_configure_failure.cmake
)
set_tests_properties(test_forge_package_plugins_crypto_signer_removed PROPERTIES DEPENDS test_forge_package_install)
