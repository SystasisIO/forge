cmake_minimum_required(VERSION 3.31)

project(forge-package-vm-wasm-interpret-component LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS vm_wasm_interpret)
option(FORGE_PACKAGE_TEST_WITH_JIT "Require the installed JIT and its canonical public profiling tests" OFF)

add_executable(forge_package_vm_wasm_interpret_component main.cpp)
target_link_libraries(forge_package_vm_wasm_interpret_component PRIVATE Forge::forge_vm_wasm_interpret)
if(APPLE)
   target_link_options(forge_package_vm_wasm_interpret_component PRIVATE
      "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/forge_package_vm_wasm_interpret_component.linkmap")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
   target_link_options(forge_package_vm_wasm_interpret_component PRIVATE
      "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/forge_package_vm_wasm_interpret_component.linkmap")
endif()

if(FORGE_PACKAGE_TEST_WITH_JIT)
   get_target_property(_definitions Forge::forge_vm_wasm_interpret INTERFACE_COMPILE_DEFINITIONS)
   if(NOT Forge_BUILT_WITH_VM_WASM_LLVM_JIT OR NOT "FORGE_VM_WASM_INTERPRET_HAS_JIT=1" IN_LIST _definitions)
      message(FATAL_ERROR "Installed interpreter must export JIT support for profiling qualification")
   endif()
   enable_testing()
   add_executable(forge_package_vm_wasm_jit_profile
      ../vm_wasm_execution/main.cpp ../vm_wasm_execution/profile_public_tests.cpp)
   target_link_libraries(forge_package_vm_wasm_jit_profile PRIVATE
      Forge::forge_vm_wasm_interpret Boost::headers)
   if(APPLE)
      target_link_options(forge_package_vm_wasm_jit_profile PRIVATE
         "LINKER:-map,${CMAKE_CURRENT_BINARY_DIR}/forge_package_vm_wasm_jit_profile.linkmap")
   elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      target_link_options(forge_package_vm_wasm_jit_profile PRIVATE
         "LINKER:-Map,${CMAKE_CURRENT_BINARY_DIR}/forge_package_vm_wasm_jit_profile.linkmap")
   endif()
   add_test(NAME package_vm_wasm_jit_profile COMMAND forge_package_vm_wasm_jit_profile
      --catch_system_errors=no --report_level=detailed)
   set_tests_properties(package_vm_wasm_jit_profile PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE)
endif()
