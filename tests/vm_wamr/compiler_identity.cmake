file(MAKE_DIRECTORY "${TEST_DIR}")
set(_compiler "${TEST_DIR}/compiler-fixture")
set(_script "${ROOT}/cmake/ForgeWamrCompilerIdentity.cmake")
file(WRITE "${_compiler}" "test-owned compiler identity fixture\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${_compiler}"
   -DSOURCE_IDENTITY=expected -DWRITE_IDENTITY=ON -P "${_script}"
   COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${_compiler}"
   -DSOURCE_IDENTITY=expected -P "${_script}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${_compiler}"
   -DSOURCE_IDENTITY=wrong -P "${_script}" RESULT_VARIABLE _wrong_profile OUTPUT_QUIET ERROR_QUIET)
if(_wrong_profile EQUAL 0)
   message(FATAL_ERROR "Wrong compiler source identity was accepted")
endif()
file(APPEND "${_compiler}" "changed\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${_compiler}"
   -DSOURCE_IDENTITY=expected -P "${_script}" RESULT_VARIABLE _modified OUTPUT_QUIET ERROR_QUIET)
if(_modified EQUAL 0)
   message(FATAL_ERROR "Changed compiler binary was accepted")
endif()
file(REMOVE "${_compiler}.forge-identity")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DCOMPILER=${_compiler}"
   -DSOURCE_IDENTITY=expected -P "${_script}" RESULT_VARIABLE _missing OUTPUT_QUIET ERROR_QUIET)
if(_missing EQUAL 0)
   message(FATAL_ERROR "Missing compiler identity was accepted")
endif()
