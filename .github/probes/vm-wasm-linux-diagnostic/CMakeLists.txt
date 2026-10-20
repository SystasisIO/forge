cmake_minimum_required(VERSION 3.25)

if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt")
   message(FATAL_ERROR "HOLD: use a fresh diagnostic binary directory")
endif()
if(NOT IS_ABSOLUTE "${CMAKE_C_COMPILER}")
   message(FATAL_ERROR "HOLD: supply the exact producer C compiler explicitly")
endif()
project(forge_linux_unwind_diagnostic LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux" OR CMAKE_CROSSCOMPILING
      OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
   message(FATAL_ERROR "HOLD: native Linux x86_64 is required")
endif()
if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang"
      OR NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL "22.1.8")
   message(FATAL_ERROR "HOLD: the CI65 producer uses qualified Clang 22.1.8")
endif()
if((DEFINED CMAKE_TRY_COMPILE_TARGET_TYPE
      AND NOT CMAKE_TRY_COMPILE_TARGET_TYPE STREQUAL "EXECUTABLE")
      OR CMAKE_C_COMPILER_LAUNCHER)
   message(FATAL_ERROR "HOLD: checks must link executables, without a compiler launcher")
endif()
if(NOT IS_ABSOLUTE "${FORGE_DIAGNOSTIC_TOOLCHAIN_PREFIX}"
      OR NOT IS_DIRECTORY "${FORGE_DIAGNOSTIC_TOOLCHAIN_PREFIX}")
   message(FATAL_ERROR "HOLD: supply an existing absolute FORGE_DIAGNOSTIC_TOOLCHAIN_PREFIX")
endif()
file(REAL_PATH "${FORGE_DIAGNOSTIC_TOOLCHAIN_PREFIX}" _prefix)
set(_unwind_input "${_prefix}/lib/libunwind.so.1")
if(NOT EXISTS "${_unwind_input}")
   message(FATAL_ERROR "HOLD: the qualified prefix has no lib/libunwind.so.1")
endif()
file(REAL_PATH "${_unwind_input}" _unwind)
cmake_path(IS_PREFIX _prefix "${_unwind}" NORMALIZE _inside_prefix)
if(NOT _inside_prefix)
   message(FATAL_ERROR "HOLD: libunwind resolves outside the qualified prefix")
endif()
file(SHA256 "${_unwind}" _unwind_sha256)
set(_report "${CMAKE_CURRENT_BINARY_DIR}/unwind-probe.txt")
file(WRITE "${_report}"
   "schema=1\ncheck=CheckFunctionExists\nheaders=none; CMake-generated C declarations\n"
   "prefix=${_prefix}\nunwind_input=${_unwind_input}\n"
   "unwind_realpath=${_unwind}\nunwind_sha256=${_unwind_sha256}\n")
foreach(_var CMAKE_VERSION CMAKE_GENERATOR CMAKE_MAKE_PROGRAM
      CMAKE_C_COMPILER CMAKE_C_COMPILER_ARG1 CMAKE_C_COMPILER_ID
      CMAKE_C_COMPILER_VERSION CMAKE_C_COMPILER_TARGET CMAKE_SYSROOT
      CMAKE_C_FLAGS CMAKE_C_FLAGS_RELEASE CMAKE_BUILD_TYPE
      CMAKE_EXE_LINKER_FLAGS CMAKE_EXE_LINKER_FLAGS_RELEASE
      CMAKE_BUILD_RPATH CMAKE_TRY_COMPILE_CONFIGURATION CMAKE_TRY_COMPILE_TARGET_TYPE
      CMAKE_C_IMPLICIT_INCLUDE_DIRECTORIES CMAKE_C_IMPLICIT_LINK_DIRECTORIES
      CMAKE_C_IMPLICIT_LINK_LIBRARIES)
   file(APPEND "${_report}" "${_var}=${${_var}}\n")
endforeach()
foreach(_env CC CFLAGS LDFLAGS CPATH C_INCLUDE_PATH LIBRARY_PATH
      LD_LIBRARY_PATH LD_PRELOAD LD_AUDIT)
   file(APPEND "${_report}" "env.${_env}=[$ENV{${_env}}]\n")
endforeach()

include(CMakePushCheckState)
include(CheckFunctionExists)
foreach(_mode DEFAULT PINNED)
   cmake_push_check_state(RESET)
   if(_mode STREQUAL "PINNED")
      set(CMAKE_REQUIRED_LIBRARIES "${_unwind_input}")
   endif()
   file(APPEND "${_report}" "\n${_mode}.required_libraries=${CMAKE_REQUIRED_LIBRARIES}\n"
      "${_mode}.required_flags=${CMAKE_REQUIRED_FLAGS}\n"
      "${_mode}.required_includes=${CMAKE_REQUIRED_INCLUDES}\n"
      "${_mode}.required_definitions=${CMAKE_REQUIRED_DEFINITIONS}\n"
      "${_mode}.required_link_options=${CMAKE_REQUIRED_LINK_OPTIONS}\n")
   foreach(_symbol __register_frame __deregister_frame __unw_add_dynamic_fde
         _Unwind_Backtrace _Unwind_GetIP)
      set(_result "FORGE_DIAG_${_mode}_${_symbol}")
      if(DEFINED ${_result})
         message(FATAL_ERROR "HOLD: ${_result} was supplied or cached")
      endif()
      check_function_exists("${_symbol}" ${_result})
      set(_bool FALSE)
      if(${_result})
         set(_bool TRUE)
      endif()
      file(APPEND "${_report}" "${_mode}.${_symbol}.variable=${_result}\n"
         "${_mode}.${_symbol}.raw=[${${_result}}]\n${_mode}.${_symbol}.bool=${_bool}\n")
   endforeach()
   cmake_pop_check_state()
endforeach()
message(STATUS "Diagnostic facts: ${_report}; FALSE is a valid recorded result, not a fix")
# No try_run, product target, forced HAVE_, install, or producer modification.
