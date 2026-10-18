cmake_minimum_required(VERSION 3.31)

project(forge-package-vm-wasm-interpret-component LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS vm_wasm_interpret)

add_executable(forge_package_vm_wasm_interpret_component main.cpp)
target_link_libraries(forge_package_vm_wasm_interpret_component PRIVATE Forge::forge_vm_wasm_interpret)
