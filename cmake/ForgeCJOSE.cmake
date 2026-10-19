include_guard(GLOBAL)

get_filename_component(_forge_cjose_source "${CMAKE_CURRENT_LIST_DIR}/../vendor/cjose" ABSOLUTE)
if(NOT EXISTS "${_forge_cjose_source}/CMakeLists.txt")
   message(FATAL_ERROR "Initialize the pinned vendor/cjose submodule before enabling a JOSE consumer")
endif()
file(STRINGS "${_forge_cjose_source}/include/cjose/jws.h" _forge_cjose_extension
   REGEX "^#define CJOSE_JWS_EXTERNAL_SIGNING_API 1$")
if(NOT _forge_cjose_extension)
   message(FATAL_ERROR "vendor/cjose must provide the qualified external-signing extension")
endif()

# Share upstream's dependency target with OAuth instead of discovering a second
# Jansson installation through an unrelated pkg-config target.
list(PREPEND CMAKE_MODULE_PATH "${_forge_cjose_source}/cmake")
find_package(Jansson 2.14.1 EXACT REQUIRED)
list(REMOVE_AT CMAKE_MODULE_PATH 0)

function(_forge_add_cjose)
   # Keep these settings local; embedding Forge must not change the parent's
   # default installation component or third-party build options.
   set(CMAKE_INSTALL_DEFAULT_COMPONENT_NAME dev)
   set(CJOSE_BUILD_SHARED OFF)
   set(CJOSE_BUILD_STATIC ON)
   set(CJOSE_BUILD_TESTS OFF)
   set(CJOSE_ENABLE_RSA1_5 OFF)
   add_subdirectory("${_forge_cjose_source}" "${PROJECT_BINARY_DIR}/vendor/cjose")
endfunction()

_forge_add_cjose()
forge_apply_vendored_implementation_policy(cjose_objects)
export(EXPORT cjoseStaticTargets
   FILE "${PROJECT_BINARY_DIR}/vendor/cjose/cjoseStaticTargets.cmake"
   NAMESPACE cjose::)
configure_file("${_forge_cjose_source}/cmake/FindJansson.cmake"
   "${PROJECT_BINARY_DIR}/vendor/cjose/FindJansson.cmake" COPYONLY)
set(_forge_cjose_source_ready TRUE)
