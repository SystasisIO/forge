cmake_minimum_required(VERSION 3.31)

foreach(
   required
   IN ITEMS
      FORGE_PACKAGE_TEST_INSTALL_PREFIX
      FORGE_PACKAGE_TEST_INSTALL_LIBDIR
      FORGE_PACKAGE_TEST_INCLUDEDIR
      FORGE_PACKAGE_TEST_RELOCATED_PREFIX
      FORGE_PACKAGE_TEST_SOURCE_DIR
      FORGE_PACKAGE_TEST_BINARY_DIR
      FORGE_PACKAGE_TEST_SOURCE_ROOT
      FORGE_PACKAGE_TEST_BUILD_ROOT
      FORGE_PACKAGE_TEST_GENERATOR
      FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE
      FORGE_PACKAGE_TEST_C_COMPILER
      FORGE_PACKAGE_TEST_CXX_COMPILER
)
   if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
      message(FATAL_ERROR "${required} is required")
   endif()
endforeach()

if(NOT IS_ABSOLUTE "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}" OR
   NOT EXISTS "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}" OR
   IS_DIRECTORY "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}" OR
   IS_SYMLINK "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}")
   message(FATAL_ERROR "the owning build's toolchain-only initial cache must be present")
endif()

file(REMOVE_RECURSE "${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}")
file(COPY "${FORGE_PACKAGE_TEST_INSTALL_PREFIX}/" DESTINATION "${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}")

set(
   installed_vm_wasm_root
   "${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}/${FORGE_PACKAGE_TEST_INCLUDEDIR}/forge/vm/wasm"
)
if(NOT IS_DIRECTORY "${installed_vm_wasm_root}/interpret")
   message(FATAL_ERROR "Installed VM interpreter headers are missing: ${installed_vm_wasm_root}/interpret")
endif()
file(GLOB installed_vm_wasm_entries LIST_DIRECTORIES TRUE "${installed_vm_wasm_root}/*")
set(supported_vm_wasm_leaves interpret execution optimizing compiler)
foreach(entry IN LISTS installed_vm_wasm_entries)
   get_filename_component(entry_name "${entry}" NAME)
   if(NOT entry_name IN_LIST supported_vm_wasm_leaves OR NOT IS_DIRECTORY "${entry}")
      message(FATAL_ERROR "Installed package exposes legacy VM public path: ${entry}")
   endif()
endforeach()

set(
   forbidden_details
   "${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}/${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/forge/internal/vm_wasm_interpret/details"
)
if(EXISTS "${forbidden_details}")
   message(FATAL_ERROR "Installed vm_wasm_interpret package exposes private details: ${forbidden_details}")
endif()

set(
   forbidden_softfloat_path
   "${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}/${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/forge/internal/vm_wasm/softfloat"
)
if(EXISTS "${forbidden_softfloat_path}")
   message(FATAL_ERROR "Installed package preserves the legacy SoftFloat path: ${forbidden_softfloat_path}")
endif()

set(
   targets_file
   "${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}/${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/cmake/Forge/ForgeTargets.cmake"
)
file(READ "${targets_file}" targets_content)

set(
   config_file
   "${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}/${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/cmake/Forge/ForgeConfig.cmake"
)
file(READ "${config_file}" config_content)

function(assert_content_omits variable label expression)
   string(REGEX MATCH "${expression}" match "${${variable}}")
   if(NOT "${match}" STREQUAL "")
      message(FATAL_ERROR "${label} preserves legacy identity: ${match}")
   endif()
endfunction()

function(assert_target_has_no_build_paths target)
   set(marker "# Create imported target ${target}")
   string(FIND "${targets_content}" "${marker}" marker_offset)
   if(marker_offset EQUAL -1)
      message(FATAL_ERROR "Installed package does not export ${target}")
   endif()

   string(SUBSTRING "${targets_content}" ${marker_offset} -1 target_tail)
   string(FIND "${target_tail}" "\n# Create imported target " next_target_offset)
   if(next_target_offset EQUAL -1)
      set(target_block "${target_tail}")
   else()
      string(SUBSTRING "${target_tail}" 0 ${next_target_offset} target_block)
   endif()

   foreach(
      forbidden
      IN ITEMS
         "${FORGE_PACKAGE_TEST_SOURCE_ROOT}"
         "${FORGE_PACKAGE_TEST_BUILD_ROOT}"
         "${FORGE_PACKAGE_TEST_INSTALL_PREFIX}"
   )
      string(FIND "${target_block}" "${forbidden}" forbidden_offset)
      if(NOT forbidden_offset EQUAL -1)
         message(FATAL_ERROR "${target} exports build-local path: ${forbidden}")
      endif()
   endforeach()
endfunction()

assert_target_has_no_build_paths("Forge::forge_core")
assert_target_has_no_build_paths("Forge::forge_exceptions")
assert_target_has_no_build_paths("Forge::forge_vm_wasm_interpret")
assert_target_has_no_build_paths("Forge::vm_wasm_interpret_softfloat_internal")
assert_content_omits(targets_content "ForgeTargets" "Forge::forge_vm_wasm([^A-Za-z0-9_]|$)")
assert_content_omits(targets_content "ForgeTargets" "Forge::vm_wasm_softfloat_internal([^A-Za-z0-9_]|$)")
assert_content_omits(targets_content "ForgeTargets" "forge/internal/vm_wasm/softfloat")
assert_content_omits(config_content "ForgeConfig" "(^|[^A-Za-z0-9_])vm_wasm([^A-Za-z0-9_]|$)")

file(REMOVE_RECURSE "${FORGE_PACKAGE_TEST_BINARY_DIR}")

set(
   configure_command
   "${CMAKE_COMMAND}"
   -S "${FORGE_PACKAGE_TEST_SOURCE_DIR}"
   -B "${FORGE_PACKAGE_TEST_BINARY_DIR}"
   -C "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}"
   -G "${FORGE_PACKAGE_TEST_GENERATOR}"
   "-DCMAKE_PREFIX_PATH=${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}"
   "-DForge_DIR=${FORGE_PACKAGE_TEST_RELOCATED_PREFIX}/${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/cmake/Forge"
   "-DFORGE_PACKAGE_TEST_WITH_JIT=${FORGE_PACKAGE_TEST_WITH_JIT}"
   "-DCMAKE_CXX_COMPILER=${FORGE_PACKAGE_TEST_CXX_COMPILER}"
   "-DCMAKE_C_COMPILER=${FORGE_PACKAGE_TEST_C_COMPILER}"
   "-DCMAKE_DISABLE_FIND_PACKAGE_Clang=ON"
)
if(NOT FORGE_PACKAGE_TEST_WITH_JIT)
   list(APPEND configure_command "-DCMAKE_DISABLE_FIND_PACKAGE_LLVM=ON")
elseif(DEFINED FORGE_PACKAGE_TEST_LLVM_DIR AND NOT "${FORGE_PACKAGE_TEST_LLVM_DIR}" STREQUAL "")
   list(APPEND configure_command "-DLLVM_DIR=${FORGE_PACKAGE_TEST_LLVM_DIR}")
endif()
if(DEFINED FORGE_PACKAGE_TEST_OSX_SYSROOT AND NOT "${FORGE_PACKAGE_TEST_OSX_SYSROOT}" STREQUAL "")
   list(APPEND configure_command "-DCMAKE_OSX_SYSROOT=${FORGE_PACKAGE_TEST_OSX_SYSROOT}")
endif()
if(DEFINED FORGE_PACKAGE_TEST_OPENSSL_ROOT_DIR AND NOT "${FORGE_PACKAGE_TEST_OPENSSL_ROOT_DIR}" STREQUAL "")
   list(APPEND configure_command "-DOPENSSL_ROOT_DIR=${FORGE_PACKAGE_TEST_OPENSSL_ROOT_DIR}")
endif()

execute_process(COMMAND ${configure_command} COMMAND_ERROR_IS_FATAL ANY)
execute_process(
   COMMAND "${CMAKE_COMMAND}" --build "${FORGE_PACKAGE_TEST_BINARY_DIR}"
   COMMAND_ERROR_IS_FATAL ANY
)
execute_process(
   COMMAND "${FORGE_PACKAGE_TEST_BINARY_DIR}/forge_package_vm_wasm_interpret_component"
   COMMAND_ERROR_IS_FATAL ANY
)
if(FORGE_PACKAGE_TEST_WITH_JIT)
   execute_process(
      COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${FORGE_PACKAGE_TEST_BINARY_DIR}"
         --output-on-failure --no-tests=error --parallel 1
         --output-log "${FORGE_PACKAGE_TEST_BINARY_DIR}/package-tests.log"
         --output-junit "${FORGE_PACKAGE_TEST_BINARY_DIR}/package-tests.xml"
      COMMAND_ERROR_IS_FATAL ANY
   )
endif()
