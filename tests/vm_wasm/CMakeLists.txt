include(${CMAKE_CURRENT_SOURCE_DIR}/donor/sources.cmake)

if(TARGET forge_vm_wasm_optimizing)
   set(FORGE_VM_WASM_OPTIMIZING_SESSION_FIXTURE_DIRECTORY "" CACHE PATH "Optional reviewed fixture reference; consumers always use a verified fresh generation")
   set(FORGE_VM_WASM_OPTIMIZING_HELPER_TEST_AS_BYTES "0" CACHE STRING "Explicit finite helper AS allowance for diagnostics only")
   set(_forge_oc_fixture_script "${CMAKE_CURRENT_SOURCE_DIR}/export_runtime_fixtures.cmake")
   set(_forge_oc_build_config "${CMAKE_CURRENT_BINARY_DIR}/compiler-build-config-$<CONFIG>.txt")
   set(_forge_oc_build_stamp "${CMAKE_CURRENT_BINARY_DIR}/compiler-build-$<CONFIG>.json")
   string(TOUPPER "${CMAKE_BUILD_TYPE}" _forge_oc_build_type)
   file(GENERATE OUTPUT "${_forge_oc_build_config}" CONTENT
      "generator=${CMAKE_GENERATOR}\nconfiguration=$<CONFIG>\ncxx=${CMAKE_CXX_COMPILER}\ncxx_id=${CMAKE_CXX_COMPILER_ID}\ncxx_version=${CMAKE_CXX_COMPILER_VERSION}\ncxx_flags=${CMAKE_CXX_FLAGS}\ncxx_configuration_flags=${CMAKE_CXX_FLAGS_${_forge_oc_build_type}}\nlink_flags=${CMAKE_EXE_LINKER_FLAGS}\nsystem=${CMAKE_SYSTEM_NAME}\nprocessor=${CMAKE_SYSTEM_PROCESSOR}\nsysroot=${CMAKE_OSX_SYSROOT}\nmodules=${FORGE_ENABLE_MODULES}\noc=${FORGE_ENABLE_VM_WASM_OPTIMIZING}\n")
   set(_forge_oc_fixture_arguments
      "-DFORGE_OC_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
      "-DFORGE_OC_BUILD_ROOT=${CMAKE_BINARY_DIR}"
      "-DFORGE_OC_BUILD_CONFIG=${_forge_oc_build_config}"
      "-DFORGE_OC_BUILD_STAMP=${_forge_oc_build_stamp}"
      "-DFORGE_OC_COMPILER=$<TARGET_FILE:test_forge_vm_wasm_optimizing_compiler>")
   add_executable(test_forge_vm_wasm_optimizing_artifact main.cpp optimizing_artifact_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_artifact PRIVATE forge_vm_wasm_optimizing Boost::included_unit_test_framework)
   add_test(NAME test_forge_vm_wasm_optimizing_artifact COMMAND test_forge_vm_wasm_optimizing_artifact --catch_system_errors=no)
   set_tests_properties(test_forge_vm_wasm_optimizing_artifact PROPERTIES LABELS vm_wasm_optimizing)
   add_executable(test_forge_vm_wasm_optimizing_session
      main.cpp optimizing_protocol_tests.cpp optimizing_session_tests.cpp)
   target_compile_definitions(test_forge_vm_wasm_optimizing_session PRIVATE FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC=1)
   target_link_libraries(test_forge_vm_wasm_optimizing_session
      PRIVATE forge_vm_wasm_optimizing forge_core Boost::included_unit_test_framework ${CMAKE_DL_LIBS})
   add_dependencies(test_forge_vm_wasm_optimizing_session forge_wasm_compiler)
   add_test(NAME test_forge_vm_wasm_optimizing_session
      COMMAND ${CMAKE_COMMAND} ${_forge_oc_fixture_arguments}
         -DFORGE_OC_MODE=run -DFORGE_OC_GATE=session
         "-DFORGE_OC_CONSUMER=$<TARGET_FILE:test_forge_vm_wasm_optimizing_session>"
         "-DFORGE_OC_HELPER=$<TARGET_FILE:forge_wasm_compiler>"
         "-DFORGE_OC_AS_BYTES=${FORGE_VM_WASM_OPTIMIZING_HELPER_TEST_AS_BYTES}"
         -P "${_forge_oc_fixture_script}")
   set_tests_properties(test_forge_vm_wasm_optimizing_session PROPERTIES
      LABELS vm_wasm_optimizing TIMEOUT 130 FIXTURES_REQUIRED forge_oc_runtime_fixtures)
   # Permanent normal-root lifecycle/startup/cache controls are in the SAME
   # session test owner above. Resource acceptance remains explicit finite input,
   # not a guessed default for the linked helper's host virtual-address baseline.
   add_executable(test_forge_vm_wasm_optimizing_session_resource main.cpp optimizing_session_tests.cpp)
   target_compile_definitions(test_forge_vm_wasm_optimizing_session_resource PRIVATE
      FORGE_OC_SPAWN_PUBLICATION_DIAGNOSTIC=1 FORGE_OC_HELPER_RESOURCE_DIAGNOSTIC=1)
   target_link_libraries(test_forge_vm_wasm_optimizing_session_resource
      PRIVATE forge_vm_wasm_optimizing forge_core Boost::included_unit_test_framework ${CMAKE_DL_LIBS})
   add_dependencies(test_forge_vm_wasm_optimizing_session_resource forge_wasm_compiler)
   add_test(NAME test_forge_vm_wasm_optimizing_session_resource
      COMMAND ${CMAKE_COMMAND} ${_forge_oc_fixture_arguments}
         -DFORGE_OC_MODE=run -DFORGE_OC_GATE=resource
         "-DFORGE_OC_CONSUMER=$<TARGET_FILE:test_forge_vm_wasm_optimizing_session_resource>"
         "-DFORGE_OC_HELPER=$<TARGET_FILE:forge_wasm_compiler>"
         "-DFORGE_OC_AS_BYTES=${FORGE_VM_WASM_OPTIMIZING_HELPER_TEST_AS_BYTES}"
         -P "${_forge_oc_fixture_script}")
   set_tests_properties(test_forge_vm_wasm_optimizing_session_resource PROPERTIES
      LABELS vm_wasm_optimizing TIMEOUT 130 FIXTURES_REQUIRED forge_oc_runtime_fixtures)
endif()

if(TARGET forge_tooling_wasm_compiler)
   add_executable(test_forge_vm_wasm_optimizing_compiler main.cpp optimizing_compiler_tests.cpp optimizing_epoch_ir_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_compiler PRIVATE forge_tooling_wasm_compiler Boost::included_unit_test_framework)
   # Frozen owning build -> POST_BUILD binding. CTest only verifies; it never
   # rebuilds or blesses an old executable with newly edited source bytes.
   add_custom_command(TARGET test_forge_vm_wasm_optimizing_compiler POST_BUILD
      COMMAND ${CMAKE_COMMAND} ${_forge_oc_fixture_arguments} -DFORGE_OC_MODE=bind
         -P "${_forge_oc_fixture_script}"
      BYPRODUCTS "${_forge_oc_build_stamp}"
      VERBATIM)
   add_test(NAME test_forge_vm_wasm_optimizing_runtime_fixture_setup
      COMMAND ${CMAKE_COMMAND} ${_forge_oc_fixture_arguments}
         -DFORGE_OC_MODE=setup
         "-DFORGE_OC_REVIEWED_DIRECTORY=${FORGE_VM_WASM_OPTIMIZING_SESSION_FIXTURE_DIRECTORY}"
         -P "${_forge_oc_fixture_script}")
   set_tests_properties(test_forge_vm_wasm_optimizing_runtime_fixture_setup PROPERTIES
      LABELS vm_wasm_optimizing TIMEOUT 150 FIXTURES_SETUP forge_oc_runtime_fixtures)
   add_test(NAME test_forge_vm_wasm_optimizing_compiler COMMAND test_forge_vm_wasm_optimizing_compiler --catch_system_errors=no)
   set_tests_properties(test_forge_vm_wasm_optimizing_compiler PROPERTIES LABELS vm_wasm_optimizing)
   set(FORGE_VM_WASM_OPTIMIZING_DONOR_ROOT "" CACHE PATH "Existing pinned Spring checkout used only for OC provenance verification")
   if(FORGE_VM_WASM_OPTIMIZING_DONOR_ROOT)
      find_package(LLVM 22.1.8 EXACT REQUIRED CONFIG)
      get_filename_component(_forge_vm_wasm_source_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
      add_test(NAME test_forge_vm_wasm_optimizing_donor_source_mapping
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/verify_donor_sources.py
            --root ${_forge_vm_wasm_source_root}
            --optimizing-donor ${FORGE_VM_WASM_OPTIMIZING_DONOR_ROOT}
            --llvm-include ${LLVM_INCLUDE_DIRS}
            --check
      )
      set_tests_properties(test_forge_vm_wasm_optimizing_donor_source_mapping PROPERTIES LABELS vm_wasm_optimizing)
   endif()
endif()

option(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY "Exclude JIT from the VM test backend matrix" OFF)
option(FORGE_VM_WASM_INTERPRET_ENABLE_FUZZ_TESTS "Build the Clang libFuzzer VM parser target" OFF)

set(FORGE_VM_WASM_INTERPRET_TEST_INCLUDE_DIRS
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/donor/support
)
set(FORGE_VM_WASM_INTERPRET_TEST_DEFINITIONS
   FORGE_VM_WASM_INTERPRET_FIXTURE_DIRECTORY="${PROJECT_SOURCE_DIR}/vendor/eos-vm-test-wasms/"
   FORGE_VM_WASM_INTERPRET_HOST_WASM="${CMAKE_CURRENT_SOURCE_DIR}/donor/support/host.wasm"
)
# The VM, not Boost.Test, owns SIGSEGV and SIGBUS while guarded guest memory is active.

add_executable(
   test_forge_vm_wasm_interpret_unit
   main.cpp
   port_regression_tests.cpp
   options_preparation_tests.cpp
   numeric_execution_tests.cpp
   watchdog_regression_tests.cpp
   ${FORGE_VM_WASM_INTERPRET_DONOR_UNIT_SOURCES}
)
target_include_directories(test_forge_vm_wasm_interpret_unit PRIVATE ${FORGE_VM_WASM_INTERPRET_TEST_INCLUDE_DIRS})
target_compile_definitions(test_forge_vm_wasm_interpret_unit PRIVATE ${FORGE_VM_WASM_INTERPRET_TEST_DEFINITIONS})
if(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
   target_compile_definitions(test_forge_vm_wasm_interpret_unit PRIVATE FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=1)
endif()
target_link_libraries(test_forge_vm_wasm_interpret_unit PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(
   NAME test_forge_vm_wasm_interpret_unit
   COMMAND test_forge_vm_wasm_interpret_unit --catch_system_errors=no --log_level=test_suite
)
set_tests_properties(test_forge_vm_wasm_interpret_unit PROPERTIES LABELS vm_wasm_interpret)

add_executable(test_forge_vm_wasm_prepared_validation main.cpp prepared_validation_tests.cpp)
target_link_libraries(test_forge_vm_wasm_prepared_validation PRIVATE
   forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_prepared_validation
   COMMAND test_forge_vm_wasm_prepared_validation --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_prepared_validation PROPERTIES LABELS vm_wasm_interpret TIMEOUT 10)

add_executable(test_forge_vm_wasm_invocation main.cpp invocation_scope_tests.cpp)
if(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
   target_compile_definitions(test_forge_vm_wasm_invocation PRIVATE FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=1)
endif()
target_link_libraries(test_forge_vm_wasm_invocation PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_invocation COMMAND test_forge_vm_wasm_invocation --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_invocation PROPERTIES LABELS vm_wasm_interpret TIMEOUT 30)

add_executable(
   test_forge_vm_wasm_interpret_internal
   main.cpp
   signals_regression_tests.cpp
   ${FORGE_VM_WASM_INTERPRET_DONOR_INTERNAL_SOURCES}
)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$" AND NOT FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
   target_sources(test_forge_vm_wasm_interpret_internal PRIVATE x86_64_regression_tests.cpp)
endif()
target_include_directories(
   test_forge_vm_wasm_interpret_internal
   PRIVATE
      ${FORGE_VM_WASM_INTERPRET_TEST_INCLUDE_DIRS}
)
target_compile_definitions(test_forge_vm_wasm_interpret_internal PRIVATE ${FORGE_VM_WASM_INTERPRET_TEST_DEFINITIONS})
if(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
   target_compile_definitions(test_forge_vm_wasm_interpret_internal PRIVATE FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=1)
endif()
target_link_libraries(test_forge_vm_wasm_interpret_internal PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_interpret_internal COMMAND test_forge_vm_wasm_interpret_internal --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_interpret_internal PROPERTIES LABELS vm_wasm_interpret)

add_executable(test_forge_vm_wasm_interpret_native_signals main.cpp native_signal_boundary_tests.cpp)
target_include_directories(
   test_forge_vm_wasm_interpret_native_signals PRIVATE ${FORGE_VM_WASM_INTERPRET_TEST_INCLUDE_DIRS}
)
target_compile_definitions(
   test_forge_vm_wasm_interpret_native_signals PRIVATE ${FORGE_VM_WASM_INTERPRET_TEST_DEFINITIONS}
)
target_link_libraries(
   test_forge_vm_wasm_interpret_native_signals PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework
)
add_test(
   NAME test_forge_vm_wasm_interpret_native_signals
   COMMAND test_forge_vm_wasm_interpret_native_signals --catch_system_errors=no
)
set_tests_properties(test_forge_vm_wasm_interpret_native_signals PROPERTIES LABELS vm_wasm_interpret TIMEOUT 30)

add_executable(
   test_forge_vm_wasm_interpret_spec
   main.cpp
   ${FORGE_VM_WASM_INTERPRET_DONOR_SPEC_SOURCES}
)
target_include_directories(test_forge_vm_wasm_interpret_spec PRIVATE ${FORGE_VM_WASM_INTERPRET_TEST_INCLUDE_DIRS})
target_compile_definitions(test_forge_vm_wasm_interpret_spec PRIVATE ${FORGE_VM_WASM_INTERPRET_TEST_DEFINITIONS})
if(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
   target_compile_definitions(test_forge_vm_wasm_interpret_spec PRIVATE FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=1)
endif()
target_link_libraries(test_forge_vm_wasm_interpret_spec PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_interpret_spec COMMAND test_forge_vm_wasm_interpret_spec --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_interpret_spec PROPERTIES LABELS vm_wasm_interpret)

target_sources(test_forge_vm_wasm_interpret_unit PRIVATE softfloat_tls_tests.cpp)
target_sources(test_forge_vm_wasm_interpret_unit PRIVATE floating_point_environment_tests.cpp)

if(TARGET forge_vm_wasm_optimizing)
   add_executable(test_forge_vm_wasm_optimizing_epoch main.cpp executor_epoch_tests.cpp)
   target_include_directories(test_forge_vm_wasm_optimizing_epoch PRIVATE
      "${PROJECT_SOURCE_DIR}/libraries/vm/wasm/optimizing")
   target_link_libraries(test_forge_vm_wasm_optimizing_epoch PRIVATE
      forge_vm_wasm_optimizing forge_asio Boost::included_unit_test_framework)
   add_test(NAME test_forge_vm_wasm_optimizing_epoch
      COMMAND test_forge_vm_wasm_optimizing_epoch --catch_system_errors=no --log_level=test_suite)
   set_tests_properties(test_forge_vm_wasm_optimizing_epoch PROPERTIES LABELS vm_wasm_optimizing TIMEOUT 30)
   add_executable(test_forge_vm_wasm_optimizing_executor main.cpp executor_tests.cpp)
   target_sources(test_forge_vm_wasm_optimizing_executor PRIVATE executor_compiler_fixture_tests.cpp)
   target_include_directories(test_forge_vm_wasm_optimizing_executor PRIVATE
      "${PROJECT_SOURCE_DIR}/libraries/vm/wasm/optimizing")
   target_link_libraries(test_forge_vm_wasm_optimizing_executor PRIVATE
      forge_vm_wasm_optimizing forge_asio Boost::included_unit_test_framework)
   add_test(NAME test_forge_vm_wasm_optimizing_executor
      COMMAND test_forge_vm_wasm_optimizing_executor --catch_system_errors=no --log_level=test_suite)
   set_tests_properties(test_forge_vm_wasm_optimizing_executor PROPERTIES LABELS vm_wasm_optimizing TIMEOUT 30)
   # Ordinary TU diagnostic: public dependency/module map only, no owning-test
   # object. Explicit reviewed request/artifact arguments are supplied at run.
   add_executable(test_forge_vm_wasm_optimizing_ordinary_consumer
      "${PROJECT_SOURCE_DIR}/tests/package_vm_wasm_optimizing_component/main.cpp")
   target_link_libraries(test_forge_vm_wasm_optimizing_ordinary_consumer PRIVATE forge_vm_wasm_optimizing)
endif()

if(FORGE_VM_WASM_INTERPRET_DONOR_ROOT)
   add_test(
      NAME test_forge_vm_wasm_interpret_donor_source_mapping
      COMMAND
         ${Python3_EXECUTABLE}
         ${CMAKE_CURRENT_SOURCE_DIR}/verify_donor_sources.py
         --donor ${FORGE_VM_WASM_INTERPRET_DONOR_ROOT}
         --root ${PROJECT_SOURCE_DIR}
         --check
   )
   set_tests_properties(test_forge_vm_wasm_interpret_donor_source_mapping PROPERTIES LABELS vm_wasm_interpret)

   add_test(
      NAME test_forge_vm_wasm_interpret_donor_mapping
      COMMAND
         ${Python3_EXECUTABLE}
         ${CMAKE_CURRENT_SOURCE_DIR}/port_donor_tests.py
         --donor ${FORGE_VM_WASM_INTERPRET_DONOR_ROOT}
         --fixtures ${PROJECT_SOURCE_DIR}/vendor/eos-vm-test-wasms
         --check
   )
   set_tests_properties(test_forge_vm_wasm_interpret_donor_mapping PROPERTIES LABELS vm_wasm_interpret)
endif()

add_executable(forge_vm_wasm_interpret_spec_test_generator spec_test_generator.cpp)
add_custom_target(
   test_forge_vm_wasm_interpret_spec_generator_mapping
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/verify_spec_generator.py
      --generator $<TARGET_FILE:forge_vm_wasm_interpret_spec_test_generator>
      --source ${CMAKE_CURRENT_SOURCE_DIR}
   DEPENDS forge_vm_wasm_interpret_spec_test_generator
   VERBATIM
)
add_test(
   NAME test_forge_vm_wasm_interpret_spec_generator_mapping
   COMMAND
      ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/verify_spec_generator.py
      --generator $<TARGET_FILE:forge_vm_wasm_interpret_spec_test_generator>
      --source ${CMAKE_CURRENT_SOURCE_DIR}
)
set_tests_properties(test_forge_vm_wasm_interpret_spec_generator_mapping PROPERTIES LABELS vm_wasm_interpret)

if(FORGE_VM_WASM_INTERPRET_ENABLE_FUZZ_TESTS)
   if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR "FORGE_VM_WASM_INTERPRET_ENABLE_FUZZ_TESTS requires Clang libFuzzer")
   endif()
   add_executable(forge_vm_wasm_interpret_fuzz fuzz_driver.cpp)
   target_link_libraries(forge_vm_wasm_interpret_fuzz PRIVATE forge_vm_wasm_interpret)
   target_compile_options(forge_vm_wasm_interpret_fuzz PRIVATE -fsanitize=fuzzer,address)
   target_link_options(forge_vm_wasm_interpret_fuzz PRIVATE -fsanitize=fuzzer,address)
endif()

# This normal-root gate requires the reviewed runtime-owned test target. A
# split compiler-only checkout must not silently report missing genuine tests
# as accepted; synchronize that existing owner before normal-root configure.
if(TARGET forge_vm_wasm_optimizing)
   if(NOT TARGET test_forge_vm_wasm_optimizing_executor)
      message(FATAL_ERROR "OC acceptance requires the existing runtime-owned executor test target")
   endif()
   if(NOT TARGET test_forge_vm_wasm_optimizing_ordinary_consumer)
      add_executable(test_forge_vm_wasm_optimizing_ordinary_consumer
         "${PROJECT_SOURCE_DIR}/tests/package_vm_wasm_optimizing_component/main.cpp")
      target_link_libraries(test_forge_vm_wasm_optimizing_ordinary_consumer PRIVATE forge_vm_wasm_optimizing)
   endif()
   foreach(gate IN ITEMS executor ordinary)
      if(gate STREQUAL "executor")
         set(consumer test_forge_vm_wasm_optimizing_executor)
         set(name test_forge_vm_wasm_optimizing_executor_compiled)
      else()
         set(consumer test_forge_vm_wasm_optimizing_ordinary_consumer)
         set(name test_forge_vm_wasm_optimizing_ordinary_consumer)
      endif()
      add_test(NAME ${name} COMMAND ${CMAKE_COMMAND} ${_forge_oc_fixture_arguments}
         -DFORGE_OC_MODE=run "-DFORGE_OC_GATE=${gate}"
         "-DFORGE_OC_CONSUMER=$<TARGET_FILE:${consumer}>"
         -P "${_forge_oc_fixture_script}")
      set_tests_properties(${name} PROPERTIES
         LABELS vm_wasm_optimizing TIMEOUT 70 FIXTURES_REQUIRED forge_oc_runtime_fixtures)
   endforeach()
endif()
