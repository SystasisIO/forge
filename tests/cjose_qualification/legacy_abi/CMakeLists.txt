cmake_minimum_required(VERSION 3.22)
project(cjose_legacy_abi LANGUAGES C)
find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)

find_path(LEGACY_INCLUDE_DIR cjose/cjose.h PATHS "${HISTORICAL_PREFIX}/include" NO_DEFAULT_PATH REQUIRED)
find_library(LEGACY_LIBRARY NAMES cjose PATHS "${HISTORICAL_PREFIX}/lib" NO_DEFAULT_PATH REQUIRED)
if(LEGACY_LIBRARY MATCHES "\\.a$")
   message(FATAL_ERROR "Historical shared library is required for binary ABI qualification")
endif()
add_library(legacy_cjose SHARED IMPORTED)
set_target_properties(legacy_cjose PROPERTIES IMPORTED_LOCATION "${LEGACY_LIBRARY}")
add_executable(cjose_legacy_consumer ../legacyconsumer.c)
target_include_directories(cjose_legacy_consumer PRIVATE "${LEGACY_INCLUDE_DIR}"
   "$<TARGET_PROPERTY:OpenSSL::Crypto,INTERFACE_INCLUDE_DIRECTORIES>")
# Old jwk.h includes openssl/obj_mac.h. Consume the selected target's public
# include requirements without adding a second direct crypto library to the ABI probe.
target_link_libraries(cjose_legacy_consumer PRIVATE legacy_cjose ${CMAKE_DL_LIBS})
set_target_properties(cjose_legacy_consumer PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
# Avoid ELF copy relocations / PLT addresses: dladdr must see library-owned
# data and function symbols, not executable trampolines or copied globals.
target_compile_options(cjose_legacy_consumer PRIVATE -fPIC -Wall -Wextra -Werror)
