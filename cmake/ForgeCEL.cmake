include_guard(GLOBAL)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR
   NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$" OR
   NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
   message(FATAL_ERROR "FORGE_ENABLE_EXPRESSION currently requires a native macOS ARM64 build")
endif()
if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" OR
   NOT CMAKE_C_COMPILER_VERSION VERSION_EQUAL CMAKE_CXX_COMPILER_VERSION OR
   NOT FORGE_ENABLE_MODULES)
   message(FATAL_ERROR "FORGE_ENABLE_EXPRESSION requires a matching LLVM Clang C/C++ driver pair and FORGE_ENABLE_MODULES=ON")
endif()
file(REAL_PATH "${CMAKE_C_COMPILER}" _forge_cel_c_driver)
file(REAL_PATH "${CMAKE_CXX_COMPILER}" _forge_cel_cxx_driver)
if(NOT _forge_cel_c_driver STREQUAL _forge_cel_cxx_driver)
   message(FATAL_ERROR "FORGE_ENABLE_EXPRESSION requires clang and clang++ from the same LLVM driver binary")
endif()
if(NOT IS_ABSOLUTE "${CMAKE_OSX_SYSROOT}" OR NOT IS_DIRECTORY "${CMAKE_OSX_SYSROOT}")
   message(FATAL_ERROR "FORGE_ENABLE_EXPRESSION requires an explicit existing CMAKE_OSX_SYSROOT directory")
endif()
if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
   message(FATAL_ERROR "Forge expression currently supports only the arm64 architecture")
endif()

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
set(FORGE_CEL_BAZEL_EXECUTABLE "" CACHE FILEPATH "Optional path to the SHA-256 pinned Bazel 8.7.0 Darwin ARM64 binary")
set(FORGE_CEL_REPOSITORY_CACHE "" CACHE PATH "Optional shared Bazel repository download cache for CEL")

set(_forge_cel_root "${PROJECT_BINARY_DIR}/_deps/cel")
set(_forge_cel_output "${_forge_cel_root}/$<CONFIG>")
set(FORGE_CEL_ARCHIVE "${_forge_cel_output}/libforge_expression_backend.a")
set(_forge_cel_deployment "${CMAKE_OSX_DEPLOYMENT_TARGET}")
if(NOT _forge_cel_deployment)
   set(_forge_cel_deployment "14.0")
endif()

add_custom_target(forge_expression_backend_build
   COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/cel/build.py"
      --source-root "${PROJECT_SOURCE_DIR}"
      --build-root "${_forge_cel_output}"
      --compiler "${CMAKE_CXX_COMPILER}"
      --c-compiler "${CMAKE_C_COMPILER}"
      --sdk "${CMAKE_OSX_SYSROOT}"
      --deployment-target "${_forge_cel_deployment}"
      --configuration "$<CONFIG>"
      --bazel "${FORGE_CEL_BAZEL_EXECUTABLE}"
      --repository-cache "${FORGE_CEL_REPOSITORY_CACHE}"
      --cxx-flags "${CMAKE_CXX_FLAGS} $<$<CONFIG:Debug>:${CMAKE_CXX_FLAGS_DEBUG}>$<$<CONFIG:Release>:${CMAKE_CXX_FLAGS_RELEASE}>$<$<CONFIG:RelWithDebInfo>:${CMAKE_CXX_FLAGS_RELWITHDEBINFO}>$<$<CONFIG:MinSizeRel>:${CMAKE_CXX_FLAGS_MINSIZEREL}>"
   BYPRODUCTS "${FORGE_CEL_ARCHIVE}"
      "${_forge_cel_output}/build-manifest.json"
      "${_forge_cel_output}/notices"
   COMMENT "Building the pinned private CEL backend with upstream Bazel targets"
   VERBATIM
)

add_library(ForgeCEL::backend STATIC IMPORTED GLOBAL)
find_library(_forge_cel_corefoundation CoreFoundation REQUIRED)
target_link_libraries(ForgeCEL::backend INTERFACE "${_forge_cel_corefoundation}")
set(_forge_cel_default_archive "${_forge_cel_root}/${CMAKE_BUILD_TYPE}/libforge_expression_backend.a")
set_target_properties(ForgeCEL::backend PROPERTIES IMPORTED_LOCATION "${_forge_cel_default_archive}")
set(_forge_cel_configuration_properties "")
set(_forge_cel_configurations Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES})
list(REMOVE_DUPLICATES _forge_cel_configurations)
foreach(_forge_cel_configuration IN LISTS _forge_cel_configurations)
   string(TOUPPER "${_forge_cel_configuration}" _forge_cel_configuration_upper)
   set(_forge_cel_configuration_archive "${_forge_cel_root}/${_forge_cel_configuration}/libforge_expression_backend.a")
   set_property(TARGET ForgeCEL::backend PROPERTY "IMPORTED_LOCATION_${_forge_cel_configuration_upper}"
      "${_forge_cel_configuration_archive}")
   string(APPEND _forge_cel_configuration_properties
      "   set_property(TARGET ForgeCEL::backend PROPERTY IMPORTED_LOCATION_${_forge_cel_configuration_upper} \"${_forge_cel_configuration_archive}\")\n")
endforeach()
add_dependencies(ForgeCEL::backend forge_expression_backend_build)

# Consumers rebuild the public Forge modules, never the private Bazel backend.
# The internal target carries no CEL/protobuf/Abseil include directories.
set(FORGE_CEL_ARCHIVE_LOCATION "${_forge_cel_default_archive}")
set(FORGE_CEL_CONFIGURATION_PROPERTIES "${_forge_cel_configuration_properties}")
configure_file("${CMAKE_CURRENT_LIST_DIR}/ForgeCELTargets.cmake.in"
   "${PROJECT_BINARY_DIR}/ForgeCELTargets.cmake" @ONLY)
set(FORGE_CEL_ARCHIVE_LOCATION "\${CMAKE_CURRENT_LIST_DIR}/../../forge/internal/expression/libforge_expression_backend.a")
set(FORGE_CEL_CONFIGURATION_PROPERTIES "")
configure_file("${CMAKE_CURRENT_LIST_DIR}/ForgeCELTargets.cmake.in"
   "${_forge_cel_root}/ForgeCELTargets-install.cmake" @ONLY)
install(FILES "${FORGE_CEL_ARCHIVE}"
   DESTINATION "${CMAKE_INSTALL_LIBDIR}/forge/internal/expression" COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES "${_forge_cel_root}/ForgeCELTargets-install.cmake"
   DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Forge" RENAME ForgeCELTargets.cmake COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES "${_forge_cel_output}/build-manifest.json"
   "${CMAKE_CURRENT_LIST_DIR}/cel/sources.json"
   "${CMAKE_CURRENT_LIST_DIR}/cel/BUILD.bazel"
   "${_forge_cel_output}/source/MODULE.bazel"
   "${_forge_cel_output}/source/MODULE.bazel.lock"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/CEL" COMPONENT dev EXCLUDE_FROM_ALL)
install(DIRECTORY "${_forge_cel_output}/notices/"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/CEL" COMPONENT dev EXCLUDE_FROM_ALL)
