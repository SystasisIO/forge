if(NOT DEFINED FORGE_CONTRACT_RUNTIME_POLICY)
   message(FATAL_ERROR "FORGE_CONTRACT_RUNTIME_POLICY is required")
endif()

include("${FORGE_CONTRACT_RUNTIME_POLICY}")

set(_runtime_test_root "${CMAKE_CURRENT_BINARY_DIR}/contract-runtime-search")
file(REMOVE_RECURSE "${_runtime_test_root}")
file(MAKE_DIRECTORY "${_runtime_test_root}/prefix/lib" "${_runtime_test_root}/prefix/lib64")
forge_contract_sdk_runtime_search_directories(
   _runtime_directories
   INSTALL_LIBRARY_DIR "${_runtime_test_root}/sdk/lib"
   PREFIXES "${_runtime_test_root}/prefix" "${_runtime_test_root}/missing"
   DIRECTORIES "${_runtime_test_root}/custom" "${_runtime_test_root}/custom"
)
foreach(
   _expected
   "${_runtime_test_root}/sdk/lib"
   "${_runtime_test_root}/prefix/lib"
   "${_runtime_test_root}/prefix/lib64"
   "${_runtime_test_root}/custom"
)
   list(FIND _runtime_directories "${_expected}" _position)
   if(_position EQUAL -1)
      message(FATAL_ERROR "runtime dependency search directory is missing: ${_expected}")
   endif()
endforeach()
list(LENGTH _runtime_directories _runtime_directory_count)
if(NOT _runtime_directory_count EQUAL 4)
   message(FATAL_ERROR "runtime dependency search directories were not normalized: ${_runtime_directories}")
endif()

set(_runtime_dependency "${_runtime_test_root}/readonly-runtime")
file(WRITE "${_runtime_dependency}" "runtime")
file(CHMOD "${_runtime_dependency}" PERMISSIONS OWNER_READ GROUP_READ WORLD_READ)
forge_contract_sdk_prepare_runtime_dependency("${_runtime_dependency}")
file(APPEND "${_runtime_dependency}" "-writable")

set(
   _resolved_dependencies
   "/opt/homebrew/Cellar/llvm/22.1.8/lib/libLLVM.dylib"
   "/opt/homebrew/Cellar/llvm/22.1.8/lib/libclang-cpp.dylib"
)
forge_contract_sdk_match_runtime_dependency(
   _matched_dependency
   "/opt/homebrew/opt/llvm/lib/libLLVM.dylib"
   ${_resolved_dependencies}
)
if(NOT _matched_dependency STREQUAL "/opt/homebrew/Cellar/llvm/22.1.8/lib/libLLVM.dylib")
   message(FATAL_ERROR "runtime dependency symlink path was not matched to its bundled library")
endif()
forge_contract_sdk_match_runtime_dependency(
   _missing_dependency
   "/opt/homebrew/opt/llvm/lib/libMissing.dylib"
   ${_resolved_dependencies}
)
if(_missing_dependency)
   message(FATAL_ERROR "unrelated runtime dependency matched a bundled library")
endif()

foreach(
   _dependency
   "libc++.so.1"
   "/usr/lib/llvm-22/lib/libc++abi.so.1"
   "/lib/x86_64-linux-gnu/libunwind.so.1"
   "/usr/lib/llvm-22/lib/libLLVM.so.22.1"
   "/usr/lib/x86_64-linux-gnu/libLLVM-22.so.1"
   "/usr/lib/llvm-22/lib/libclang-cpp.so.22.1"
   "/usr/lib/llvm-22/lib/liblldWasm.so.22.1"
)
   if(NOT _dependency MATCHES "${FORGE_CONTRACT_LLVM_RUNTIME_REGEX}")
      message(FATAL_ERROR "LLVM runtime is not selected for bundling: ${_dependency}")
   endif()
endforeach()

if(NOT "libstdc++.so.6" MATCHES "${FORGE_CONTRACT_LINUX_CXX_RUNTIME_REGEX}")
   message(FATAL_ERROR "Linux C++ runtime is not selected for bundling")
endif()

foreach(_dependency "libc.so.6" "/usr/lib/x86_64-linux-gnu/libm.so.6")
   if(_dependency MATCHES "${FORGE_CONTRACT_LLVM_RUNTIME_REGEX}")
      message(FATAL_ERROR "system runtime is incorrectly selected for bundling: ${_dependency}")
   endif()
endforeach()

set(UNIX TRUE)
set(APPLE FALSE)
forge_contract_sdk_runtime_dependency_filters(_filters)
list(FIND _filters PRE_INCLUDE_REGEXES _include_position)
list(FIND _filters POST_INCLUDE_REGEXES _post_include_position)
list(FIND _filters POST_EXCLUDE_REGEXES _exclude_position)
list(FIND _filters PRE_EXCLUDE_REGEXES _pre_exclude_position)
if(_include_position EQUAL -1 OR _post_include_position EQUAL -1 OR _exclude_position EQUAL -1)
   message(FATAL_ERROR "Linux runtime dependency filters are incomplete: ${_filters}")
endif()
if(NOT _pre_exclude_position EQUAL -1)
   message(FATAL_ERROR "resolved system paths must not use PRE_EXCLUDE_REGEXES: ${_filters}")
endif()
if(_post_include_position GREATER _exclude_position)
   message(FATAL_ERROR "Linux LLVM C++ runtime post-includes must precede system exclusions")
endif()

foreach(
   _required
   "${FORGE_CONTRACT_LLVM_RUNTIME_REGEX}"
   "${FORGE_CONTRACT_LINUX_CXX_RUNTIME_REGEX}"
)
   set(_positions)
   set(_index 0)
   foreach(_filter IN LISTS _filters)
      if(_filter STREQUAL _required)
         list(APPEND _positions ${_index})
      endif()
      math(EXPR _index "${_index} + 1")
   endforeach()
   list(LENGTH _positions _position_count)
   if(NOT _position_count EQUAL 2)
      message(FATAL_ERROR "runtime filter must be present in both include phases: ${_required}")
   endif()
   list(GET _positions 0 _first_position)
   list(GET _positions 1 _second_position)
   if(_first_position LESS_EQUAL _include_position OR _first_position GREATER_EQUAL _post_include_position)
      message(FATAL_ERROR "runtime filter is missing from PRE_INCLUDE_REGEXES: ${_required}")
   endif()
   if(_second_position LESS_EQUAL _post_include_position OR _second_position GREATER_EQUAL _exclude_position)
      message(FATAL_ERROR "runtime filter is missing from POST_INCLUDE_REGEXES: ${_required}")
   endif()
endforeach()

foreach(_system_path "^/System/Library/" "^/usr/lib/" "^/lib/")
   list(FIND _filters "${_system_path}" _position)
   if(_position LESS _exclude_position)
      message(FATAL_ERROR "system path is not a post-resolution exclusion: ${_system_path}")
   endif()
endforeach()

# Exercise the owning launcher's local resolution without creating ExternalProjects.
get_filename_component(_sdk_cmake_dir "${FORGE_CONTRACT_RUNTIME_POLICY}" DIRECTORY)
file(READ "${_sdk_cmake_dir}/ForgeContractFunctions.cmake" _launcher)
string(REGEX MATCHALL "--parallel" _jobs_options "${_launcher}")
list(LENGTH _jobs_options _jobs_option_count)
string(FIND "${_launcher}"
   [==[--target "${ARG_CONTRACT}_artifacts" --parallel "${_build_jobs}"]==]
   _jobs_command)
if(NOT _jobs_option_count EQUAL 1 OR _jobs_command EQUAL -1)
   message(FATAL_ERROR "launcher must forward the resolved jobs to its only build command")
endif()
string(FIND "${_launcher}" "function(forge_add_contract_project target)" _jobs_begin)
string(FIND "${_launcher}" "   cmake_parse_arguments(" _jobs_end)
if(_jobs_begin EQUAL -1 OR _jobs_end LESS _jobs_begin)
   message(FATAL_ERROR "launcher resolution must precede argument/ExternalProject actions")
endif()
math(EXPR _jobs_length "${_jobs_end} - ${_jobs_begin}")
string(SUBSTRING "${_launcher}" ${_jobs_begin} ${_jobs_length} _jobs_probe)
set(_jobs_output "${_runtime_test_root}/launcher-jobs.txt")
string(APPEND _jobs_probe
   "   file(WRITE \"${_jobs_output}\" \"\${_build_jobs}\")\n"
   "endfunction()\n"
   "forge_add_contract_project(probe)\n")
set(_jobs_probe_file "${_runtime_test_root}/launcher-jobs.cmake")
file(WRITE "${_jobs_probe_file}" "${_jobs_probe}")
foreach(_jobs IN ITEMS unset 1 2 8 0002)
   file(REMOVE "${_jobs_output}")
   set(_jobs_environment --unset=CMAKE_BUILD_PARALLEL_LEVEL)
   if(_jobs STREQUAL "unset")
      set(_expected_jobs 4)
   else()
      list(APPEND _jobs_environment "CMAKE_BUILD_PARALLEL_LEVEL=${_jobs}")
      set(_expected_jobs "${_jobs}")
   endif()
   execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env ${_jobs_environment}
         "${CMAKE_COMMAND}" -P "${_jobs_probe_file}"
      RESULT_VARIABLE _jobs_result OUTPUT_VARIABLE _jobs_stdout ERROR_VARIABLE _jobs_stderr)
   if(NOT _jobs_result EQUAL 0 OR NOT EXISTS "${_jobs_output}")
      message(FATAL_ERROR "launcher rejected valid jobs '${_jobs}': ${_jobs_stdout}${_jobs_stderr}")
   endif()
   file(READ "${_jobs_output}" _actual_jobs)
   if(NOT _actual_jobs STREQUAL _expected_jobs)
      message(FATAL_ERROR "launcher jobs '${_jobs}' resolved to '${_actual_jobs}', expected '${_expected_jobs}'")
   endif()
endforeach()
string(ASCII 217 162 _jobs_non_ascii)
foreach(_jobs IN ITEMS "" 0 000 -1 1.5 native " 2" +2 "${_jobs_non_ascii}")
   file(REMOVE "${_jobs_output}")
   execute_process(
      COMMAND "${CMAKE_COMMAND}" -E env "CMAKE_BUILD_PARALLEL_LEVEL=${_jobs}"
         "${CMAKE_COMMAND}" -P "${_jobs_probe_file}"
      RESULT_VARIABLE _jobs_result OUTPUT_VARIABLE _jobs_stdout ERROR_VARIABLE _jobs_stderr)
   string(CONCAT _jobs_log "${_jobs_stdout}" "${_jobs_stderr}")
   if(_jobs_result EQUAL 0 OR EXISTS "${_jobs_output}" OR NOT _jobs_log MATCHES "positive integer")
      message(FATAL_ERROR "launcher did not refuse invalid jobs '${_jobs}' before work: ${_jobs_log}")
   endif()
endforeach()
