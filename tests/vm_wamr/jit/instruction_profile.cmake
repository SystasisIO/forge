# Qualification of the complete reviewed runtime, never a candidate-TU overlay
# on an older archive. The coordinator includes this fragment explicitly.
function(forge_add_wamr_instruction_profile_probe candidate_root)
   if(NOT FORGE_ENABLE_VM_WASM_LLVM_JIT OR NOT TARGET forge_vm_wasm_interpret)
      message(FATAL_ERROR "Instruction profiling requires the existing LLVM-JIT runtime")
   endif()
   find_package(Git REQUIRED)
   find_package(LLVM 22.1.8 EXACT REQUIRED CONFIG)
   execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${candidate_root}" rev-parse HEAD
      OUTPUT_VARIABLE _base OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
   get_target_property(_runtime_identity forge_vm_wasm_interpret FORGE_WAMR_SOURCE_IDENTITY)
   if(NOT _runtime_identity MATCHES "^${_base}-")
      message(FATAL_ERROR "Profile qualification requires the complete pinned candidate runtime, not an old archive")
   endif()

   get_target_property(_sources forge_vm_wasm_interpret SOURCES)
   set(_compiler "")
   foreach(_name IN ITEMS aot_compiler.c aot_llvm.c aot_jit_profile.cpp)
      set(_count 0)
      foreach(_source IN LISTS _sources)
         get_filename_component(_filename "${_source}" NAME)
         if(_filename STREQUAL _name)
            math(EXPR _count "${_count}+1")
            get_filename_component(_compiler "${_source}" DIRECTORY)
         endif()
      endforeach()
      if(NOT _count EQUAL 1)
         message(FATAL_ERROR "The existing WAMR runtime must compile ${_name} exactly once (found ${_count})")
      endif()
   endforeach()
   get_filename_component(_iwasm "${_compiler}/.." ABSOLUTE)
   set(_fingerprint "${_runtime_identity}")
   foreach(_file IN ITEMS
         compilation/aot_compiler.c compilation/aot_llvm.c compilation/aot_llvm.h
         compilation/aot_jit_profile.cpp compilation/aot_jit_profile.h
         compilation/iwasm_compl.cmake interpreter/wasm_loader.c interpreter/wasm_loader.h
         interpreter/wasm.h interpreter/wasm_runtime.h common/wasm_runtime_common.h
         include/wasm_export.h include/wasm_c_api.h)
      file(SHA256 "${_iwasm}/${_file}" _actual)
      file(SHA256 "${candidate_root}/core/iwasm/${_file}" _candidate)
      if(NOT _actual STREQUAL _candidate)
         message(FATAL_ERROR "Runtime/profile source mismatch: ${_file}; rebuild the whole reviewed fork")
      endif()
      string(APPEND _fingerprint ";${_file}=${_actual}")
   endforeach()
   string(SHA256 _candidate_identity "${_fingerprint}")
   message(STATUS "Instruction profile runtime identity=${_candidate_identity}; runtime=${_runtime_identity}")

   # All compiler/profile/loader implementations are already members of this
   # one WAMR archive. No alternate compiler objects, vmlib or aotclib are added.
   add_executable(test_forge_wamr_jit_instruction_profile
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/instruction_profile_probe.cpp")
   target_include_directories(test_forge_wamr_jit_instruction_profile PRIVATE
      "${PROJECT_SOURCE_DIR}/libraries/vm/wasm/interpret"
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,INCLUDE_DIRECTORIES>"
      ${LLVM_INCLUDE_DIRS})
   target_compile_definitions(test_forge_wamr_jit_instruction_profile PRIVATE
      "$<TARGET_PROPERTY:forge_vm_wasm_interpret,COMPILE_DEFINITIONS>"
      "FORGE_WAMR_PROFILE_PROOF_IDENTITY=\"${_candidate_identity}\"")
   target_compile_options(test_forge_wamr_jit_instruction_profile PRIVATE
      "$<$<COMPILE_LANGUAGE:C,CXX>:-fno-fast-math;-ffp-contract=off;-frounding-math>")
   target_link_libraries(test_forge_wamr_jit_instruction_profile PRIVATE forge_vm_wasm_interpret LLVM)
   if(APPLE)
      target_link_options(test_forge_wamr_jit_instruction_profile PRIVATE
         "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/test_forge_wamr_jit_instruction_profile.linkmap")
   elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(test_forge_wamr_jit_instruction_profile PRIVATE
         "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/test_forge_wamr_jit_instruction_profile.linkmap")
   endif()
   add_test(NAME test_forge_wamr_jit_instruction_profile
      COMMAND test_forge_wamr_jit_instruction_profile runtime)
   set_tests_properties(test_forge_wamr_jit_instruction_profile PROPERTIES
      TIMEOUT 30 LABELS "vm_wasm_interpret;jit;instruction_profile" RUN_SERIAL TRUE)
   foreach(_scenario IN ITEMS plain empty multi early_heap early_cancel late_cancel mapping)
      add_test(NAME "test_forge_wamr_jit_instruction_profile_${_scenario}"
         COMMAND test_forge_wamr_jit_instruction_profile "${_scenario}")
      set_tests_properties("test_forge_wamr_jit_instruction_profile_${_scenario}" PROPERTIES
         TIMEOUT 30 LABELS "vm_wasm_interpret;jit;instruction_profile" RUN_SERIAL TRUE)
   endforeach()
endfunction()
