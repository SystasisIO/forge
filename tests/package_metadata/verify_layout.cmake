cmake_minimum_required(VERSION 3.31)

foreach(required IN ITEMS FORGE_PACKAGE_TEST_SOURCE_ROOT FORGE_PACKAGE_TEST_BUILD_ROOT
   FORGE_PACKAGE_TEST_INSTALL_LIBDIR FORGE_PACKAGE_TEST_INSTALL_DATADIR
   FORGE_PACKAGE_TEST_GENERATOR FORGE_PACKAGE_TEST_CXX_COMPILER)
   if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
      message(FATAL_ERROR "${required} is required")
   endif()
endforeach()

# Inspect the genuine generated install export without building unrelated leaves.
file(GLOB exports "${FORGE_PACKAGE_TEST_BUILD_ROOT}/CMakeFiles/Export/*/ForgeTargets.cmake")
list(LENGTH exports export_count)
if(NOT export_count EQUAL 1)
   message(FATAL_ERROR "Expected one generated Forge install export; found ${export_count}")
endif()
list(GET exports 0 targets_file)
file(READ "${targets_file}" targets)
set(marker "# Create imported target Forge::forge_codec_xml")
string(FIND "${targets}" "${marker}" start)
if(start EQUAL -1)
   message(FATAL_ERROR "Generated install export does not contain codec_xml")
endif()
string(SUBSTRING "${targets}" ${start} -1 target_tail)
string(FIND "${target_tail}" "\n# Create imported target " end)
if(end EQUAL -1)
   message(FATAL_ERROR "Cannot delimit codec_xml install export")
endif()
string(SUBSTRING "${target_tail}" 0 ${end} xml_target)
foreach(forbidden IN ITEMS "${FORGE_PACKAGE_TEST_SOURCE_ROOT}" "${FORGE_PACKAGE_TEST_BUILD_ROOT}" "vendor/pugixml")
   string(FIND "${xml_target}" "${forbidden}" found)
   if(NOT found EQUAL -1)
      message(FATAL_ERROR "codec_xml install export contains private path: ${forbidden}")
   endif()
endforeach()

set(test_root "${FORGE_PACKAGE_TEST_BUILD_ROOT}/package-metadata-layout")
set(original "${test_root}/original")
set(relocated "${test_root}/relocated")
set(package_dir "${FORGE_PACKAGE_TEST_INSTALL_LIBDIR}/cmake/Forge")
set(notice_metadata "${FORGE_PACKAGE_TEST_BUILD_ROOT}/ForgeNoticeFiles.cmake")
include("${notice_metadata}")
file(MAKE_DIRECTORY "${original}/${package_dir}" "${original}/${FORGE_PACKAGE_TEST_INSTALL_DATADIR}/doc/Forge")
file(COPY "${FORGE_PACKAGE_TEST_BUILD_ROOT}/ForgeConfig.cmake"
   "${FORGE_PACKAGE_TEST_BUILD_ROOT}/ForgeConfigVersion.cmake"
   "${FORGE_PACKAGE_TEST_BUILD_ROOT}/ForgeTargets.cmake"
   DESTINATION "${original}/${package_dir}")
foreach(notice IN LISTS FORGE_PACKAGE_NOTICE_SOURCES)
   file(COPY "${notice}"
      DESTINATION "${original}/${FORGE_PACKAGE_TEST_INSTALL_DATADIR}/doc/Forge")
endforeach()
file(COPY "${original}/" DESTINATION "${relocated}")

# These are metadata-only fixtures with real build-tree targets, not installed
# libraries. Exercise the real configured package at both prefix locations and
# separately prove focused build-tree discovery needs no installed notices.
foreach(case IN ITEMS original relocated build-tree)
   set(command "${CMAKE_COMMAND}" --fresh
      -S "${FORGE_PACKAGE_TEST_SOURCE_ROOT}/tests/package_metadata"
      -B "${test_root}/consumer-${case}"
      -G "${FORGE_PACKAGE_TEST_GENERATOR}"
      "-DCMAKE_CXX_COMPILER=${FORGE_PACKAGE_TEST_CXX_COMPILER}"
      "-DFORGE_EXPECTED_NOTICE_METADATA=${notice_metadata}")
   if(case STREQUAL "build-tree")
      list(APPEND command "-DForge_DIR=${FORGE_PACKAGE_TEST_BUILD_ROOT}")
   else()
      list(APPEND command "-DForge_DIR=${${case}}/${package_dir}"
         "-DFORGE_EXPECTED_NOTICES_DIR=${${case}}/${FORGE_PACKAGE_TEST_INSTALL_DATADIR}/doc/Forge"
         -DFORGE_EXPECT_NOTICES_PRESENT=ON)
   endif()
   if(FORGE_PACKAGE_TEST_OSX_SYSROOT)
      list(APPEND command "-DCMAKE_OSX_SYSROOT=${FORGE_PACKAGE_TEST_OSX_SYSROOT}")
   endif()
   execute_process(COMMAND ${command} COMMAND_ERROR_IS_FATAL ANY)
endforeach()

# Reject incomplete redistribution and directories masquerading as notice files.
# Only the named files within these owned metadata fixtures are changed.
foreach(case IN ITEMS missing-notice directory-notice)
   set(prefix "${test_root}/${case}")
   file(MAKE_DIRECTORY "${prefix}")
   set(notice "${prefix}/${FORGE_PACKAGE_TEST_INSTALL_DATADIR}/doc/Forge/NOTICE")
   # A previous directory-notice run must not obstruct the next fixture copy.
   file(REMOVE_RECURSE "${notice}")
   file(COPY "${original}/" DESTINATION "${prefix}")
   file(REMOVE_RECURSE "${notice}")
   if(case STREQUAL "directory-notice")
      file(MAKE_DIRECTORY "${notice}")
   endif()
   set(command "${CMAKE_COMMAND}" --fresh
      -S "${FORGE_PACKAGE_TEST_SOURCE_ROOT}/tests/package_metadata"
      -B "${test_root}/consumer-${case}"
      -G "${FORGE_PACKAGE_TEST_GENERATOR}"
      "-DCMAKE_CXX_COMPILER=${FORGE_PACKAGE_TEST_CXX_COMPILER}"
      "-DForge_DIR=${prefix}/${package_dir}"
      -DFORGE_EXPECT_NOTICES_PRESENT=ON)
   if(FORGE_PACKAGE_TEST_OSX_SYSROOT)
      list(APPEND command "-DCMAKE_OSX_SYSROOT=${FORGE_PACKAGE_TEST_OSX_SYSROOT}")
   endif()
   execute_process(COMMAND ${command} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
   if(result EQUAL 0 OR NOT "${output}${error}" MATCHES "Legal-notices metadata does not locate NOTICE")
      message(FATAL_ERROR "${case} did not produce the expected legal-notices rejection: ${output}${error}")
   endif()
endforeach()
message(STATUS "Package metadata layout, relocation, notice closure/rejection, build-tree discovery and codec_xml install export passed")
