cmake_minimum_required(VERSION 3.31)
project(forge_wamr_compiler_qualification LANGUAGES C CXX ASM)
enable_testing()
find_package(Threads REQUIRED)
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../../../libraries/vm/wasm/compiler" compiler)
add_executable(wamr_compiler_probe compiler_probe.cpp)
target_compile_features(wamr_compiler_probe PRIVATE cxx_std_23)
target_include_directories(wamr_compiler_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../../libraries/vm/wasm/compiler")
target_link_libraries(wamr_compiler_probe PRIVATE forge_vm_wasm_compiler)
add_test(NAME wamr_compiler_rejection
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:wamr_compiler_probe>"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/rejection.cmake")
set_tests_properties(wamr_compiler_rejection PROPERTIES TIMEOUT 10)
add_test(NAME wamr_compiler_capacity_profile COMMAND wamr_compiler_probe --capacity-profile)
set_tests_properties(wamr_compiler_capacity_profile PROPERTIES TIMEOUT 30)
add_test(NAME wamr_compiler_blocktype_policy COMMAND wamr_compiler_probe --blocktype-policy)
set_tests_properties(wamr_compiler_blocktype_policy PROPERTIES TIMEOUT 60)
add_test(NAME wamr_compiler_profile_contract
   COMMAND "${CMAKE_COMMAND}"
      "-DPROFILE=${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/ForgeWamrProfile.cmake"
      -S "${CMAKE_CURRENT_SOURCE_DIR}/profile_contract"
      -B "${CMAKE_CURRENT_BINARY_DIR}/profile_contract")
get_target_property(_compiler_definitions forge_vm_wasm_compiler COMPILE_DEFINITIONS)
get_target_property(_compiler_includes forge_vm_wasm_compiler INCLUDE_DIRECTORIES)
add_executable(wamr_compiler_table_layout_probe ../table_layout_probe.cpp)
target_compile_features(wamr_compiler_table_layout_probe PRIVATE cxx_std_23)
target_compile_definitions(wamr_compiler_table_layout_probe PRIVATE ${_compiler_definitions})
target_include_directories(wamr_compiler_table_layout_probe PRIVATE ${_compiler_includes})
target_link_libraries(wamr_compiler_table_layout_probe PRIVATE forge_vm_wasm_compiler)
add_test(NAME wamr_compiler_table_layout COMMAND wamr_compiler_table_layout_probe)
set_tests_properties(wamr_compiler_table_layout PROPERTIES TIMEOUT 10)
add_executable(wamr_cli_malformed_fixture cli/malformed_fixture.c)
add_test(NAME wamr_compiler_cli_recovery
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:wamrc>"
      "-DWASM_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DWASM_LD=${FORGE_GUEST_WASM_LD}"
      "-DMALFORMED_FIXTURE=$<TARGET_FILE:wamr_cli_malformed_fixture>"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_recovery"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cli/recovery.cmake")
set_tests_properties(wamr_compiler_cli_recovery PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/blocktype_cli.cmake" CONTENT [=[
string(RANDOM LENGTH 12 ALPHABET "0123456789abcdef" _run)
set(WORK "${WORK}/run-${_run}")
file(MAKE_DIRECTORY "${WORK}")
message(STATUS "Scoped standard-blocktype CLI evidence: ${WORK}")
execute_process(COMMAND "${PROBE}" --write-blocktype-fixtures "${WORK}"
   WORKING_DIRECTORY "${WORK}" COMMAND_ERROR_IS_FATAL ANY)
cmake_host_system_information(RESULT _host QUERY OS_PLATFORM)
if(_host MATCHES "^(arm64|aarch64)$")
   set(_target aarch64)
elseif(_host MATCHES "^(x86_64|amd64|AMD64)$")
   set(_target x86_64)
else()
   message(FATAL_ERROR "No qualified CLI target for ${_host}")
endif()
foreach(_opcode IN ITEMS block loop if)
   foreach(_kind IN ITEMS void zero overlong-zero nonvoid-index numeric)
      set(_fixture "${WORK}/${_opcode}-${_kind}.wasm")
      set(_output "${WORK}/${_opcode}-${_kind}.aot")
      file(SHA256 "${_fixture}" _before)
      execute_process(COMMAND "${COMPILER}" "--target=${_target}" --cpu=generic
         --bounds-checks=1 --stack-bounds-checks=1 --disable-simd --disable-bulk-memory
         --disable-ref-types --enable-multi-thread -o "${_output}" "${_fixture}"
         WORKING_DIRECTORY "${WORK}" RESULT_VARIABLE _result OUTPUT_VARIABLE _out ERROR_VARIABLE _err TIMEOUT 15)
      file(WRITE "${WORK}/${_opcode}-${_kind}.log" "exit=${_result}\n${_out}${_err}")
      if(NOT "${_result}" STREQUAL "0" OR NOT EXISTS "${_output}")
         message(FATAL_ERROR "Generic CLI standard blocktype ${_opcode}/${_kind} failed (${_result}): ${_out}${_err}")
      endif()
      file(READ "${_output}" _magic LIMIT 8 HEX)
      file(SHA256 "${_fixture}" _after)
      if(NOT _magic STREQUAL "00616f7406000000" OR NOT _before STREQUAL _after)
         message(FATAL_ERROR "Generic CLI output/input ownership failed for ${_opcode}/${_kind}")
      endif()
   endforeach()
endforeach()
message(STATUS "PASS generic CLI standard typed blocktypes, 15 actual compilations; separate from Forge strict policy")
]=])
add_test(NAME wamr_compiler_cli_standard_blocktypes
   COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:wamr_compiler_probe>"
      "-DCOMPILER=$<TARGET_FILE:wamrc>" "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/blocktype_cli"
      -P "${CMAKE_CURRENT_BINARY_DIR}/blocktype_cli.cmake")
set_tests_properties(wamr_compiler_cli_standard_blocktypes PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
get_target_property(_identity forge_vm_wasm_compiler FORGE_WAMR_SOURCE_IDENTITY)
add_custom_target(qualification_compiler ALL
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:wamr_compiler_probe>"
      "-DSOURCE_IDENTITY=${_identity}" -DWRITE_IDENTITY=ON
      -P "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake/ForgeWamrCompilerIdentity.cmake"
   DEPENDS wamr_compiler_probe forge_wamrc_identity wamr_cli_malformed_fixture VERBATIM)
