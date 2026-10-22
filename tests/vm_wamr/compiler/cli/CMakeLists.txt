cmake_minimum_required(VERSION 3.31)
project(forge_wamr_cli_qualification LANGUAGES C CXX ASM)
enable_testing()
find_package(Git REQUIRED)
if(WAMR_CLI_ARCHIVE)
   if(WAMR_CLI_SOURCE OR NOT WAMR_CLI_ARCHIVE_SHA256 OR NOT WAMR_CLI_BASE_REVISION)
      message(FATAL_ERROR "Archive qualification requires its SHA256 and base revision, without WAMR_CLI_SOURCE")
   endif()
   file(SHA256 "${WAMR_CLI_ARCHIVE}" _archive_digest)
   if(NOT _archive_digest STREQUAL WAMR_CLI_ARCHIVE_SHA256)
      message(FATAL_ERROR "Controlled source archive digest mismatch")
   endif()
   set(WAMR_CLI_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/source-${_archive_digest}")
   if(NOT EXISTS "${WAMR_CLI_SOURCE}/.source-complete")
      file(ARCHIVE_EXTRACT INPUT "${WAMR_CLI_ARCHIVE}" DESTINATION "${WAMR_CLI_SOURCE}")
      file(WRITE "${WAMR_CLI_SOURCE}/.source-complete" "${_archive_digest}\n")
   endif()
   set(_revision "${WAMR_CLI_BASE_REVISION}")
   set(_status "controlled working-tree archive (not an immutable published pin)\narchive_sha256=${_archive_digest}\n")
else()
   execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${WAMR_CLI_SOURCE}" rev-parse HEAD
      OUTPUT_VARIABLE _revision OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
   execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${WAMR_CLI_SOURCE}" status --porcelain
      OUTPUT_VARIABLE _status COMMAND_ERROR_IS_FATAL ANY)
   execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${WAMR_CLI_SOURCE}" diff --binary --no-ext-diff HEAD -- wamr-compiler/main.c
      OUTPUT_FILE "${CMAKE_CURRENT_BINARY_DIR}/cli-main.patch" COMMAND_ERROR_IS_FATAL ANY)
endif()
if(NOT IS_DIRECTORY "${WAMR_CLI_SOURCE}/wamr-compiler")
   message(FATAL_ERROR "WAMR_CLI_SOURCE must explicitly select a controlled WAMR checkout")
endif()
get_filename_component(_forge_root "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
file(SHA256 "${WAMR_CLI_SOURCE}/wamr-compiler/main.c" _main_digest)
file(SHA256 "${_forge_root}/cmake/ForgeWamrProfile.cmake" _profile_digest)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/source-provenance.txt"
   "source=${WAMR_CLI_SOURCE}\nrevision=${_revision}\nmain_sha256=${_main_digest}\nprofile_sha256=${_profile_digest}\nstatus:\n${_status}")

# This test-only project selects source, never patches it or changes Forge's pin.
include("${_forge_root}/cmake/ForgeWamrProfile.cmake")
set(WAMR_BUILD_WITH_CUSTOM_LLVM 1)
find_package(LLVM 22.1.8 EXACT REQUIRED CONFIG)
add_subdirectory("${WAMR_CLI_SOURCE}/wamr-compiler" vendor EXCLUDE_FROM_ALL)
forge_wamr_apply_profile("${WAMR_CLI_SOURCE}/wamr-compiler" compiler)
get_target_property(_runtime_sources vmlib SOURCES)
list(FILTER _runtime_sources EXCLUDE REGEX "/common/gc/")
set_property(TARGET vmlib PROPERTY SOURCES ${_runtime_sources})
if(NOT LLVM_ENABLE_RTTI)
   message(FATAL_ERROR "CLI qualification requires the same RTTI-enabled LLVM as Forge's compiler")
endif()
target_compile_options(aotclib PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-frtti>")
foreach(_target IN ITEMS vmlib aotclib)
   target_compile_options(${_target} PRIVATE -fno-fast-math -ffp-contract=off -frounding-math)
endforeach()
if(WAMR_CLI_BASELINE_MAIN)
   # Diagnostic control: original CLI, same libraries/profile/instrumentation.
   # Never installed or used as the compiler under qualification.
   add_executable(wamrc_callback_baseline EXCLUDE_FROM_ALL "${WAMR_CLI_BASELINE_MAIN}")
   foreach(_property IN ITEMS INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_LIBRARIES POSITION_INDEPENDENT_CODE)
      get_target_property(_value wamrc ${_property})
      if(_value)
         set_property(TARGET wamrc_callback_baseline PROPERTY ${_property} "${_value}")
      endif()
   endforeach()
   get_directory_property(_c_flags DIRECTORY "${WAMR_CLI_SOURCE}/wamr-compiler" DEFINITION CMAKE_C_FLAGS)
   target_compile_options(wamrc_callback_baseline PRIVATE "SHELL:${_c_flags}")
   file(SHA256 "${WAMR_CLI_BASELINE_MAIN}" _baseline_digest)
   file(APPEND "${CMAKE_CURRENT_BINARY_DIR}/source-provenance.txt" "baseline_main_sha256=${_baseline_digest}\n")
endif()
add_executable(wamr_cli_malformed_fixture malformed_fixture.c)
add_custom_target(qualification_cli ALL DEPENDS wamrc wamr_cli_malformed_fixture)
add_test(NAME wamr_compiler_cli_recovery
   COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=$<TARGET_FILE:wamrc>"
      "-DWASM_C_COMPILER=${CMAKE_C_COMPILER}"
      "-DWASM_LD=${FORGE_GUEST_WASM_LD}"
      "-DMALFORMED_FIXTURE=$<TARGET_FILE:wamr_cli_malformed_fixture>"
      "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/cli_recovery"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/recovery.cmake")
set_tests_properties(wamr_compiler_cli_recovery PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
if(CMAKE_EXPORT_COMPILE_COMMANDS)
   add_test(NAME wamr_compiler_cli_profile
      COMMAND "${CMAKE_COMMAND}"
         "-DCLI_COMMANDS=${CMAKE_CURRENT_BINARY_DIR}/compile_commands.json"
         "-DCLI_MAIN=${WAMR_CLI_SOURCE}/wamr-compiler/main.c"
         "-DLIBRARY_COMMANDS=${WAMR_CLI_LIBRARY_COMMANDS}"
         -P "${CMAKE_CURRENT_SOURCE_DIR}/profile.cmake")
   set_tests_properties(wamr_compiler_cli_profile PROPERTIES TIMEOUT 30)
endif()
