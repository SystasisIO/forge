cmake_minimum_required(VERSION 3.31)

project(forge-package-vm-wasm-optimizing-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS asio vm_wasm_optimizing)

add_executable(forge_package_vm_wasm_optimizing_component main.cpp)
target_link_libraries(forge_package_vm_wasm_optimizing_component PRIVATE
   Forge::forge_asio Forge::forge_vm_wasm_optimizing)

set(FORGE_OC_CONSUMER_REQUEST "" CACHE FILEPATH "Reviewed native add fixture request")
set(FORGE_OC_CONSUMER_ARTIFACT "" CACHE FILEPATH "Matching reviewed native add artifact")
set(FORGE_OC_CONSUMER_HELPER "" CACHE FILEPATH "Actual helper in the relocated prefix/bin")
set(FORGE_OC_CONSUMER_CACHE "" CACHE PATH "Fresh owned per-run consumer cache parent")
set(FORGE_OC_CONSUMER_AS_BYTES "0" CACHE STRING "Explicit finite test AS allowance, not a default")
foreach(input IN ITEMS FORGE_OC_CONSUMER_REQUEST FORGE_OC_CONSUMER_ARTIFACT)
   if(NOT IS_ABSOLUTE "${${input}}" OR NOT EXISTS "${${input}}")
      message(FATAL_ERROR "${input} must name an existing absolute reviewed fixture file")
   endif()
endforeach()
foreach(input IN ITEMS FORGE_OC_CONSUMER_HELPER FORGE_OC_CONSUMER_CACHE)
   if(NOT IS_ABSOLUTE "${${input}}" OR NOT EXISTS "${${input}}")
      message(FATAL_ERROR "${input} must name an existing absolute relocated helper/owned cache path")
   endif()
endforeach()
if(NOT FORGE_OC_CONSUMER_AS_BYTES MATCHES "^[0-9]+$" OR FORGE_OC_CONSUMER_AS_BYTES STREQUAL "0")
   message(FATAL_ERROR "explicit finite helper AS bytes are required")
endif()

enable_testing()
add_test(NAME forge_package_vm_wasm_optimizing_component
   COMMAND forge_package_vm_wasm_optimizing_component
      "${FORGE_OC_CONSUMER_REQUEST}" "${FORGE_OC_CONSUMER_ARTIFACT}"
      "${FORGE_OC_CONSUMER_HELPER}" "${FORGE_OC_CONSUMER_CACHE}" "${FORGE_OC_CONSUMER_AS_BYTES}")
set_tests_properties(forge_package_vm_wasm_optimizing_component PROPERTIES TIMEOUT 30)
