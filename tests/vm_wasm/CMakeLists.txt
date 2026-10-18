include(${CMAKE_CURRENT_SOURCE_DIR}/donor/sources.cmake)

# Keep the pinned donor snapshot intact; its retired executable arena is not built.
list(REMOVE_ITEM FORGE_VM_WASM_INTERPRET_DONOR_UNIT_SOURCES
   ${CMAKE_CURRENT_SOURCE_DIR}/donor/unit/allocator_tests.cpp)

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
   watchdog_regression_tests.cpp
   ${FORGE_VM_WASM_INTERPRET_DONOR_UNIT_SOURCES}
)
target_sources(test_forge_vm_wasm_interpret_unit PRIVATE
   floating_point_environment_tests.cpp softfloat_tls_tests.cpp)
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

add_test(NAME test_forge_vm_wasm_floating_point_environment
   COMMAND test_forge_vm_wasm_interpret_unit --catch_system_errors=no --run_test=floating_point_environment)
add_test(NAME test_forge_vm_wasm_softfloat_tls
   COMMAND test_forge_vm_wasm_interpret_unit --catch_system_errors=no --run_test=softfloat_tls)
set_tests_properties(test_forge_vm_wasm_floating_point_environment test_forge_vm_wasm_softfloat_tls
   PROPERTIES LABELS vm_wasm_interpret TIMEOUT 60)

add_executable(test_forge_vm_wasm_prepared_validation main.cpp prepared_validation_tests.cpp)
target_link_libraries(test_forge_vm_wasm_prepared_validation PRIVATE
   forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_prepared_validation
   COMMAND test_forge_vm_wasm_prepared_validation --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_prepared_validation PROPERTIES LABELS vm_wasm_interpret TIMEOUT 10)

add_executable(test_forge_vm_wasm_numeric main.cpp numeric_execution_tests.cpp)
target_link_libraries(test_forge_vm_wasm_numeric PRIVATE
   forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_numeric
   COMMAND test_forge_vm_wasm_numeric --catch_system_errors=no --log_level=test_suite)
set_tests_properties(test_forge_vm_wasm_numeric PROPERTIES LABELS vm_wasm_interpret TIMEOUT 30)

add_executable(test_forge_vm_wasm_invocation
   main.cpp invocation_scope_tests.cpp watchdog_absolute_tests.cpp x86_64_regression_tests.cpp)
target_sources(test_forge_vm_wasm_invocation PRIVATE context_exit_tests.cpp)
include(${CMAKE_CURRENT_SOURCE_DIR}/context_exit_fp.cmake)
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

add_test(
   NAME test_forge_vm_wasm_profile_oracle_transform
   COMMAND ${Python3_EXECUTABLE} -B ${CMAKE_CURRENT_SOURCE_DIR}/test_port_donor_tests.py -v
)
set_tests_properties(test_forge_vm_wasm_profile_oracle_transform PROPERTIES LABELS vm_wasm_interpret TIMEOUT 10)

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
