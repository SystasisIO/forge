cmake_minimum_required(VERSION 3.22)
project(cjose_original_tests LANGUAGES C)
find_package(PkgConfig REQUIRED)
pkg_check_modules(CHECK REQUIRED IMPORTED_TARGET check)
list(APPEND CMAKE_MODULE_PATH "${CANDIDATE_SOURCE}/cmake")
find_package(OpenSSL REQUIRED)
find_package(Jansson REQUIRED)
set(CJOSE_BUILD_TESTS OFF CACHE BOOL "Original suite is owned by this fixture" FORCE)
add_subdirectory("${CANDIDATE_SOURCE}" backend)
set(original_sources check_cjose.c check_version.c check_util.c check_base64.c
   check_concatkdf.c check_jwk.c check_jwe.c check_jws.c check_header.c)
list(TRANSFORM original_sources PREPEND "${ORIGINAL_SOURCE}/test/")
add_executable(check_original ${original_sources} $<TARGET_OBJECTS:cjose_objects>)
# These private includes belong exclusively to UNMODIFIED upstream tests.
# Public qualification probes never see them.
target_include_directories(check_original PRIVATE "${CANDIDATE_SOURCE}/include"
   "${CANDIDATE_SOURCE}/src" "${CMAKE_CURRENT_BINARY_DIR}/backend/generated-include"
   "${CMAKE_CURRENT_BINARY_DIR}/backend/generated-include/cjose")
target_link_libraries(check_original PRIVATE PkgConfig::CHECK OpenSSL::Crypto Jansson::Jansson)
target_compile_definitions(check_original PRIVATE OPENSSL_API_COMPAT=0x10100000L VERSION="0.8.1")
if(UNIX)
   find_package(Threads REQUIRED)
   target_link_libraries(check_original PRIVATE Threads::Threads m)
endif()
set_target_properties(check_original PROPERTIES C_STANDARD 99 C_EXTENSIONS ON)
