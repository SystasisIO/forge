cmake_minimum_required(VERSION 3.31)

project(forge-package-vm-wasm-interpret-component LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS vm_wasm_interpret)

add_executable(forge_package_vm_wasm_interpret_component main.cpp)
target_link_libraries(forge_package_vm_wasm_interpret_component PRIVATE Forge::forge_vm_wasm_interpret)
if(APPLE)
   target_link_options(forge_package_vm_wasm_interpret_component PRIVATE
      "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/forge_package_vm_wasm_interpret_component.linkmap")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
   target_link_options(forge_package_vm_wasm_interpret_component PRIVATE
      "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/forge_package_vm_wasm_interpret_component.linkmap")
endif()
