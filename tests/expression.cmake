if(NOT TARGET forge_expression)
   return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(test_forge_expression
   "${CMAKE_CURRENT_LIST_DIR}/unit/main.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/expression_profile.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/expression_runtime.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/expression_artifact.cpp"
)
target_link_libraries(test_forge_expression PRIVATE forge_expression Boost::included_unit_test_framework)
target_compile_definitions(test_forge_expression PRIVATE
   FORGE_EXPRESSION_WORKER="$<TARGET_FILE:forge_expression_worker>")
add_dependencies(test_forge_expression forge_expression_worker)
add_test(NAME test_forge_expression COMMAND test_forge_expression)
add_executable(test_forge_expression_worker
   "${CMAKE_CURRENT_LIST_DIR}/unit/main.cpp"
   "${CMAKE_CURRENT_LIST_DIR}/expression_worker.cpp"
)
target_link_libraries(test_forge_expression_worker PRIVATE forge_expression Boost::included_unit_test_framework)
target_compile_definitions(test_forge_expression_worker PRIVATE
   FORGE_EXPRESSION_WORKER="$<TARGET_FILE:forge_expression_worker>")
add_dependencies(test_forge_expression_worker forge_expression_worker)
add_test(NAME test_forge_expression_worker COMMAND test_forge_expression_worker)
set_tests_properties(test_forge_expression_worker PROPERTIES TIMEOUT 300 LABELS "expression;macos")
add_test(NAME test_forge_expression_oracle COMMAND "${Python3_EXECUTABLE}"
   "${CMAKE_CURRENT_LIST_DIR}/expression_arithmetic_oracle.py" --check)
