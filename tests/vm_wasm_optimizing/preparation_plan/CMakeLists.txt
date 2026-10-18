cmake_minimum_required(VERSION 3.31)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   project(forge_wamr_preparation_plan_probe LANGUAGES CXX)
   enable_testing()
endif()

# Standalone stage-1 probe. Parent owns registration in the main test graph.
get_filename_component(_forge_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
add_library(wamr_preparation_plan_probe_owner STATIC
   "${_forge_root}/libraries/vm/wasm/interpret/engine_load_plan.cpp")
target_compile_features(wamr_preparation_plan_probe_owner PRIVATE cxx_std_23)
target_compile_options(wamr_preparation_plan_probe_owner PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(wamr_preparation_plan_probe planning_owner_tests.cpp)
target_compile_features(wamr_preparation_plan_probe PRIVATE cxx_std_23)
target_compile_options(wamr_preparation_plan_probe PRIVATE -Wall -Wextra -Wpedantic -Werror)
target_link_libraries(wamr_preparation_plan_probe PRIVATE wamr_preparation_plan_probe_owner)

option(FORGE_WAMR_PREPARATION_PLAN_SANITIZERS "Sanitize the isolated stage-1 probe" OFF)
if(FORGE_WAMR_PREPARATION_PLAN_SANITIZERS)
   foreach(_target IN ITEMS wamr_preparation_plan_probe_owner wamr_preparation_plan_probe)
      target_compile_options(${_target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
   endforeach()
   target_link_options(wamr_preparation_plan_probe PRIVATE -fsanitize=address,undefined)
endif()

foreach(_case IN ITEMS metadata_limit exact_limit ledger_limit live_limit shadow_maps realloc_peak
      realloc_success realloc_failure realloc_cancellation allocation_failures cancellation invalid_requests overflow isolation rollback)
   add_test(NAME "wamr_preparation_plan_${_case}" COMMAND wamr_preparation_plan_probe "${_case}")
   set_tests_properties("wamr_preparation_plan_${_case}" PROPERTIES TIMEOUT 10 LABELS "wamr;preparation;prototype")
endforeach()
add_test(NAME wamr_preparation_plan_no_native_effects
   COMMAND "${CMAKE_COMMAND}" "-DNM=${CMAKE_NM}" "-DARCHIVE=$<TARGET_FILE:wamr_preparation_plan_probe_owner>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/verify_no_native_effects.cmake")
set_tests_properties(wamr_preparation_plan_no_native_effects PROPERTIES TIMEOUT 10 LABELS "wamr;preparation;prototype")
