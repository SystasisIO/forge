cmake_minimum_required(VERSION 3.31)

project(forge-package-vm-wasm-optimizing-component LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER "Build the separate installed compiler fixture executables" ON)
option(FORGE_PACKAGE_OPTIMIZING_BUILD_SESSION "Build the canonical installed helper/session/cache consumer" ON)
set(FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX "" CACHE FILEPATH "Export-name artifact prefix, required without a producer")
set(FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR "" CACHE PATH "Session artifacts directory, required without a producer")
set(FORGE_PACKAGE_OPTIMIZING_HELPER "" CACHE FILEPATH "Explicit installed forge-wasm-compiler executable")
set(FORGE_WAMR_COMPILER_TEST_AS_BYTES "" CACHE STRING "Explicit finite diagnostic helper address-space bytes")

set(_components vm_wasm_optimizing asio)
if(FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER)
   list(APPEND _components vm_wasm_compiler)
endif()
find_package(Forge CONFIG REQUIRED COMPONENTS ${_components})
enable_testing()

set(_canonical "${CMAKE_CURRENT_SOURCE_DIR}/../vm_wasm_optimizing")
if(NOT FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX)
   if(NOT FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER)
      message(FATAL_ERROR "Runtime-only qualification requires FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX")
   endif()
   set(FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX "${CMAKE_CURRENT_BINARY_DIR}/fixtures/export-names")
endif()
if(FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER)
   get_filename_component(_fixture_parent "${FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX}" DIRECTORY)
   file(MAKE_DIRECTORY "${_fixture_parent}")
   add_executable(forge_package_vm_wasm_optimizing_producer "${_canonical}/export_name_tests.cpp")
   target_compile_definitions(forge_package_vm_wasm_optimizing_producer PRIVATE
      FORGE_WAMR_EXPORT_NAME_COMPILER_FIXTURE=1)
   target_link_libraries(forge_package_vm_wasm_optimizing_producer PRIVATE Forge::forge_vm_wasm_compiler)
   add_test(NAME package_optimizing_export_names_compile
      COMMAND forge_package_vm_wasm_optimizing_producer "${FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX}")
   set_tests_properties(package_optimizing_export_names_compile PROPERTIES
      FIXTURES_SETUP package_optimizing_export_names TIMEOUT 180 RUN_SERIAL TRUE)
else()
   foreach(_label IN ITEMS distinct hook-suffix hook-exact)
      foreach(_extension IN ITEMS foc request)
         if(NOT EXISTS "${FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX}-${_label}.${_extension}")
            message(FATAL_ERROR "Missing real producer fixture: ${FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX}-${_label}.${_extension}")
         endif()
      endforeach()
   endforeach()
endif()

add_executable(forge_package_vm_wasm_optimizing_runtime "${_canonical}/export_name_tests.cpp")
target_link_libraries(forge_package_vm_wasm_optimizing_runtime PRIVATE Forge::forge_vm_wasm_optimizing Forge::forge_asio)
add_test(NAME package_optimizing_export_names_runtime
   COMMAND forge_package_vm_wasm_optimizing_runtime "${FORGE_PACKAGE_OPTIMIZING_FIXTURE_PREFIX}")
set_tests_properties(package_optimizing_export_names_runtime PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
if(FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER)
   set_tests_properties(package_optimizing_export_names_runtime PROPERTIES
      FIXTURES_REQUIRED package_optimizing_export_names)
endif()

if(FORGE_PACKAGE_OPTIMIZING_BUILD_SESSION)
   if(NOT UNIX)
      message(FATAL_ERROR "The canonical session consumer requires UNIX (unistd.h/mkdtemp)")
   endif()
   if(NOT IS_ABSOLUTE "${FORGE_PACKAGE_OPTIMIZING_HELPER}" OR
      NOT EXISTS "${FORGE_PACKAGE_OPTIMIZING_HELPER}" OR IS_DIRECTORY "${FORGE_PACKAGE_OPTIMIZING_HELPER}")
      message(FATAL_ERROR "Session qualification requires an explicit existing installed helper executable")
   endif()
   if(NOT FORGE_WAMR_COMPILER_TEST_AS_BYTES MATCHES "^[1-9][0-9]*$")
      message(FATAL_ERROR "Session qualification requires explicit finite FORGE_WAMR_COMPILER_TEST_AS_BYTES")
   endif()
   if(NOT FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR)
      if(NOT FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER)
         message(FATAL_ERROR "Runtime-only session qualification requires FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR")
      endif()
      set(FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR "${CMAKE_CURRENT_BINARY_DIR}/fixtures/session")
   endif()
   if(FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER)
      file(MAKE_DIRECTORY "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}")
      add_executable(forge_package_vm_wasm_optimizing_session_wasm "${_canonical}/heavy_fixture.cpp")
      add_executable(forge_package_vm_wasm_optimizing_session_producer "${_canonical}/compile_fixture.cpp")
      target_link_libraries(forge_package_vm_wasm_optimizing_session_producer PRIVATE Forge::forge_vm_wasm_compiler)
      add_test(NAME package_optimizing_session_wasm
         COMMAND forge_package_vm_wasm_optimizing_session_wasm "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}")
      set_tests_properties(package_optimizing_session_wasm PROPERTIES
         FIXTURES_SETUP package_optimizing_session_wasm TIMEOUT 30 RUN_SERIAL TRUE)
      add_test(NAME package_optimizing_session_compile COMMAND forge_package_vm_wasm_optimizing_session_producer
         "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}/heavy-8192.wasm"
         "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}/heavy-8192.foc"
         "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}/heavy-8192.request")
      set_tests_properties(package_optimizing_session_compile PROPERTIES
         FIXTURES_REQUIRED package_optimizing_session_wasm FIXTURES_SETUP package_optimizing_session
         TIMEOUT 180 RUN_SERIAL TRUE)
   else()
      foreach(_file IN ITEMS heavy-8192.request heavy-8192.foc multi-result.wasm)
         if(NOT EXISTS "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}/${_file}")
            message(FATAL_ERROR "Missing real producer session fixture: ${_file}")
         endif()
      endforeach()
   endif()
   add_executable(forge_package_vm_wasm_optimizing_session "${_canonical}/session_tests.cpp")
   target_link_libraries(forge_package_vm_wasm_optimizing_session PRIVATE Forge::forge_vm_wasm_optimizing Forge::forge_asio)
   file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/session-cache")
   add_test(NAME package_optimizing_session COMMAND forge_package_vm_wasm_optimizing_session
      "${FORGE_PACKAGE_OPTIMIZING_HELPER}"
      "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}/heavy-8192.request"
      "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}/heavy-8192.foc"
      "${CMAKE_CURRENT_BINARY_DIR}/session-cache"
      "${FORGE_WAMR_COMPILER_TEST_AS_BYTES}"
      "${FORGE_PACKAGE_OPTIMIZING_SESSION_FIXTURE_DIR}/multi-result.wasm")
   set_tests_properties(package_optimizing_session PROPERTIES TIMEOUT 900 RUN_SERIAL TRUE)
   if(FORGE_PACKAGE_OPTIMIZING_BUILD_PRODUCER)
      set_tests_properties(package_optimizing_session PROPERTIES FIXTURES_REQUIRED package_optimizing_session)
   endif()
endif()

foreach(_target IN ITEMS forge_package_vm_wasm_optimizing_producer forge_package_vm_wasm_optimizing_runtime
      forge_package_vm_wasm_optimizing_session_producer forge_package_vm_wasm_optimizing_session)
   if(TARGET ${_target})
      if(APPLE)
         target_link_options(${_target} PRIVATE "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/${_target}.linkmap")
      elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
         target_link_options(${_target} PRIVATE "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/${_target}.linkmap")
      endif()
   endif()
endforeach()
