add_executable(test_forge_vm_wasm_execution main.cpp execution_tests.cpp public_runtime_tests.cpp public_allocator_tests.cpp allocator_lifetime_tests.cpp)
target_sources(test_forge_vm_wasm_execution PRIVATE export_name_tests.cpp)
target_link_libraries(test_forge_vm_wasm_execution PRIVATE forge_vm_wasm_execution Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_execution COMMAND test_forge_vm_wasm_execution --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_execution PROPERTIES TIMEOUT 30 LABELS vm_wasm_execution)

add_executable(test_forge_vm_wasm_public_bridge public_bridge_probe.cpp)
target_link_libraries(test_forge_vm_wasm_public_bridge PRIVATE forge_vm_wasm_interpret)

add_executable(test_forge_vm_wasm_engine_memory main.cpp engine_memory_tests.cpp)
target_link_libraries(test_forge_vm_wasm_engine_memory PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
target_include_directories(test_forge_vm_wasm_engine_memory PRIVATE
   "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>")
target_compile_definitions(test_forge_vm_wasm_engine_memory PRIVATE
   "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>")
add_test(NAME test_forge_vm_wasm_engine_memory COMMAND test_forge_vm_wasm_engine_memory --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_engine_memory PROPERTIES TIMEOUT 10 LABELS vm_wasm_execution)

add_executable(test_forge_vm_wasm_consumer_api main.cpp consumer_api_tests.cpp)
target_link_libraries(test_forge_vm_wasm_consumer_api PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_consumer_api COMMAND test_forge_vm_wasm_consumer_api --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_consumer_api PROPERTIES TIMEOUT 15 LABELS vm_wasm_execution)

add_executable(test_forge_vm_wasm_timing_allocation timing_allocation_failure_probe.cpp)
target_link_libraries(test_forge_vm_wasm_timing_allocation PRIVATE forge_vm_wasm_interpret)
add_test(NAME test_forge_vm_wasm_timing_allocation COMMAND test_forge_vm_wasm_timing_allocation)
set_tests_properties(test_forge_vm_wasm_timing_allocation PROPERTIES TIMEOUT 10 LABELS vm_wasm_execution)

add_executable(test_forge_vm_wasm_allocator_allocation allocator_allocation_failure_probe.cpp)
target_link_libraries(test_forge_vm_wasm_allocator_allocation PRIVATE forge_vm_wasm_interpret)
add_test(NAME test_forge_vm_wasm_allocator_allocation COMMAND test_forge_vm_wasm_allocator_allocation)
set_tests_properties(test_forge_vm_wasm_allocator_allocation PROPERTIES TIMEOUT 10 LABELS vm_wasm_execution)

add_executable(test_forge_vm_wasm_preparation_budget main.cpp preparation_budget_tests.cpp)
target_link_libraries(test_forge_vm_wasm_preparation_budget PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_preparation_budget COMMAND test_forge_vm_wasm_preparation_budget --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_preparation_budget PROPERTIES TIMEOUT 30 LABELS vm_wasm_execution)

add_executable(test_forge_vm_wasm_profile_public main.cpp profile_public_tests.cpp)
target_link_libraries(test_forge_vm_wasm_profile_public PRIVATE forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_profile_public COMMAND test_forge_vm_wasm_profile_public --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_profile_public PROPERTIES
   TIMEOUT 120 LABELS "vm_wasm_execution;jit;instruction_profile" RUN_SERIAL TRUE)

add_subdirectory(../vm_wamr/fp_status "${CMAKE_CURRENT_BINARY_DIR}/fp_status")

add_executable(test_forge_wamr_jit_load ../vm_wamr/jit/load_probe.cpp)
target_link_libraries(test_forge_wamr_jit_load PRIVATE forge_vm_wasm_interpret)
target_include_directories(test_forge_wamr_jit_load PRIVATE
   "${PROJECT_SOURCE_DIR}/libraries/vm/wasm/interpret"
   "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>")
target_compile_definitions(test_forge_wamr_jit_load PRIVATE
   "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>")
foreach(_case IN ITEMS deferred compile legacy)
   add_test(NAME test_forge_wamr_jit_${_case} COMMAND test_forge_wamr_jit_load ${_case})
   set_tests_properties(test_forge_wamr_jit_${_case} PROPERTIES TIMEOUT 60 LABELS "vm_wasm_interpret;jit" RUN_SERIAL TRUE)
endforeach()
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   add_test(NAME test_forge_wamr_jit_bridge COMMAND test_forge_wamr_jit_load bridge)
   set_tests_properties(test_forge_wamr_jit_bridge PROPERTIES TIMEOUT 60 LABELS "vm_wasm_interpret;jit" RUN_SERIAL TRUE)
   find_package(LLVM 22.1.8 EXACT REQUIRED CONFIG)
   add_executable(test_forge_wamr_jit_native_maps ../vm_wamr/jit/native_maps_probe.cpp)
   target_link_libraries(test_forge_wamr_jit_native_maps PRIVATE forge_vm_wasm_interpret LLVM)
   target_include_directories(test_forge_wamr_jit_native_maps PRIVATE
      "${PROJECT_SOURCE_DIR}/libraries/vm/wasm/interpret"
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>"
      ${LLVM_INCLUDE_DIRS})
   target_compile_definitions(test_forge_wamr_jit_native_maps PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>")
   add_test(NAME test_forge_wamr_jit_native_maps COMMAND test_forge_wamr_jit_native_maps)
   set_tests_properties(test_forge_wamr_jit_native_maps PROPERTIES
      TIMEOUT 120 LABELS "vm_wasm_interpret;jit;native_mapping" RUN_SERIAL TRUE)
   add_executable(test_forge_wamr_jit_profile_map ../vm_wamr/jit/profile_map_probe.cpp)
   target_link_libraries(test_forge_wamr_jit_profile_map PRIVATE forge_vm_wasm_interpret LLVM)
   target_include_directories(test_forge_wamr_jit_profile_map PRIVATE
      "${PROJECT_SOURCE_DIR}/libraries/vm/wasm/interpret"
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>"
      ${LLVM_INCLUDE_DIRS})
   target_compile_definitions(test_forge_wamr_jit_profile_map PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>")
   add_test(NAME test_forge_wamr_jit_profile_map COMMAND test_forge_wamr_jit_profile_map)
   set_tests_properties(test_forge_wamr_jit_profile_map PROPERTIES
      TIMEOUT 30 LABELS "vm_wasm_interpret;jit;instruction_profile" RUN_SERIAL TRUE)
   add_executable(test_forge_wamr_jit_error_threads ../vm_wamr/jit/error_thread_probe.cpp)
   target_link_libraries(test_forge_wamr_jit_error_threads PRIVATE forge_vm_wasm_interpret)
   target_include_directories(test_forge_wamr_jit_error_threads PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>")
   target_compile_definitions(test_forge_wamr_jit_error_threads PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>")
   add_test(NAME test_forge_wamr_jit_error_threads COMMAND test_forge_wamr_jit_error_threads)
   set_tests_properties(test_forge_wamr_jit_error_threads PROPERTIES
      TIMEOUT 30 LABELS "vm_wasm_interpret;jit;thread_isolation" RUN_SERIAL TRUE)
   foreach(_probe IN ITEMS test_forge_wamr_jit_load test_forge_wamr_jit_native_maps test_forge_wamr_jit_error_threads)
      if(APPLE)
         target_link_options(${_probe} PRIVATE "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/${_probe}.linkmap")
      elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
         target_link_options(${_probe} PRIVATE "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/${_probe}.linkmap")
      endif()
   endforeach()
endif()

foreach(_order IN ITEMS 0 1)
   add_library(forge_wamr_load_args_${_order} OBJECT ../vm_wamr/jit/load_args_probe.cpp)
   target_include_directories(forge_wamr_load_args_${_order} PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>")
   target_compile_definitions(forge_wamr_load_args_${_order} PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>" FORGE_WAMR_C_API_FIRST=${_order})
endforeach()
add_executable(test_forge_wamr_load_args ../vm_wamr/jit/load_args_main.cpp
   $<TARGET_OBJECTS:forge_wamr_load_args_0> $<TARGET_OBJECTS:forge_wamr_load_args_1>)
add_test(NAME test_forge_wamr_load_args COMMAND test_forge_wamr_load_args)
set_tests_properties(test_forge_wamr_load_args PROPERTIES TIMEOUT 10 LABELS "vm_wasm_interpret;jit")

add_executable(wamr_capacity_baseline ../vm_wamr/capacity_probe.cpp)
target_compile_definitions(wamr_capacity_baseline PRIVATE FORGE_WAMR_CAPACITY_BASELINE=1)
target_link_libraries(wamr_capacity_baseline PRIVATE forge_vm_wasm_execution)

add_executable(test_forge_vm_wasm_execution_sdk sdk_fixture.cpp)
target_link_libraries(test_forge_vm_wasm_execution_sdk PRIVATE forge_vm_wasm_execution)
set(FORGE_VM_WASM_EXECUTION_SDK_FIXTURE "" CACHE FILEPATH "Real compiled guest/tests/wire_values fixture")
if(FORGE_VM_WASM_EXECUTION_SDK_FIXTURE)
   add_test(NAME test_forge_vm_wasm_execution_sdk
      COMMAND test_forge_vm_wasm_execution_sdk "${FORGE_VM_WASM_EXECUTION_SDK_FIXTURE}")
   set_tests_properties(test_forge_vm_wasm_execution_sdk PROPERTIES TIMEOUT 10 LABELS vm_wasm_execution)
endif()

set(FORGE_WAMR_INSTRUCTION_PROFILE_CANDIDATE "" CACHE PATH
   "Unpublished WAMR compiler sources for the opt-in instruction profiling proof")
if(FORGE_WAMR_INSTRUCTION_PROFILE_CANDIDATE)
   include("${PROJECT_SOURCE_DIR}/tests/vm_wamr/jit/instruction_profile.cmake")
   forge_add_wamr_instruction_profile_probe("${FORGE_WAMR_INSTRUCTION_PROFILE_CANDIDATE}")
endif()
