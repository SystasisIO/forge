cmake_minimum_required(VERSION 3.31)

project(AlignedMultiIndexHostTest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT ALIGNED_MULTI_INDEX_WASM OR NOT EXISTS "${ALIGNED_MULTI_INDEX_WASM}")
   message(FATAL_ERROR "ALIGNED_MULTI_INDEX_WASM must name the built contract")
endif()

find_package(
   Forge CONFIG REQUIRED
   COMPONENTS chain_protocol tooling_testing
)

add_executable(aligned_multi_index_vm_tests vm_tests.cpp)
target_compile_definitions(
   aligned_multi_index_vm_tests
   PRIVATE ALIGNED_MULTI_INDEX_WASM="${ALIGNED_MULTI_INDEX_WASM}"
)
target_link_libraries(
   aligned_multi_index_vm_tests
   PRIVATE
      Forge::forge_chain_protocol
      Forge::forge_tooling_testing
)
