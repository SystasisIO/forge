cmake_minimum_required(VERSION 3.31)

if(NOT DEFINED CMAKE_CXX_COMPILER)
   set(CMAKE_CXX_COMPILER "/opt/homebrew/opt/llvm/bin/clang++" CACHE FILEPATH "Native qualification compiler")
endif()
if(NOT DEFINED CMAKE_OSX_SYSROOT)
   set(CMAKE_OSX_SYSROOT "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk" CACHE PATH "macOS SDK")
endif()

project(forge_expression_native_comparison LANGUAGES CXX)

if(NOT APPLE OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
   message(FATAL_ERROR "This research harness supports macOS with Homebrew LLVM only")
endif()

if(NOT CMAKE_BUILD_TYPE)
   set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(EXPRESSION_NATIVE_EXPRTK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../vendor/exprtk"
   CACHE PATH "Exact pinned ExprTk header directory")
set(EXPRESSION_NATIVE_GENERATED_INCLUDE_DIR ""
   CACHE PATH "Directory containing generated cases.inc")

if(NOT EXISTS "${EXPRESSION_NATIVE_EXPRTK_DIR}/exprtk.hpp")
   message(FATAL_ERROR "Provide the pinned ExprTk header through EXPRESSION_NATIVE_EXPRTK_DIR")
endif()
file(SHA256 "${EXPRESSION_NATIVE_EXPRTK_DIR}/exprtk.hpp" expression_native_exprtk_sha256)
if(NOT expression_native_exprtk_sha256 STREQUAL "a056126bb009b5676a0aade83f86035d384ea8fd4392037380e4774353871218")
   message(FATAL_ERROR "ExprTk header does not match pin 1e4a80b5ec9b4832ed59c6faa65f625a01b18ef0")
endif()
if(NOT EXISTS "${EXPRESSION_NATIVE_GENERATED_INCLUDE_DIR}/cases.inc")
   message(FATAL_ERROR "Provide generated cases.inc through EXPRESSION_NATIVE_GENERATED_INCLUDE_DIR")
endif()

add_library(expression_native_exprtk_profile INTERFACE)
target_include_directories(expression_native_exprtk_profile INTERFACE "${EXPRESSION_NATIVE_EXPRTK_DIR}")
target_compile_definitions(expression_native_exprtk_profile INTERFACE
   exprtk_disable_rtl_io
   exprtk_disable_rtl_io_file
   exprtk_disable_rtl_vecops
   exprtk_disable_string_capabilities
   exprtk_disable_return_statement
   exprtk_disable_break_continue
   exprtk_enable_range_runtime_checks)
target_compile_options(expression_native_exprtk_profile INTERFACE -g0 -O2 -fno-fast-math -ffp-contract=off)

add_executable(expression_native_exprtk exprtk.cpp)
target_include_directories(expression_native_exprtk PRIVATE "${EXPRESSION_NATIVE_GENERATED_INCLUDE_DIR}")
target_link_libraries(expression_native_exprtk PRIVATE expression_native_exprtk_profile)

add_executable(expression_native_exprtk_lazy_probe exprtk_lazy_probe.cpp)
target_link_libraries(expression_native_exprtk_lazy_probe PRIVATE expression_native_exprtk_profile)

# This target is intentionally absent from the default build. Its real compiler
# result and stderr are retained by the qualification runner as evidence.
add_executable(expression_native_exprtk_int64_probe EXCLUDE_FROM_ALL exprtk_int64_probe.cpp)
target_link_libraries(expression_native_exprtk_int64_probe PRIVATE expression_native_exprtk_profile)
