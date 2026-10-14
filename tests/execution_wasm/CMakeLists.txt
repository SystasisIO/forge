add_executable(test_forge_execution_wasm main.cpp execution_tests.cpp)
target_link_libraries(test_forge_execution_wasm PRIVATE forge_execution_wasm Boost::included_unit_test_framework)
add_test(NAME test_forge_execution_wasm COMMAND test_forge_execution_wasm --catch_system_errors=no)
set_tests_properties(test_forge_execution_wasm PROPERTIES TIMEOUT 30 LABELS execution_wasm)

add_executable(test_forge_execution_wasm_sdk sdk_fixture.cpp)
target_link_libraries(test_forge_execution_wasm_sdk PRIVATE forge_execution_wasm)
set(FORGE_EXECUTION_WASM_SDK_FIXTURE "" CACHE FILEPATH "Real compiled guest/tests/wire_values fixture")
if(FORGE_EXECUTION_WASM_SDK_FIXTURE)
   add_test(NAME test_forge_execution_wasm_sdk
      COMMAND test_forge_execution_wasm_sdk "${FORGE_EXECUTION_WASM_SDK_FIXTURE}")
   set_tests_properties(test_forge_execution_wasm_sdk PROPERTIES TIMEOUT 10 LABELS execution_wasm)
endif()
