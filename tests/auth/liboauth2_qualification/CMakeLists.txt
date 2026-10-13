cmake_minimum_required(VERSION 3.31)

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
