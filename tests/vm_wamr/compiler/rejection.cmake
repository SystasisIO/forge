execute_process(COMMAND "${COMPILER}" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result STREQUAL "1" OR NOT error MATCHES "^FAIL Expected input WASM and output AOT paths")
   message(FATAL_ERROR "Compiler rejection escaped its C++ handler: ${result}: ${output}${error}")
endif()
