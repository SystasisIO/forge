add_library(
   forge_tooling_testing STATIC
   compiler_builtins.cpp
   host.cpp
   host_impl.cpp
   memory_driver.cpp
   softfloat.cpp
)

get_filename_component(
   _forge_tooling_testing_source_root
   "${CMAKE_CURRENT_LIST_DIR}/../../.."
   ABSOLUTE
)
set(_forge_tooling_testing_generated_include "${CMAKE_CURRENT_BINARY_DIR}/generated/include")
configure_file(
   "${_forge_tooling_testing_source_root}/guest/cmake/types.h.in"
   "${_forge_tooling_testing_generated_include}/forge/contract/types.h"
   COPYONLY
)

target_sources(
   forge_tooling_testing
   PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
         include/forge/tooling/testing/exceptions.cppm
         include/forge/tooling/testing/schema.cppm
         include/forge/tooling/testing/state.cppm
         include/forge/tooling/testing/host.cppm
)

target_include_directories(
   forge_tooling_testing
   PRIVATE
      "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
      "$<BUILD_INTERFACE:${_forge_tooling_testing_source_root}/guest/libraries/contract/include>"
      "$<BUILD_INTERFACE:${_forge_tooling_testing_generated_include}>"
)

target_link_libraries(
   forge_tooling_testing
   PUBLIC
      forge_chain_protocol
      forge_db_object
      forge_exceptions
      forge_crypto_digest
      Boost::headers
   PRIVATE
      forge_asio
      forge_codec_hex
      forge_crypto_asymmetric
      forge_crypto_bls
      forge_crypto_bn256
      forge_crypto_math
      forge_db_core
      forge_db_ids
      forge_raw
      forge_vm_wasm_interpret
      softfloat
)

target_compile_features(forge_tooling_testing PUBLIC cxx_std_23)

if(TARGET forge_expression)
   target_sources(forge_tooling_testing PRIVATE conversion.cpp)
   target_sources(forge_tooling_testing PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES include/forge/tooling/testing/conversion.cppm
   )
   target_compile_definitions(forge_tooling_testing PUBLIC FORGE_TOOLING_TESTING_EXPRESSION=1)
   target_link_libraries(forge_tooling_testing PUBLIC forge_expression forge_core)
endif()

if(BUILD_TESTING OR FORGE_CONTRACT_SDK_HOST_PROFILE)
   if(TARGET forge_expression)
      add_executable(forge_tooling_testing_expression_tests
         "${_forge_tooling_testing_source_root}/tests/tooling_testing/expression_tests.cpp")
      target_link_libraries(forge_tooling_testing_expression_tests PRIVATE
         forge_tooling_testing forge_vm_wasm_interpret forge_vm_wasm_execution forge_expression Boost::headers)
      target_compile_definitions(forge_tooling_testing_expression_tests PRIVATE
         "FORGE_EXPRESSION_WORKER=\"$<TARGET_FILE:forge_expression_worker>\"")
      add_dependencies(forge_tooling_testing_expression_tests forge_expression_worker)
      add_test(NAME forge_tooling_testing_expression_tests COMMAND forge_tooling_testing_expression_tests)
      set_tests_properties(forge_tooling_testing_expression_tests PROPERTIES TIMEOUT 300 LABELS "expression;contract")
   endif()
   add_executable(
      forge_tooling_testing_tests
      "${_forge_tooling_testing_source_root}/tests/tooling_testing/compiler_builtins_tests.cpp"
   )
   target_include_directories(forge_tooling_testing_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
   target_link_libraries(
      forge_tooling_testing_tests
      PRIVATE
         forge_tooling_testing
         forge_vm_wasm_interpret
         Boost::headers
   )

   add_custom_target(
      forge_tooling_testing_check
      COMMAND "$<TARGET_FILE:forge_tooling_testing_tests>"
      DEPENDS forge_tooling_testing_tests
      USES_TERMINAL
   )

   if(BUILD_TESTING)
      add_test(NAME forge_tooling_testing_tests COMMAND forge_tooling_testing_tests)
      set_tests_properties(forge_tooling_testing_tests PROPERTIES TIMEOUT 180)
   endif()
endif()
