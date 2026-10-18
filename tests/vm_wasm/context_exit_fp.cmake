# Test-local fenv faults in the same pinned common loader/runtime source.
get_target_property(_exit_vm_sources forge_vm_wasm_interpret SOURCES)
list(FILTER _exit_vm_sources INCLUDE REGEX "/wasm_runtime_common[.]c$")
list(LENGTH _exit_vm_sources _exit_common_count)
if(NOT _exit_common_count EQUAL 1)
   message(FATAL_ERROR "Context exit FP tests require the single production WAMR common source")
endif()

add_library(forge_vm_wasm_context_exit_fp_common OBJECT ${_exit_vm_sources})
foreach(property COMPILE_DEFINITIONS COMPILE_OPTIONS INCLUDE_DIRECTORIES)
   get_target_property(_exit_property forge_vm_wasm_interpret ${property})
   if(_exit_property)
      set_property(TARGET forge_vm_wasm_context_exit_fp_common PROPERTY ${property} "${_exit_property}")
   endif()
endforeach()
if(FORGE_ENABLE_VM_WASM_LLVM_JIT)
   target_include_directories(forge_vm_wasm_context_exit_fp_common SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS})
endif()
target_compile_definitions(forge_vm_wasm_context_exit_fp_common PRIVATE
   fegetenv=context_exit_probe_fegetenv
   fesetenv=context_exit_probe_fesetenv
   fesetround=context_exit_probe_fesetround)

add_executable(test_forge_vm_wasm_context_exit_fp main.cpp context_exit_tests.cpp context_exit_fp_faults.cpp
   $<TARGET_OBJECTS:forge_vm_wasm_context_exit_fp_common>)
target_compile_definitions(test_forge_vm_wasm_context_exit_fp PRIVATE FORGE_VM_WASM_INTERPRET_TEST_FP_FAULTS=1)
if(FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY)
   target_compile_definitions(test_forge_vm_wasm_context_exit_fp PRIVATE FORGE_VM_WASM_INTERPRET_TEST_INTERPRETER_ONLY=1)
endif()
target_link_libraries(test_forge_vm_wasm_context_exit_fp PRIVATE
   forge_vm_wasm_interpret Boost::included_unit_test_framework)
add_test(NAME test_forge_vm_wasm_context_exit_fp COMMAND test_forge_vm_wasm_context_exit_fp --catch_system_errors=no)
set_tests_properties(test_forge_vm_wasm_context_exit_fp PROPERTIES LABELS vm_wasm_interpret TIMEOUT 30 RUN_SERIAL TRUE)

add_executable(test_forge_vm_wasm_context_exit_allocation_failure context_exit_allocation_failure_probe.cpp)
target_link_libraries(test_forge_vm_wasm_context_exit_allocation_failure PRIVATE forge_vm_wasm_interpret)
add_test(NAME test_forge_vm_wasm_context_exit_allocation_failure COMMAND test_forge_vm_wasm_context_exit_allocation_failure)
set_tests_properties(test_forge_vm_wasm_context_exit_allocation_failure PROPERTIES
   LABELS vm_wasm_interpret TIMEOUT 15 RUN_SERIAL TRUE)
