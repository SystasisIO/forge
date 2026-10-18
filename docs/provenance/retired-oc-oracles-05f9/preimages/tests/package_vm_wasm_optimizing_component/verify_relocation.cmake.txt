cmake_minimum_required(VERSION 3.31)

foreach(required IN ITEMS FORGE_PACKAGE_TEST_INSTALL_PREFIX FORGE_PACKAGE_TEST_INSTALL_LIBDIR
   FORGE_PACKAGE_TEST_BINDIR FORGE_PACKAGE_TEST_SOURCE_DIR FORGE_PACKAGE_TEST_GENERATOR
   FORGE_PACKAGE_TEST_CXX_COMPILER FORGE_PACKAGE_TEST_C_COMPILER FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE
   FORGE_OC_SOURCE_ROOT
   FORGE_OC_BUILD_ROOT FORGE_OC_COMPILER FORGE_OC_AS_BYTES)
   if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
      message(FATAL_ERROR "${required} is required")
   endif()
endforeach()
include("${FORGE_OC_SOURCE_ROOT}/tests/vm_wasm/export_runtime_fixtures.cmake")
forge_oc_absolute(FORGE_PACKAGE_TEST_INSTALL_PREFIX)
forge_oc_absolute(FORGE_PACKAGE_TEST_SOURCE_DIR)
forge_oc_absolute(FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE)
if(NOT EXISTS "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}" OR
   IS_DIRECTORY "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}" OR
   IS_SYMLINK "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}")
   message(FATAL_ERROR "the owning build's toolchain-only initial cache must be present")
endif()
if(NOT IS_DIRECTORY "${FORGE_PACKAGE_TEST_INSTALL_PREFIX}" OR
   NOT IS_DIRECTORY "${FORGE_PACKAGE_TEST_SOURCE_DIR}")
   message(FATAL_ERROR "installed package and existing consumer source must be present")
endif()
foreach(relative IN ITEMS FORGE_PACKAGE_TEST_INSTALL_LIBDIR FORGE_PACKAGE_TEST_BINDIR)
   if(IS_ABSOLUTE "${${relative}}" OR "${${relative}}" MATCHES "(^|/)\\.\\.(/|$)|[;\r\n]")
      message(FATAL_ERROR "installed directory must be a bounded relative path")
   endif()
endforeach()
if(NOT FORGE_OC_AS_BYTES MATCHES "^[0-9]+$" OR FORGE_OC_AS_BYTES STREQUAL "0")
   message(FATAL_ERROR "explicit finite helper AS bytes are mandatory")
endif()
forge_oc_verify_fixtures(fixtures)
forge_oc_native_suffix(suffix)
get_filename_component(generation "${fixtures}" DIRECTORY)
set(run "")
foreach(attempt RANGE 1 8)
   string(RANDOM LENGTH 20 ALPHABET 0123456789abcdef id)
   set(candidate "${generation}/package-${id}")
   if(NOT EXISTS "${candidate}" AND NOT IS_SYMLINK "${candidate}")
      set(run "${candidate}")
      break()
   endif()
endforeach()
if(run STREQUAL "")
   message(FATAL_ERROR "cannot claim a fresh package test output")
endif()
# These three new siblings can never alias input/source/payload paths. No
# remove_recurse or overwrite of a supplied directory is used on any path.
foreach(input IN ITEMS FORGE_PACKAGE_TEST_INSTALL_PREFIX FORGE_PACKAGE_TEST_SOURCE_DIR)
   file(REAL_PATH "${${input}}" normalized)
   cmake_path(IS_PREFIX normalized "${run}" NORMALIZE overlaps)
   cmake_path(IS_PREFIX run "${normalized}" NORMALIZE reverse_overlaps)
   if(overlaps OR reverse_overlaps)
      message(FATAL_ERROR "owned package output overlaps input/source")
   endif()
endforeach()
file(MAKE_DIRECTORY "${run}/prefix" "${run}/build" "${run}/cache")
file(CHMOD "${run}" "${run}/prefix" "${run}/build" "${run}/cache"
   PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
file(COPY "${FORGE_PACKAGE_TEST_INSTALL_PREFIX}/" DESTINATION "${run}/prefix")
set(helper "${run}/prefix/${FORGE_PACKAGE_TEST_BINDIR}/forge-wasm-compiler")
if(NOT EXISTS "${helper}" OR IS_DIRECTORY "${helper}" OR IS_SYMLINK "${helper}")
   message(FATAL_ERROR "dev install must include the actual relocated compiler helper")
endif()
set(targets "${run}/prefix/${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/cmake/Forge/ForgeTargets.cmake")
file(READ "${targets}" content)
foreach(target IN ITEMS Forge::forge_core Forge::forge_vm_wasm_optimizing)
   string(FIND "${content}" "# Create imported target ${target}" begin)
   if(begin LESS 0)
      message(FATAL_ERROR "installed runtime leaf missing: ${target}")
   endif()
   string(SUBSTRING "${content}" ${begin} -1 block)
   string(FIND "${block}" "\n# Create imported target " end)
   if(NOT end LESS 0)
      string(SUBSTRING "${block}" 0 ${end} block)
   endif()
   foreach(forbidden IN ITEMS "${FORGE_OC_SOURCE_ROOT}" "${FORGE_OC_BUILD_ROOT}"
      "${FORGE_PACKAGE_TEST_INSTALL_PREFIX}")
      string(FIND "${block}" "${forbidden}" found)
      if(NOT found LESS 0)
         message(FATAL_ERROR "installed runtime export retains origin path")
      endif()
   endforeach()
endforeach()
set(command "${CMAKE_COMMAND}" -S "${FORGE_PACKAGE_TEST_SOURCE_DIR}" -B "${run}/build"
   -C "${FORGE_PACKAGE_TEST_TOOLCHAIN_CACHE}"
   -G "${FORGE_PACKAGE_TEST_GENERATOR}" "-DCMAKE_PREFIX_PATH=${run}/prefix"
   "-DForge_DIR=${run}/prefix/${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/cmake/Forge"
   "-DCMAKE_CXX_COMPILER=${FORGE_PACKAGE_TEST_CXX_COMPILER}"
   "-DCMAKE_C_COMPILER=${FORGE_PACKAGE_TEST_C_COMPILER}"
   -DCMAKE_DISABLE_FIND_PACKAGE_LLVM=TRUE
   "-DFORGE_OC_CONSUMER_REQUEST=${fixtures}/add-${suffix}.request"
   "-DFORGE_OC_CONSUMER_ARTIFACT=${fixtures}/add-${suffix}.foc"
   "-DFORGE_OC_CONSUMER_HELPER=${helper}"
   "-DFORGE_OC_CONSUMER_CACHE=${run}/cache"
   "-DFORGE_OC_CONSUMER_AS_BYTES=${FORGE_OC_AS_BYTES}")
if(DEFINED FORGE_PACKAGE_TEST_OSX_SYSROOT AND NOT FORGE_PACKAGE_TEST_OSX_SYSROOT STREQUAL "")
   list(APPEND command "-DCMAKE_OSX_SYSROOT=${FORGE_PACKAGE_TEST_OSX_SYSROOT}")
endif()
if(DEFINED FORGE_PACKAGE_TEST_OPENSSL_ROOT_DIR AND NOT "${FORGE_PACKAGE_TEST_OPENSSL_ROOT_DIR}" STREQUAL "")
   list(APPEND command "-DOPENSSL_ROOT_DIR=${FORGE_PACKAGE_TEST_OPENSSL_ROOT_DIR}")
endif()
execute_process(COMMAND ${command} COMMAND_ERROR_IS_FATAL ANY TIMEOUT 120)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${run}/build" -j 4
   COMMAND_ERROR_IS_FATAL ANY TIMEOUT 600)
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${run}/build"
   --output-on-failure --parallel 1 -R "^forge_package_vm_wasm_optimizing_component$"
   COMMAND_ERROR_IS_FATAL ANY TIMEOUT 40)
message(STATUS "Relocated runtime-only consumer/helper gate completed; owned output retained: ${run}")
