cmake_minimum_required(VERSION 3.31)

option(FORGE_QUALIFY_PATCHED_LIBOAUTH2 "Build the patched public-API F0 fixture" OFF)
if(FORGE_QUALIFY_PATCHED_LIBOAUTH2 AND CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   project(forge_auth_liboauth2_patched_qualification LANGUAGES C CXX)
   get_filename_component(_forge_root "${CMAKE_CURRENT_SOURCE_DIR}/../../.." ABSOLUTE)
   list(PREPEND CMAKE_MODULE_PATH "${_forge_root}/cmake")
   find_package(Threads REQUIRED)
   find_package(OpenSSL 3 REQUIRED COMPONENTS SSL Crypto)
   include(ForgeVendorCompilePolicy)
   include(ForgeOAuth2)
   enable_testing()
endif()

if(TARGET oauth2-internal)
   find_package(Python3 REQUIRED COMPONENTS Interpreter)
   add_executable(forge_auth_liboauth2_patched_qualification patched_probe.cpp)
   target_compile_features(forge_auth_liboauth2_patched_qualification PRIVATE cxx_std_23)
   target_link_libraries(forge_auth_liboauth2_patched_qualification PRIVATE oauth2-internal)
   add_test(
      NAME test_forge_auth_liboauth2_patched_regression
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/patched_run.py
         --probe $<TARGET_FILE:forge_auth_liboauth2_patched_qualification>
   )
   set_tests_properties(test_forge_auth_liboauth2_patched_regression PROPERTIES
      TIMEOUT 180 LABELS "auth;f0;patched-liboauth2"
   )
   return()
endif()

project(forge_auth_liboauth2_qualification LANGUAGES CXX)

find_package(PkgConfig REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
pkg_check_modules(LIBOAUTH2 REQUIRED IMPORTED_TARGET liboauth2=2.4.0)

add_executable(forge_auth_liboauth2_qualification probe.cpp)
target_compile_features(forge_auth_liboauth2_qualification PRIVATE cxx_std_23)
target_link_libraries(forge_auth_liboauth2_qualification PRIVATE PkgConfig::LIBOAUTH2)

enable_testing()
add_test(
   NAME test_forge_auth_liboauth2_characterization
   COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run.py
      --probe $<TARGET_FILE:forge_auth_liboauth2_qualification>
)
set_tests_properties(test_forge_auth_liboauth2_characterization PROPERTIES TIMEOUT 120)
