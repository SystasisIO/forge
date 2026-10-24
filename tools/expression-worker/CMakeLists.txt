add_executable(forge_expression_worker main.cpp)
set_target_properties(forge_expression_worker PROPERTIES OUTPUT_NAME forge-expression-worker)
target_include_directories(forge_expression_worker PRIVATE ${PROJECT_SOURCE_DIR}/libraries/expression)
target_link_libraries(forge_expression_worker PRIVATE forge_expression_worker_runtime)

add_custom_command(TARGET forge_expression_worker POST_BUILD
   COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/cmake/cel/verify_worker.py"
      --binary "$<TARGET_FILE:forge_expression_worker>"
      --compiler "${CMAKE_CXX_COMPILER}"
      --source-root "${PROJECT_SOURCE_DIR}"
      --backend-manifest "${PROJECT_BINARY_DIR}/_deps/cel/$<CONFIG>/build-manifest.json"
      --runtime-manifest "${PROJECT_BINARY_DIR}/_deps/cel/$<CONFIG>/runtime-manifest.json"
      --link-map "${FORGE_CEL_WORKER_LINK_MAP}"
      --output "${FORGE_CEL_WORKER_MANIFEST}"
   BYPRODUCTS "${FORGE_CEL_WORKER_MANIFEST}"
   COMMENT "Verifying the isolated expression worker allocation/runtime closure"
   VERBATIM)
