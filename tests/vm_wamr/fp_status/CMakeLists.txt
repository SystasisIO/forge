# One test-only replacement TU from the exact prepared runtime source. The
# archive (including exec-env allocation) is built normally with the new layout.
get_target_property(_runtime_sources forge_vm_wasm_interpret SOURCES)
get_target_property(_runtime_identity forge_vm_wasm_interpret FORGE_WAMR_SOURCE_IDENTITY)
foreach(_name IN ITEMS wasm_runtime_common.c wasm_exec_env.c)
   set(_matches "")
   foreach(_source IN LISTS _runtime_sources)
      get_filename_component(_basename "${_source}" NAME)
      if(_basename STREQUAL _name)
         list(APPEND _matches "${_source}")
      endif()
   endforeach()
   list(LENGTH _matches _count)
   if(NOT _count EQUAL 1)
      message(FATAL_ERROR "FP regression requires exactly one runtime ${_name}; found ${_count}")
   endif()
   list(GET _matches 0 _${_name})
endforeach()
get_filename_component(_common_dir "${_wasm_runtime_common.c}" DIRECTORY)
get_filename_component(_env_dir "${_wasm_exec_env.c}" DIRECTORY)
if(NOT _common_dir STREQUAL _env_dir OR NOT _runtime_identity)
   message(FATAL_ERROR "FP regression requires one identified prepared runtime tree")
endif()

function(forge_fp_status_compile target)
   target_compile_features(${target} PRIVATE c_std_11)
   target_include_directories(${target} PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>")
   target_compile_definitions(${target} PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>")
   target_compile_options(${target} PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_OPTIONS>")
endfunction()

add_library(forge_wamr_fp_status_common OBJECT "${_wasm_runtime_common.c}")
forge_fp_status_compile(forge_wamr_fp_status_common)
target_compile_definitions(forge_wamr_fp_status_common PRIVATE
   fegetenv=probe_fegetenv fesetenv=probe_fesetenv fesetround=probe_fesetround)

add_executable(test_forge_wamr_fp_status fp_status_probe.c
   $<TARGET_OBJECTS:forge_wamr_fp_status_common>)
forge_fp_status_compile(test_forge_wamr_fp_status)
target_compile_definitions(test_forge_wamr_fp_status PRIVATE
   "FORGE_FP_STATUS_RUNTIME_IDENTITY=\"${_runtime_identity}\"")
target_link_libraries(test_forge_wamr_fp_status PRIVATE forge_vm_wasm_interpret)
set_property(TARGET test_forge_wamr_fp_status PROPERTY LINKER_LANGUAGE CXX)

# Compile the immutable pre-hook layout oracle in the actual runtime shape and
# the two LLVM-free header shapes, without building another runtime/backend.
foreach(_profile RANGE 0 2)
   add_library(forge_wamr_fp_status_layout_${_profile} OBJECT fp_status_layout.c)
   forge_fp_status_compile(forge_wamr_fp_status_layout_${_profile})
   target_compile_definitions(forge_wamr_fp_status_layout_${_profile} PRIVATE
      FORGE_FP_LAYOUT_PROFILE=${_profile})
   add_dependencies(test_forge_wamr_fp_status forge_wamr_fp_status_layout_${_profile})
endforeach()

set(_map "${CMAKE_CURRENT_BINARY_DIR}/test_forge_wamr_fp_status.linkmap")
if(APPLE)
   target_link_options(test_forge_wamr_fp_status PRIVATE "LINKER:-map,${_map}")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
   target_link_options(test_forge_wamr_fp_status PRIVATE "LINKER:-Map,${_map}")
else()
   message(FATAL_ERROR "FP regression provider proof is qualified on Darwin/Linux only")
endif()
if(NOT CMAKE_NM)
   message(FATAL_ERROR "FP regression provider proof requires CMAKE_NM")
endif()
add_test(NAME test_forge_wamr_fp_status
   COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/verify_provider.py"
      --binary "$<TARGET_FILE:test_forge_wamr_fp_status>"
      --map "${_map}" --nm "${CMAKE_NM}")
set_tests_properties(test_forge_wamr_fp_status PROPERTIES
   TIMEOUT 30 LABELS "vm_wasm_execution;fp_status" RUN_SERIAL TRUE)
add_test(NAME test_forge_wamr_fp_status_provider_selftest
   COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/test_verify_provider.py" -v)
set_tests_properties(test_forge_wamr_fp_status_provider_selftest PROPERTIES
   TIMEOUT 10 LABELS "vm_wasm_execution;fp_status")
