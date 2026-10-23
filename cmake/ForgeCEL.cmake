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
file(READ "${CMAKE_CURRENT_LIST_DIR}/cel/sources.json" _forge_cel_source_pins)
string(JSON _forge_cel_llvm_version GET "${_forge_cel_source_pins}" llvm version)
if(NOT CMAKE_CXX_COMPILER_VERSION VERSION_EQUAL _forge_cel_llvm_version)
   message(FATAL_ERROR "Expression worker requires the qualified LLVM ${_forge_cel_llvm_version} static runtime")
endif()
set(FORGE_CEL_BAZEL_EXECUTABLE "" CACHE FILEPATH "Optional path to the SHA-256 pinned Bazel 8.7.0 Darwin ARM64 binary")
set(FORGE_CEL_REPOSITORY_CACHE "" CACHE PATH "Optional shared Bazel repository download cache for CEL")

set(_forge_cel_root "${PROJECT_BINARY_DIR}/_deps/cel")
set(_forge_cel_output "${_forge_cel_root}/$<CONFIG>")
set(FORGE_CEL_ARCHIVE "${_forge_cel_output}/libforge_expression_backend.a")
set(FORGE_CEL_RUNTIME_OBJECTS
   "${_forge_cel_output}/runtime/cxa_exception_storage.o"
   "${_forge_cel_output}/runtime/emutls.o"
   "${_forge_cel_output}/runtime/atexit.o"
   "${_forge_cel_output}/runtime/new.o")
set(FORGE_CEL_WORKER_MANIFEST "${_forge_cel_output}/worker-manifest.json")
set(FORGE_CEL_WORKER_LINK_MAP "${_forge_cel_output}/worker-link.map")
set_source_files_properties(${FORGE_CEL_RUNTIME_OBJECTS} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)
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
      "${_forge_cel_output}/runtime-manifest.json"
      ${FORGE_CEL_RUNTIME_OBJECTS}
      "${_forge_cel_output}/notices"
   COMMENT "Building the pinned private CEL backend with upstream Bazel targets"
   VERBATIM
)

add_library(ForgeCEL::backend STATIC IMPORTED GLOBAL)
find_library(_forge_cel_corefoundation CoreFoundation REQUIRED)
target_link_libraries(ForgeCEL::backend INTERFACE "${_forge_cel_corefoundation}")
set(_forge_cel_default_archive "${_forge_cel_root}/${CMAKE_BUILD_TYPE}/libforge_expression_backend.a")
set_target_properties(ForgeCEL::backend PROPERTIES IMPORTED_LOCATION "${_forge_cel_default_archive}")
set(_forge_cel_configurations Debug Release RelWithDebInfo MinSizeRel ${CMAKE_CONFIGURATION_TYPES})
list(REMOVE_DUPLICATES _forge_cel_configurations)
foreach(_forge_cel_configuration IN LISTS _forge_cel_configurations)
   string(TOUPPER "${_forge_cel_configuration}" _forge_cel_configuration_upper)
   set(_forge_cel_configuration_archive "${_forge_cel_root}/${_forge_cel_configuration}/libforge_expression_backend.a")
   set_property(TARGET ForgeCEL::backend PROPERTY "IMPORTED_LOCATION_${_forge_cel_configuration_upper}"
      "${_forge_cel_configuration_archive}")
endforeach()
add_dependencies(ForgeCEL::backend forge_expression_backend_build)

# Consumers rebuild the public Forge modules, never the private Bazel backend.
# The internal target carries no CEL/protobuf/Abseil include directories.
set(FORGE_CEL_WORKER_PROPERTIES "")
if(CMAKE_CONFIGURATION_TYPES)
   list(GET CMAKE_CONFIGURATION_TYPES 0 _forge_cel_default_configuration)
   foreach(_forge_cel_configuration IN LISTS CMAKE_CONFIGURATION_TYPES)
      string(APPEND FORGE_CEL_WORKER_PROPERTIES
         "   include(\"\${CMAKE_CURRENT_LIST_DIR}/ForgeCELWorker-${_forge_cel_configuration}.cmake\")\n")
   endforeach()
else()
   set(_forge_cel_default_configuration "${CMAKE_BUILD_TYPE}")
   string(APPEND FORGE_CEL_WORKER_PROPERTIES
      "   include(\"\${CMAKE_CURRENT_LIST_DIR}/ForgeCELWorker-${CMAKE_BUILD_TYPE}.cmake\")\n")
endif()
# The target is created later by libraries/expression. Generation resolves its
# actual location, including custom output directories and multi-config layouts.
set(_forge_cel_import_configuration "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,NOCONFIG>")
file(GENERATE OUTPUT "${PROJECT_BINARY_DIR}/ForgeCELWorker-$<CONFIG>.cmake"
   CONTENT "set_property(TARGET ForgeCEL::worker APPEND PROPERTY IMPORTED_CONFIGURATIONS \"${_forge_cel_import_configuration}\")\nset_property(TARGET ForgeCEL::worker PROPERTY IMPORTED_LOCATION_$<UPPER_CASE:${_forge_cel_import_configuration}> \"$<TARGET_FILE:forge_expression_worker>\")\nif(\"$<CONFIG>\" STREQUAL \"${_forge_cel_default_configuration}\")\n   set_property(TARGET ForgeCEL::worker PROPERTY IMPORTED_LOCATION \"$<TARGET_FILE:forge_expression_worker>\")\nendif()\n")
configure_file("${CMAKE_CURRENT_LIST_DIR}/ForgeCELTargets.cmake.in"
   "${PROJECT_BINARY_DIR}/ForgeCELTargets.cmake" @ONLY)
# Absolute GNUInstallDirs destinations ignore --prefix and cannot follow a
# relocated package. Expression requires both locations inside that prefix.
foreach(_forge_cel_install_directory CMAKE_INSTALL_LIBDIR CMAKE_INSTALL_BINDIR)
   if(IS_ABSOLUTE "${${_forge_cel_install_directory}}")
      message(FATAL_ERROR "Expression requires prefix-relative ${_forge_cel_install_directory}; absolute install destinations cannot relocate with the worker package")
   endif()
   cmake_path(SET _forge_cel_normal_directory NORMALIZE "${${_forge_cel_install_directory}}")
   if(_forge_cel_normal_directory MATCHES "^\\.\\.(/|$)")
      message(FATAL_ERROR "Expression requires ${_forge_cel_install_directory} to remain inside the installation prefix")
   endif()
endforeach()
file(RELATIVE_PATH _forge_cel_package_to_bin
   "/__forge_prefix__/${CMAKE_INSTALL_LIBDIR}/cmake/Forge"
   "/__forge_prefix__/${CMAKE_INSTALL_BINDIR}")
set(FORGE_CEL_WORKER_PROPERTIES
   "   set_property(TARGET ForgeCEL::worker PROPERTY IMPORTED_LOCATION \"\${CMAKE_CURRENT_LIST_DIR}/${_forge_cel_package_to_bin}/forge-expression-worker\")\n")
configure_file("${CMAKE_CURRENT_LIST_DIR}/ForgeCELTargets.cmake.in"
   "${_forge_cel_root}/ForgeCELTargets-install.cmake" @ONLY)
install(FILES "${_forge_cel_root}/ForgeCELTargets-install.cmake"
   DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Forge" RENAME ForgeCELTargets.cmake COMPONENT dev EXCLUDE_FROM_ALL)
install(FILES "${_forge_cel_output}/build-manifest.json"
   "${_forge_cel_output}/runtime-manifest.json"
   "${FORGE_CEL_WORKER_MANIFEST}"
   "${CMAKE_CURRENT_LIST_DIR}/cel/sources.json"
   "${CMAKE_CURRENT_LIST_DIR}/cel/BUILD.bazel"
   "${_forge_cel_output}/source/MODULE.bazel"
   "${_forge_cel_output}/source/MODULE.bazel.lock"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/CEL" COMPONENT dev EXCLUDE_FROM_ALL)
install(DIRECTORY "${_forge_cel_output}/notices/"
   DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/Forge/CEL" COMPONENT dev EXCLUDE_FROM_ALL)
