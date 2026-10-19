cmake_minimum_required(VERSION 3.22)
project(cjose_public_qualification LANGUAGES C)

set(CJOSE_QUALIFICATION_LINKAGE shared CACHE STRING "Installed component: shared or static")
set_property(CACHE CJOSE_QUALIFICATION_LINKAGE PROPERTY STRINGS shared static)
option(CJOSE_QUALIFICATION_SANITIZERS "Instrument the public fixture" OFF)
option(CJOSE_QUALIFICATION_FUZZ "Build the bounded libFuzzer target" OFF)
if(NOT CJOSE_QUALIFICATION_LINKAGE MATCHES "^(shared|static)$")
   message(FATAL_ERROR "Invalid installed component")
endif()

# Never add_subdirectory(vendor/cjose): this is a genuine installed consumer.
find_package(cjose CONFIG REQUIRED COMPONENTS ${CJOSE_QUALIFICATION_LINKAGE})
find_package(PkgConfig REQUIRED)
pkg_check_modules(JANSSON REQUIRED IMPORTED_TARGET jansson=2.14.1)
find_package(Threads REQUIRED)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

add_executable(cjose_public_probe probe.c profile.c)
target_compile_features(cjose_public_probe PRIVATE c_std_11)
target_link_libraries(cjose_public_probe PRIVATE
   cjose::cjose_${CJOSE_QUALIFICATION_LINKAGE} PkgConfig::JANSSON Threads::Threads)
target_compile_options(cjose_public_probe PRIVATE -Wall -Wextra -Werror)
if(CJOSE_QUALIFICATION_SANITIZERS)
   target_compile_options(cjose_public_probe PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
   target_link_options(cjose_public_probe PRIVATE -fsanitize=address,undefined)
endif()
if(CJOSE_QUALIFICATION_FUZZ)
   add_executable(cjose_profile_fuzz fuzz.c profile.c)
   target_compile_features(cjose_profile_fuzz PRIVATE c_std_11)
   target_link_libraries(cjose_profile_fuzz PRIVATE
      cjose::cjose_${CJOSE_QUALIFICATION_LINKAGE} PkgConfig::JANSSON)
   target_compile_options(cjose_profile_fuzz PRIVATE
      -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
   target_link_options(cjose_profile_fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
