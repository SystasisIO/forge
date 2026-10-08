cmake_minimum_required(VERSION 3.31)
project(ForgeDonorCompatibilityTests LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT EXISTS "${FORGE_CONTRACT_TEST_DONOR_COMPAT_WASM}")
   message(FATAL_ERROR "FORGE_CONTRACT_TEST_DONOR_COMPAT_WASM must name the built donorcompat contract")
endif()
find_package(Forge CONFIG REQUIRED COMPONENTS chain_protocol variant raw vm_wasm_interpret)
find_package(Boost REQUIRED)
add_executable(forge_contract_donor_compatibility_tests compatibility_tests.cpp)
target_link_libraries(forge_contract_donor_compatibility_tests PRIVATE
   Forge::forge_chain_protocol Forge::forge_variant Forge::forge_raw
   Forge::forge_vm_wasm_interpret Boost::headers)
target_compile_definitions(forge_contract_donor_compatibility_tests PRIVATE
   FORGE_CONTRACT_TEST_DONOR_COMPAT_WASM="${FORGE_CONTRACT_TEST_DONOR_COMPAT_WASM}")
enable_testing()
add_test(NAME forge_contract_donor_compatibility_tests COMMAND forge_contract_donor_compatibility_tests)

set(_runtime "${CMAKE_CURRENT_SOURCE_DIR}/../../libraries/runtime")
set(_libc "${CMAKE_CURRENT_SOURCE_DIR}/../../../vendor/cdt-musl")
add_executable(forge_contract_stdio_lock_tests stdio_lock_tests.cpp "${_runtime}/stdio_lock.cpp")
target_include_directories(forge_contract_stdio_lock_tests PRIVATE
   "${_runtime}" "${_libc}/include" "${_libc}/src/internal" "${_libc}/arch/eos")
target_compile_options(forge_contract_stdio_lock_tests PRIVATE -nostdinc)
add_test(NAME forge_contract_stdio_lock_tests COMMAND forge_contract_stdio_lock_tests)
