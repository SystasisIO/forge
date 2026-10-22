add_executable(wamr_heavy_fixture heavy_fixture.cpp)
target_compile_features(wamr_heavy_fixture PRIVATE cxx_std_23)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/heavy-16384.wasm" "${CMAKE_CURRENT_BINARY_DIR}/heavy-32768.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-body.wasm" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-initializer.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/traps.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/multi-result.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-entry.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-start.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-post_instantiate.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-healthy.wasm"
   COMMAND wamr_heavy_fixture "${CMAKE_CURRENT_BINARY_DIR}" DEPENDS wamr_heavy_fixture VERBATIM)
if(TARGET forge_vm_wasm_compiler)
   add_executable(wamr_compile_fixture compile_fixture.cpp)
   target_link_libraries(wamr_compile_fixture PRIVATE forge_vm_wasm_compiler)
   add_executable(wamr_export_name_fixture export_name_tests.cpp)
   target_compile_definitions(wamr_export_name_fixture PRIVATE
      FORGE_WAMR_EXPORT_NAME_COMPILER_FIXTURE=1)
   target_link_libraries(wamr_export_name_fixture PRIVATE forge_vm_wasm_compiler)
   add_executable(test_forge_vm_wasm_optimizing_export_names export_name_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_export_names PRIVATE forge_vm_wasm_optimizing)
   set(_export_names "${CMAKE_CURRENT_BINARY_DIR}/export-names")
   add_test(NAME wamr_export_names_compile COMMAND wamr_export_name_fixture "${_export_names}")
   set_tests_properties(wamr_export_names_compile PROPERTIES
      FIXTURES_SETUP wamr_export_names TIMEOUT 180 RUN_SERIAL TRUE LABELS "wamr;optimizing;export_names")
   add_test(NAME wamr_export_names_runtime
      COMMAND test_forge_vm_wasm_optimizing_export_names "${_export_names}")
   set_tests_properties(wamr_export_names_runtime PROPERTIES
      FIXTURES_REQUIRED wamr_export_names TIMEOUT 180 RUN_SERIAL TRUE LABELS "wamr;optimizing;export_names")
   if(BUILD_TESTING)
      if(APPLE)
         target_link_options(wamr_compile_fixture PRIVATE "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/wamr_compile_fixture.linkmap")
      elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
         target_link_options(wamr_compile_fixture PRIVATE "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/wamr_compile_fixture.linkmap")
      endif()
   endif()
   foreach(_size IN ITEMS 8192 16384 32768)
      add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.request"
         COMMAND wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.wasm"
            "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.request"
         DEPENDS wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.wasm" VERBATIM)
   endforeach()
   foreach(_variant IN ITEMS body initializer)
      add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-${_variant}.foc"
         "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-${_variant}.foc.request"
         COMMAND wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-${_variant}.wasm"
            "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-${_variant}.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-${_variant}.foc.request"
         DEPENDS wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-${_variant}.wasm" VERBATIM)
   endforeach()
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/traps.foc" "${CMAKE_CURRENT_BINARY_DIR}/traps.request"
      COMMAND wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/traps.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/traps.foc" "${CMAKE_CURRENT_BINARY_DIR}/traps.request"
      DEPENDS wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/traps.wasm" VERBATIM)
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.foc"
      "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.request"
      COMMAND wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.foc"
         "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.request" operational-oracles
      DEPENDS wamr_compile_fixture "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.wasm" VERBATIM)
   add_custom_target(wamr_optimizing_fixtures DEPENDS
      "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-16384.foc"
      "${CMAKE_CURRENT_BINARY_DIR}/heavy-32768.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-body.foc"
      "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-initializer.foc" "${CMAKE_CURRENT_BINARY_DIR}/traps.foc"
      "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.foc")
   set(_stack_capacity_fixtures)
   foreach(_case IN ITEMS entry start post_instantiate healthy)
      set(_fixture "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-${_case}")
      add_custom_command(OUTPUT "${_fixture}.foc" "${_fixture}.request"
         COMMAND wamr_compile_fixture "${_fixture}.wasm" "${_fixture}.foc" "${_fixture}.request"
         DEPENDS wamr_compile_fixture "${_fixture}.wasm" VERBATIM)
      list(APPEND _stack_capacity_fixtures "${_fixture}.foc" "${_fixture}.request")
   endforeach()
   add_custom_target(wamr_optimizing_stack_capacity_fixtures DEPENDS ${_stack_capacity_fixtures})
   add_executable(test_forge_vm_wasm_optimizing_stack_capacity stack_capacity_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_stack_capacity PRIVATE forge_vm_wasm_optimizing)
   add_dependencies(test_forge_vm_wasm_optimizing_stack_capacity wamr_optimizing_stack_capacity_fixtures)
   foreach(_case IN ITEMS entry start post_instantiate)
      add_test(NAME "wamr_optimizing_stack_capacity_${_case}"
         COMMAND test_forge_vm_wasm_optimizing_stack_capacity "${_case}"
            "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-${_case}.foc"
            "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-${_case}.request"
            "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-healthy.foc"
            "${CMAKE_CURRENT_BINARY_DIR}/stack-capacity-healthy.request")
      set_tests_properties("wamr_optimizing_stack_capacity_${_case}" PROPERTIES
         TIMEOUT 180 LABELS "wamr;optimizing;native_stack")
   endforeach()
   add_executable(test_forge_vm_wasm_optimizing runtime_tests.cpp operational_oracles.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing PRIVATE forge_vm_wasm_optimizing)
   add_dependencies(test_forge_vm_wasm_optimizing wamr_optimizing_fixtures)
   add_executable(test_forge_vm_wasm_optimizing_traps trap_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_traps PRIVATE forge_vm_wasm_optimizing)
   add_dependencies(test_forge_vm_wasm_optimizing_traps wamr_optimizing_fixtures)
   add_executable(test_forge_vm_wasm_optimizing_preparation preparation_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_preparation PRIVATE forge_vm_wasm_optimizing)
   add_dependencies(test_forge_vm_wasm_optimizing_preparation wamr_optimizing_fixtures)
   foreach(_case IN ITEMS unreachable divide_zero overflow conversion table_bounds indirect_type depth
         private_index unexported_import indirect_import table_null memory_bounds)
      add_test(NAME "wamr_optimizing_guest_${_case}" COMMAND test_forge_vm_wasm_optimizing_traps "${_case}"
         "${CMAKE_CURRENT_BINARY_DIR}/traps.foc" "${CMAKE_CURRENT_BINARY_DIR}/traps.request")
      set_tests_properties("wamr_optimizing_guest_${_case}" PROPERTIES TIMEOUT 30 LABELS "wamr;optimizing;guest_trap")
   endforeach()
   add_executable(test_forge_vm_wasm_optimizing_session session_tests.cpp)
   if(APPLE)
      target_sources(test_forge_vm_wasm_optimizing_session PRIVATE session_late_spawn_tests.cpp)
      target_compile_definitions(test_forge_vm_wasm_optimizing_session PRIVATE
         FORGE_WAMR_SESSION_LATE_SPAWN_TEST=1)
   endif()
   target_link_libraries(test_forge_vm_wasm_optimizing_session PRIVATE forge_vm_wasm_optimizing)
   add_dependencies(test_forge_vm_wasm_optimizing_session wamr_optimizing_fixtures wasm_compiler)
   add_executable(test_forge_vm_wasm_optimizing_session_probe session_probe_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_session_probe PRIVATE forge_vm_wasm_optimizing)
   target_include_directories(test_forge_vm_wasm_optimizing_session_probe PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/vm/wasm/optimizing")
   add_dependencies(test_forge_vm_wasm_optimizing_session_probe wamr_optimizing_fixtures wasm_compiler)
   add_executable(test_forge_vm_wasm_optimizing_cleanup cleanup_tests.cpp)
   target_link_libraries(test_forge_vm_wasm_optimizing_cleanup PRIVATE forge_vm_wasm_optimizing)
   target_include_directories(test_forge_vm_wasm_optimizing_cleanup PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/vm/wasm/interpret")
   add_dependencies(test_forge_vm_wasm_optimizing_cleanup wamr_optimizing_fixtures wasm_compiler)
   set(FORGE_WAMR_COMPILER_TEST_AS_BYTES "" CACHE STRING "Explicit finite diagnostic helper address-space bytes")
   if(FORGE_WAMR_COMPILER_TEST_AS_BYTES)
      add_test(NAME wamr_optimizing_session_bounded COMMAND test_forge_vm_wasm_optimizing_session
         "$<TARGET_FILE:wasm_compiler>" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.request"
         "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.foc" "${CMAKE_CURRENT_BINARY_DIR}"
         "${FORGE_WAMR_COMPILER_TEST_AS_BYTES}" "${CMAKE_CURRENT_BINARY_DIR}/multi-result.wasm")
      set_tests_properties(wamr_optimizing_session_bounded PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE LABELS "wamr;optimizing;session")
      if(APPLE)
         add_test(NAME wamr_optimizing_session_late_spawn_cleanup
            COMMAND test_forge_vm_wasm_optimizing_session late-spawn-cleanup
               "$<TARGET_FILE:wasm_compiler>" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.request"
               "${CMAKE_CURRENT_BINARY_DIR}" "${FORGE_WAMR_COMPILER_TEST_AS_BYTES}")
         set_tests_properties(wamr_optimizing_session_late_spawn_cleanup PROPERTIES
            TIMEOUT 45 RUN_SERIAL TRUE LABELS "wamr;optimizing;session;cleanup")
         add_test(NAME wamr_optimizing_session_workspace_refusal
            COMMAND test_forge_vm_wasm_optimizing_session workspace-refusal
               "$<TARGET_FILE:wasm_compiler>" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.request"
               "${CMAKE_CURRENT_BINARY_DIR}" "${FORGE_WAMR_COMPILER_TEST_AS_BYTES}")
         set_tests_properties(wamr_optimizing_session_workspace_refusal PROPERTIES
            TIMEOUT 90 RUN_SERIAL TRUE LABELS "wamr;optimizing;session;cleanup")
      endif()
      foreach(_case IN ITEMS late overlap)
         add_test(NAME "wamr_optimizing_session_probe_${_case}"
            COMMAND test_forge_vm_wasm_optimizing_session_probe "${_case}"
               "$<TARGET_FILE:wasm_compiler>" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.request"
               "${CMAKE_CURRENT_BINARY_DIR}" "${FORGE_WAMR_COMPILER_TEST_AS_BYTES}")
         set_tests_properties("wamr_optimizing_session_probe_${_case}" PROPERTIES
            TIMEOUT 480 RUN_SERIAL TRUE LABELS "wamr;optimizing;session;preparation_reservation")
      endforeach()
      add_test(NAME wamr_optimizing_session_cleanup COMMAND test_forge_vm_wasm_optimizing_cleanup
         "$<TARGET_FILE:wasm_compiler>" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.request"
         "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.foc" "${CMAKE_CURRENT_BINARY_DIR}"
         "${FORGE_WAMR_COMPILER_TEST_AS_BYTES}")
      set_tests_properties(wamr_optimizing_session_cleanup PROPERTIES TIMEOUT 360 RUN_SERIAL TRUE
         LABELS "wamr;optimizing;session;cleanup")
   endif()
   foreach(_size IN ITEMS 8192 16384 32768)
      add_test(NAME "wamr_optimizing_${_size}_preparation" COMMAND test_forge_vm_wasm_optimizing_preparation
         "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.request")
      set_tests_properties("wamr_optimizing_${_size}_preparation" PROPERTIES TIMEOUT 180 LABELS "wamr;optimizing;preparation")
      foreach(_case IN ITEMS heavy depth deadline cancel host_error exported_import runtime_pin identity)
         add_test(NAME "wamr_optimizing_${_size}_${_case}" COMMAND test_forge_vm_wasm_optimizing "${_case}"
            "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-${_size}.request" "${_size}")
         set_tests_properties("wamr_optimizing_${_size}_${_case}" PROPERTIES TIMEOUT 180 LABELS "wamr;optimizing")
      endforeach()
   endforeach()
   add_test(NAME wamr_optimizing_untrusted_substitution COMMAND test_forge_vm_wasm_optimizing identity
      "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192.request" 8192
      "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-body.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-8192-initializer.foc")
   set_tests_properties(wamr_optimizing_untrusted_substitution PROPERTIES TIMEOUT 30 LABELS "wamr;optimizing;provenance")
   add_test(NAME wamr_optimizing_physical_capacity COMMAND test_forge_vm_wasm_optimizing physical
      "${CMAKE_CURRENT_BINARY_DIR}/heavy-32768.foc" "${CMAKE_CURRENT_BINARY_DIR}/heavy-32768.request" 32768)
   set_tests_properties(wamr_optimizing_physical_capacity PROPERTIES TIMEOUT 180 LABELS "wamr;optimizing")
   foreach(_case IN ITEMS integer cfg memory fp62 benchmark_integer benchmark_memory)
      add_test(NAME "wamr_optimizing_oracle_${_case}" COMMAND test_forge_vm_wasm_optimizing "oracle_${_case}"
         "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.foc"
         "${CMAKE_CURRENT_BINARY_DIR}/operational-oracles.request" 32768)
      set_tests_properties("wamr_optimizing_oracle_${_case}" PROPERTIES TIMEOUT 180
         LABELS "wamr;optimizing;operational_oracle")
   endforeach()
endif()

# Private allocation-owner qualification only; no production loader wiring.
add_subdirectory(preparation_plan)
