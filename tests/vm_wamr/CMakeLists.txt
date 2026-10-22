cmake_minimum_required(VERSION 3.31)
project(forge_wamr_qualification LANGUAGES C CXX ASM)

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   message(FATAL_ERROR "WAMR qualification is standalone; it does not select a production backend")
endif()
enable_testing()
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
get_filename_component(FORGE_SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${FORGE_SOURCE_ROOT}/cmake/ForgeWamrSource.cmake")
include("${FORGE_SOURCE_ROOT}/cmake/ForgeWamrProfile.cmake")
forge_prepare_wamr(WAMR_ROOT_DIR _wamr_identity)

set(FORGE_WAMRC "" CACHE FILEPATH "Separately built wamrc from the pinned vendor tree")
set(FORGE_WAMR_COMPILER_PROBE "" CACHE FILEPATH "Probe linked to vm/wasm/compiler from the same source tree")
set(FORGE_GUEST_CLANG "" CACHE FILEPATH "Clang with a wasm32 backend for probe fixtures")
set(FORGE_GUEST_WASM_LD "" CACHE FILEPATH "Matching wasm-ld, when installed separately from Clang")
option(FORGE_WAMR_FAST_INTERP "Qualify the fast rather than classic interpreter" OFF)
if(NOT FORGE_GUEST_CLANG OR NOT EXISTS "${FORGE_GUEST_CLANG}")
   message(FATAL_ERROR "Set FORGE_GUEST_CLANG explicitly; the probe does not download a toolchain")
endif()
if(NOT FORGE_WAMRC OR NOT EXISTS "${FORGE_WAMRC}")
   message(FATAL_ERROR "Set FORGE_WAMRC; silently skipping AOT qualification is forbidden")
endif()
if(NOT FORGE_WAMR_COMPILER_PROBE OR NOT EXISTS "${FORGE_WAMR_COMPILER_PROBE}")
   message(FATAL_ERROR "Set FORGE_WAMR_COMPILER_PROBE; library-produced AOT must also be qualified")
endif()
foreach(_compiler IN ITEMS "${FORGE_WAMRC}" "${FORGE_WAMR_COMPILER_PROBE}")
   execute_process(COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${_compiler}"
      "-DSOURCE_IDENTITY=${_wamr_identity}"
      -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
      COMMAND_ERROR_IS_FATAL ANY)
endforeach()

string(TOLOWER "${CMAKE_SYSTEM_NAME}" WAMR_BUILD_PLATFORM)
if(APPLE)
   add_compile_definitions(BH_PLATFORM_DARWIN)
endif()
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
   set(WAMR_BUILD_TARGET AARCH64)
   set(_aot_target aarch64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
   set(WAMR_BUILD_TARGET X86_64)
   set(_aot_target x86_64)
else()
   message(FATAL_ERROR "Unqualified probe architecture: ${CMAKE_SYSTEM_PROCESSOR}")
endif()

# Exercise the same shared feature profile, with the explicitly selected
# interpreter variant. This is qualification, not production acceptance.
if(FORGE_WAMR_FAST_INTERP)
   set(WAMR_BUILD_FAST_INTERP 1)
else()
   set(WAMR_BUILD_FAST_INTERP 0)
endif()
include("${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake")
forge_wamr_apply_profile("${CMAKE_CURRENT_SOURCE_DIR}" runtime)
add_library(wamr_probe_runtime STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_compile_options(wamr_probe_runtime PRIVATE -fno-fast-math -ffp-contract=off -frounding-math)
target_link_libraries(wamr_probe_runtime PUBLIC Threads::Threads ${CMAKE_DL_LIBS} m)

set(_interpret "${FORGE_SOURCE_ROOT}/libraries/vm/wasm/interpret")
add_library(forge_wamr_private_probe STATIC
   "${_interpret}/engine_runtime.cpp" "${_interpret}/engine_module.cpp"
   "${_interpret}/engine_module_jit.cpp" "${_interpret}/engine_load_budget.cpp"
   "${_interpret}/engine_memory.cpp"
   "${_interpret}/engine_bindings.cpp"
   "${_interpret}/engine_exception.cpp"
   "${_interpret}/engine_limits.cpp"
   "${_interpret}/engine_instance.cpp" "${_interpret}/engine_thread_scope.cpp")
target_compile_features(forge_wamr_private_probe PRIVATE cxx_std_23)
target_link_libraries(forge_wamr_private_probe PRIVATE wamr_probe_runtime)

add_executable(wamr_probe probe.cpp)
target_compile_features(wamr_probe PRIVATE cxx_std_23)
target_include_directories(wamr_probe PRIVATE "${_interpret}")
target_link_libraries(wamr_probe PRIVATE forge_wamr_private_probe wamr_probe_runtime)

set(_wasm "${CMAKE_CURRENT_BINARY_DIR}/fixture.wasm")
set(_object "${CMAKE_CURRENT_BINARY_DIR}/fixture.o")
set(_aot "${CMAKE_CURRENT_BINARY_DIR}/fixture.aot")
get_filename_component(_clang_bin "${FORGE_GUEST_CLANG}" DIRECTORY)
if(FORGE_GUEST_WASM_LD)
   if(NOT EXISTS "${FORGE_GUEST_WASM_LD}")
      message(FATAL_ERROR "FORGE_GUEST_WASM_LD does not exist")
   endif()
   set(_wasm_ld "${FORGE_GUEST_WASM_LD}")
else()
   find_program(_wasm_ld NAMES wasm-ld HINTS "${_clang_bin}" NO_DEFAULT_PATH REQUIRED)
endif()
add_custom_command(OUTPUT "${_object}"
   COMMAND "${FORGE_GUEST_CLANG}" --target=wasm32 -mcpu=mvp -O2 -ffreestanding
      -c -o "${_object}" "${CMAKE_CURRENT_SOURCE_DIR}/fixture.c"
   DEPENDS fixture.c "${FORGE_GUEST_CLANG}" VERBATIM)
add_custom_command(OUTPUT "${_wasm}"
   COMMAND "${_wasm_ld}" --no-entry --export-all
      --initial-memory=131072 --max-memory=262144
      -o "${_wasm}" "${_object}"
   DEPENDS "${_object}" "${_wasm_ld}" VERBATIM)
add_custom_command(OUTPUT "${_aot}"
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMRC}"
      "-DSOURCE_IDENTITY=${_wamr_identity}"
      -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
   COMMAND "${FORGE_WAMRC}" "--target=${_aot_target}" --cpu=generic
      --bounds-checks=1 --stack-bounds-checks=1 --disable-simd
      --disable-bulk-memory --disable-ref-types --enable-multi-thread
      -o "${_aot}" "${_wasm}"
   DEPENDS "${_wasm}" "${FORGE_WAMRC}" VERBATIM)
add_custom_target(wamr_probe_fixtures ALL DEPENDS "${_wasm}" "${_aot}")
add_dependencies(wamr_probe wamr_probe_fixtures)

add_executable(wamr_start_fixture start_fixture.cpp)
target_compile_features(wamr_start_fixture PRIVATE cxx_std_23)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/start.wasm"
   COMMAND wamr_start_fixture "${CMAKE_CURRENT_BINARY_DIR}/start.wasm"
   DEPENDS wamr_start_fixture VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/start.aot"
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMRC}"
      "-DSOURCE_IDENTITY=${_wamr_identity}"
      -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
   COMMAND "${FORGE_WAMRC}" "--target=${_aot_target}" --cpu=generic
      --bounds-checks=1 --stack-bounds-checks=1 --disable-simd
      --disable-bulk-memory --disable-ref-types --enable-multi-thread
      -o "${CMAKE_CURRENT_BINARY_DIR}/start.aot" "${CMAKE_CURRENT_BINARY_DIR}/start.wasm"
   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/start.wasm" "${FORGE_WAMRC}" VERBATIM)
add_custom_target(wamr_start_fixtures ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/start.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/start.aot")
add_dependencies(wamr_probe wamr_start_fixtures)

foreach(_fixture IN ITEMS fixture start)
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
         "-DSOURCE_IDENTITY=${_wamr_identity}"
         -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
      COMMAND "${FORGE_WAMR_COMPILER_PROBE}"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
endforeach()
add_custom_target(wamr_library_fixtures ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/fixture.library_aot"
   "${CMAKE_CURRENT_BINARY_DIR}/start.library_aot")
add_dependencies(wamr_probe wamr_library_fixtures)

foreach(_mode IN ITEMS wasm aot library_aot)
   add_test(NAME "wamr_${_mode}_initialization"
      COMMAND wamr_probe initialization "${CMAKE_CURRENT_BINARY_DIR}/start.${_mode}")
   set_tests_properties("wamr_${_mode}_initialization" PROPERTIES LABELS "wamr;experimental" TIMEOUT 15
      ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
   foreach(_case IN ITEMS core native_abi memory traps host_pointer isolation cancel numeric rounding
                         fp_boundary subnormal_environment concurrent_cancel corpus
                         call_buffers ownership nested_fp)
      add_test(NAME "wamr_${_mode}_${_case}"
         COMMAND wamr_probe "${_case}" "${CMAKE_CURRENT_BINARY_DIR}/fixture.${_mode}")
      set_tests_properties("wamr_${_mode}_${_case}" PROPERTIES
         LABELS "wamr;experimental" TIMEOUT 15
         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
   endforeach()
endforeach()
add_test(NAME wamr_numeric_same_host_parity
   COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/compare.py"
      "$<TARGET_FILE:wamr_probe>" "${_wasm}" "${_aot}")
set_tests_properties(wamr_numeric_same_host_parity PROPERTIES
   LABELS "wamr;experimental" TIMEOUT 30
   ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
add_test(NAME wamr_numeric_corpus_parity
   COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/compare.py"
      "$<TARGET_FILE:wamr_probe>" "${_wasm}" "${_aot}" corpus)
set_tests_properties(wamr_numeric_corpus_parity PROPERTIES LABELS "wamr;experimental" TIMEOUT 30
   ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
foreach(_case IN ITEMS numeric corpus)
   add_test(NAME "wamr_library_${_case}_parity"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/compare.py"
         "$<TARGET_FILE:wamr_probe>" "${_wasm}" "${CMAKE_CURRENT_BINARY_DIR}/fixture.library_aot" "${_case}")
   set_tests_properties("wamr_library_${_case}_parity" PROPERTIES LABELS "wamr;experimental" TIMEOUT 30
      ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
endforeach()

# Compile the actual public options module; the fixture has no copied policy DTO.
add_library(wamr_public_options STATIC)
target_sources(wamr_public_options PUBLIC FILE_SET CXX_MODULES BASE_DIRS "${_interpret}/include"
   FILES "${_interpret}/include/forge/vm/wasm/interpret/options.cppm"
      "${_interpret}/include/forge/vm/wasm/interpret/capacity_profile.cppm")
target_compile_features(wamr_public_options PUBLIC cxx_std_23)
add_executable(wamr_limits_probe limits_probe.cpp)
target_include_directories(wamr_limits_probe PRIVATE "${_interpret}")
target_link_libraries(wamr_limits_probe PRIVATE wamr_public_options forge_wamr_private_probe wamr_probe_runtime)
add_test(NAME wamr_metadata_limits COMMAND wamr_limits_probe)
# The approved bounded profile deliberately rejects the historical extreme.
# Assert the cause and healthy controls; do not invert an arbitrary process failure.
add_executable(wamr_capacity_probe capacity_probe.cpp)
target_include_directories(wamr_capacity_probe PRIVATE "${_interpret}")
target_link_libraries(wamr_capacity_probe PRIVATE wamr_public_options forge_wamr_private_probe wamr_probe_runtime)
target_compile_features(wamr_capacity_probe PRIVATE cxx_std_23)
add_test(NAME wamr_profile_capacity_rejection COMMAND wamr_capacity_probe)
add_test(NAME wamr_profile_aot_admission
   COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:wamr_capacity_probe>"
      "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
      "-DSOURCE_IDENTITY=${_wamr_identity}" "-DROOT=${FORGE_SOURCE_ROOT}"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/profile-rejection"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/profile_rejection.cmake")
set_tests_properties(wamr_profile_capacity_rejection wamr_profile_aot_admission PROPERTIES
   LABELS "wamr;experimental;profile" TIMEOUT 60
   ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
add_executable(wamr_instruction_limits_probe instruction_limits_probe.cpp)
target_include_directories(wamr_instruction_limits_probe PRIVATE "${_interpret}")
target_link_libraries(wamr_instruction_limits_probe PRIVATE wamr_public_options forge_wamr_private_probe wamr_probe_runtime)
add_test(NAME wamr_instruction_limits COMMAND wamr_instruction_limits_probe)
add_executable(wamr_depth_fixture depth_fixture.cpp)
target_compile_features(wamr_depth_fixture PRIVATE cxx_std_23)
add_executable(wamr_depth_probe depth_probe.cpp)
target_compile_features(wamr_depth_probe PRIVATE cxx_std_23)
target_include_directories(wamr_depth_probe PRIVATE "${_interpret}")
target_link_libraries(wamr_depth_probe PRIVATE forge_wamr_private_probe wamr_probe_runtime)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/depth.wasm" "${CMAKE_CURRENT_BINARY_DIR}/depth_start.wasm"
   COMMAND wamr_depth_fixture "${CMAKE_CURRENT_BINARY_DIR}/depth.wasm" "${CMAKE_CURRENT_BINARY_DIR}/depth_start.wasm"
   DEPENDS wamr_depth_fixture VERBATIM)
foreach(_fixture IN ITEMS depth depth_start)
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
         "-DSOURCE_IDENTITY=${_wamr_identity}"
         -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
      COMMAND "${FORGE_WAMR_COMPILER_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
endforeach()
add_custom_target(wamr_depth_fixtures ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/depth.library_aot"
   "${CMAKE_CURRENT_BINARY_DIR}/depth_start.library_aot")
add_dependencies(wamr_depth_probe wamr_depth_fixtures)
foreach(_mode IN ITEMS wasm library_aot)
   add_test(NAME "wamr_${_mode}_call_depth" COMMAND wamr_depth_probe
      "${CMAKE_CURRENT_BINARY_DIR}/depth.${_mode}" "${CMAKE_CURRENT_BINARY_DIR}/depth_start.${_mode}")
   set_tests_properties("wamr_${_mode}_call_depth" PROPERTIES LABELS "wamr;experimental" TIMEOUT 15
      ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
endforeach()
set_tests_properties(wamr_instruction_limits PROPERTIES LABELS "wamr;experimental" TIMEOUT 15
   ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
set_tests_properties(wamr_metadata_limits PROPERTIES LABELS "wamr;experimental" TIMEOUT 15
   ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
add_test(NAME wamr_compiler_identity
   COMMAND "${CMAKE_COMMAND}" "-DROOT=${FORGE_SOURCE_ROOT}" "-DTEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/identity-test"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/compiler_identity.cmake")

configure_file("${FORGE_SOURCE_ROOT}/guest/cmake/types.h.in"
   "${CMAKE_CURRENT_BINARY_DIR}/generated/forge/contract/types.h" COPYONLY)
add_executable(wamr_bindings_probe bindings_probe.cpp)
target_compile_features(wamr_bindings_probe PRIVATE cxx_std_23)
target_include_directories(wamr_bindings_probe PRIVATE "${_interpret}"
   "${FORGE_SOURCE_ROOT}/guest/libraries/contract/include" "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(wamr_bindings_probe PRIVATE forge_wamr_private_probe wamr_probe_runtime)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bindings.wasm"
   COMMAND "${FORGE_GUEST_CLANG}" --target=wasm32 -mcpu=mvp -O2 -ffreestanding
      -c -o "${CMAKE_CURRENT_BINARY_DIR}/bindings.o" "${CMAKE_CURRENT_SOURCE_DIR}/bindings_fixture.c"
   COMMAND "${_wasm_ld}" --no-entry --export-all --initial-memory=131072 --max-memory=262144
      -o "${CMAKE_CURRENT_BINARY_DIR}/bindings.wasm" "${CMAKE_CURRENT_BINARY_DIR}/bindings.o"
   DEPENDS bindings_fixture.c "${FORGE_GUEST_CLANG}" "${_wasm_ld}" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bindings.library_aot"
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
      "-DSOURCE_IDENTITY=${_wamr_identity}"
      -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
   COMMAND "${FORGE_WAMR_COMPILER_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/bindings.wasm"
      "${CMAKE_CURRENT_BINARY_DIR}/bindings.library_aot"
   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bindings.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
add_custom_target(wamr_bindings_fixtures ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bindings.wasm"
   "${CMAKE_CURRENT_BINARY_DIR}/bindings.library_aot")
add_dependencies(wamr_bindings_probe wamr_bindings_fixtures)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bindings_static.wasm"
   COMMAND "${FORGE_GUEST_CLANG}" --target=wasm32 -mcpu=mvp -O2 -ffreestanding -DFORGE_STATIC_BINDINGS
      -c -o "${CMAKE_CURRENT_BINARY_DIR}/bindings_static.o" "${CMAKE_CURRENT_SOURCE_DIR}/bindings_fixture.c"
   COMMAND "${_wasm_ld}" --no-entry --export-all --initial-memory=131072 --max-memory=262144
      -o "${CMAKE_CURRENT_BINARY_DIR}/bindings_static.wasm" "${CMAKE_CURRENT_BINARY_DIR}/bindings_static.o"
   DEPENDS bindings_fixture.c "${FORGE_GUEST_CLANG}" "${_wasm_ld}" VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/bindings_unbounded.wasm"
   COMMAND "${_wasm_ld}" --no-entry --export-all --initial-memory=131072
      -o "${CMAKE_CURRENT_BINARY_DIR}/bindings_unbounded.wasm" "${CMAKE_CURRENT_BINARY_DIR}/bindings.o"
   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bindings.wasm" "${_wasm_ld}" VERBATIM)
foreach(_fixture IN ITEMS bindings_static bindings_unbounded)
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
         "-DSOURCE_IDENTITY=${_wamr_identity}"
         -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
      COMMAND "${FORGE_WAMR_COMPILER_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
   foreach(_mode IN ITEMS wasm library_aot)
      if(_fixture STREQUAL "bindings_static")
         set(_case static_memory)
      else()
         set(_case memory)
      endif()
      add_test(NAME "wamr_${_fixture}_${_mode}" COMMAND wamr_bindings_probe "${_case}"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.${_mode}")
      set_tests_properties("wamr_${_fixture}_${_mode}" PROPERTIES LABELS "wamr;experimental" TIMEOUT 15
         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
   endforeach()
endforeach()
add_custom_target(wamr_bindings_memory_fixtures ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/bindings_static.library_aot"
   "${CMAKE_CURRENT_BINARY_DIR}/bindings_unbounded.library_aot")
add_dependencies(wamr_bindings_probe wamr_bindings_memory_fixtures)
foreach(_mode IN ITEMS wasm library_aot)
   foreach(_case IN ITEMS admission scalar exceptions memory cancel isolation busy)
      add_test(NAME "wamr_bindings_${_mode}_${_case}" COMMAND wamr_bindings_probe "${_case}"
         "${CMAKE_CURRENT_BINARY_DIR}/bindings.${_mode}")
      set_tests_properties("wamr_bindings_${_mode}_${_case}" PROPERTIES LABELS "wamr;experimental" TIMEOUT 15
         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
   endforeach()
endforeach()
add_executable(wamr_bindings_fixture bindings_fixture.cpp)
target_compile_features(wamr_bindings_fixture PRIVATE cxx_std_23)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sdk.wasm" "${CMAKE_CURRENT_BINARY_DIR}/binding_start.wasm"
   COMMAND wamr_bindings_fixture "${CMAKE_CURRENT_BINARY_DIR}/sdk.wasm" "${CMAKE_CURRENT_BINARY_DIR}/binding_start.wasm"
   DEPENDS wamr_bindings_fixture VERBATIM)
foreach(_fixture IN ITEMS sdk binding_start)
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
         "-DSOURCE_IDENTITY=${_wamr_identity}"
         -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
      COMMAND "${FORGE_WAMR_COMPILER_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
endforeach()

add_executable(wamr_imported_initialization_fixture imported_initialization_fixture.cpp)
target_compile_features(wamr_imported_initialization_fixture PRIVATE cxx_std_23)
add_executable(wamr_imported_initialization_probe imported_initialization_probe.cpp)
target_compile_features(wamr_imported_initialization_probe PRIVATE cxx_std_23)
target_include_directories(wamr_imported_initialization_probe PRIVATE "${_interpret}")
target_link_libraries(wamr_imported_initialization_probe PRIVATE forge_wamr_private_probe wamr_probe_runtime)

set(_imported_initialization_names
   imported_start imported_post imported_order imported_post_result imported_post_parameter)
set(_imported_initialization_wasm)
set(_imported_initialization_aot)
foreach(_fixture IN LISTS _imported_initialization_names)
   list(APPEND _imported_initialization_wasm "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm")
   list(APPEND _imported_initialization_aot "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot")
endforeach()
add_custom_command(OUTPUT ${_imported_initialization_wasm}
   COMMAND wamr_imported_initialization_fixture "${CMAKE_CURRENT_BINARY_DIR}"
   DEPENDS wamr_imported_initialization_fixture VERBATIM)
foreach(_fixture IN LISTS _imported_initialization_names)
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
         "-DSOURCE_IDENTITY=${_wamr_identity}"
         -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
      COMMAND "${FORGE_WAMR_COMPILER_PROBE}" "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
endforeach()
add_custom_target(wamr_imported_initialization_fixtures ALL
   DEPENDS ${_imported_initialization_wasm} ${_imported_initialization_aot})
add_dependencies(wamr_imported_initialization_probe wamr_imported_initialization_fixtures)

foreach(_mode IN ITEMS wasm library_aot)
   foreach(_fixture IN ITEMS start post)
      foreach(_case IN ITEMS success failure cancel failure_cancel precancel depth
                             deferred deferred_failure deferred_cancel deferred_depth)
         add_test(NAME "wamr_imported_${_mode}_${_fixture}_${_case}"
            COMMAND wamr_imported_initialization_probe "${_case}"
               "${CMAKE_CURRENT_BINARY_DIR}/imported_${_fixture}.${_mode}")
         set_tests_properties("wamr_imported_${_mode}_${_fixture}_${_case}" PROPERTIES
            LABELS "wamr;experimental;initialization" TIMEOUT 15
            ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
      endforeach()
   endforeach()
   foreach(_case IN ITEMS order signature_result signature_parameter)
      if(_case STREQUAL "order")
         set(_fixture imported_order)
      elseif(_case STREQUAL "signature_result")
         set(_fixture imported_post_result)
      else()
         set(_fixture imported_post_parameter)
      endif()
      add_test(NAME "wamr_imported_${_mode}_${_case}"
         COMMAND wamr_imported_initialization_probe "${_case}"
            "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.${_mode}")
      set_tests_properties("wamr_imported_${_mode}_${_case}" PROPERTIES
         LABELS "wamr;experimental;initialization" TIMEOUT 15
         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
   endforeach()
endforeach()
add_custom_target(wamr_sdk_bindings_fixtures ALL DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/sdk.library_aot"
   "${CMAKE_CURRENT_BINARY_DIR}/binding_start.library_aot")
add_dependencies(wamr_bindings_probe wamr_sdk_bindings_fixtures)
foreach(_mode IN ITEMS wasm library_aot)
   foreach(_case IN ITEMS sdk start)
      if(_case STREQUAL "sdk")
         set(_fixture sdk)
      else()
         set(_fixture binding_start)
      endif()
      add_test(NAME "wamr_bindings_${_mode}_${_case}" COMMAND wamr_bindings_probe "${_case}"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.${_mode}")
      set_tests_properties("wamr_bindings_${_mode}_${_case}" PROPERTIES LABELS "wamr;experimental" TIMEOUT 15
         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
   endforeach()
endforeach()

add_executable(wamr_exception_info_fixture exception_info_fixture.cpp)
target_compile_features(wamr_exception_info_fixture PRIVATE cxx_std_23)
add_executable(wamr_exception_info_probe exception_info_probe.cpp)
target_compile_features(wamr_exception_info_probe PRIVATE cxx_std_23)
target_include_directories(wamr_exception_info_probe PRIVATE "${_interpret}")
target_link_libraries(wamr_exception_info_probe PRIVATE forge_wamr_private_probe wamr_probe_runtime)
set(_exception_names exception_info exception_info_start_trap exception_info_start_host)
set(_exception_wasm)
set(_exception_aot)
foreach(_fixture IN LISTS _exception_names)
   list(APPEND _exception_wasm "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm")
   list(APPEND _exception_aot "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot")
endforeach()
add_custom_command(OUTPUT ${_exception_wasm}
   COMMAND wamr_exception_info_fixture "${CMAKE_CURRENT_BINARY_DIR}"
   DEPENDS wamr_exception_info_fixture VERBATIM)
foreach(_fixture IN LISTS _exception_names)
   add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
         "-DSOURCE_IDENTITY=${_wamr_identity}"
         -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
      COMMAND "${FORGE_WAMR_COMPILER_PROBE}"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm"
         "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.library_aot"
      DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
endforeach()
add_custom_target(wamr_exception_info_fixtures ALL DEPENDS ${_exception_wasm} ${_exception_aot})
add_dependencies(wamr_exception_info_probe wamr_exception_info_fixtures)
foreach(_mode IN ITEMS wasm library_aot)
   foreach(_case IN ITEMS api guest forwarding priority resources concurrent init_trap init_host)
      set(_fixture exception_info)
      if(_case STREQUAL "init_trap")
         set(_fixture exception_info_start_trap)
      elseif(_case STREQUAL "init_host")
         set(_fixture exception_info_start_host)
      endif()
      add_test(NAME "wamr_exception_${_mode}_${_case}" COMMAND wamr_exception_info_probe
         "${_case}" "${CMAKE_CURRENT_BINARY_DIR}/${_fixture}.${_mode}")
      set_tests_properties("wamr_exception_${_mode}_${_case}" PROPERTIES
         LABELS "wamr;experimental;exceptions" TIMEOUT 20
         ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
   endforeach()
endforeach()

add_executable(wamr_allocation_failure_fixture allocation_failure_fixture.cpp)
target_compile_features(wamr_allocation_failure_fixture PRIVATE cxx_std_23)
add_executable(wamr_allocation_failure_probe allocation_failure_probe.cpp)
target_compile_features(wamr_allocation_failure_probe PRIVATE cxx_std_23)
target_link_libraries(wamr_allocation_failure_probe PRIVATE wamr_probe_runtime)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.wasm"
   COMMAND wamr_allocation_failure_fixture "${CMAKE_CURRENT_BINARY_DIR}"
   DEPENDS wamr_allocation_failure_fixture VERBATIM)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.library_aot"
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${FORGE_WAMR_COMPILER_PROBE}"
      "-DSOURCE_IDENTITY=${_wamr_identity}"
      -P "${FORGE_SOURCE_ROOT}/cmake/ForgeWamrCompilerIdentity.cmake"
   COMMAND "${FORGE_WAMR_COMPILER_PROBE}"
      "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.wasm"
      "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.library_aot"
   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.wasm" "${FORGE_WAMR_COMPILER_PROBE}" VERBATIM)
add_custom_target(wamr_allocation_failure_fixtures ALL
   DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.wasm"
      "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.library_aot")
add_dependencies(wamr_allocation_failure_probe wamr_allocation_failure_fixtures)
foreach(_mode IN ITEMS wasm library_aot)
   add_test(NAME "wamr_allocation_${_mode}_raw17" COMMAND wamr_allocation_failure_probe
      "${CMAKE_CURRENT_BINARY_DIR}/allocation_failure.${_mode}")
   set_tests_properties("wamr_allocation_${_mode}_raw17" PROPERTIES
      LABELS "wamr;experimental;exceptions" TIMEOUT 20
      ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1;ASAN_OPTIONS=halt_on_error=1")
endforeach()
