cmake_minimum_required(VERSION 3.31)

project(ProductProtocol LANGUAGES C CXX)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(
   Forge CONFIG REQUIRED
   COMPONENTS chain_protocol chain_savanna_values tooling_testing crypto_digest raw vm_wasm_interpret
)
if(PRODUCT_FORGE_SOURCE_ROOT)
   include(
      "${PRODUCT_FORGE_SOURCE_ROOT}/guest/cmake/ForgeContractFunctions.cmake"
   )
else()
   find_package(ForgeContract CONFIG REQUIRED)
endif()

add_subdirectory(libraries/chain)

add_executable(product_protocol_host_tests host_tests.cpp)
target_link_libraries(
   product_protocol_host_tests
   PRIVATE
      product_chain_protocol
      Forge::forge_chain_protocol
      Forge::forge_chain_savanna_values
      Forge::forge_raw
)

forge_add_contract_project(
   product_guest
   SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/guest"
   BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/product.guest"
   CONTRACT product
   SOURCE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}"
)
get_target_property(
   _product_wasm product_guest FORGE_CONTRACT_WASM_FILE
)

add_executable(product_protocol_vm_tests vm_tests.cpp)
target_compile_definitions(
   product_protocol_vm_tests
   PRIVATE PRODUCT_PROTOCOL_WASM="${_product_wasm}"
)
target_link_libraries(
   product_protocol_vm_tests
   PRIVATE
      product_chain_protocol
      Forge::forge_chain_protocol
      Forge::forge_chain_savanna_values
      Forge::forge_tooling_testing
      Forge::forge_raw
)
add_dependencies(product_protocol_vm_tests product_guest)

install(
   TARGETS
      product_chain_values
      product_chain_limits
      product_chain_protocol
   EXPORT ProductProtocolTargets
   FILE_SET forge_contract_modules
      DESTINATION "${CMAKE_INSTALL_DATADIR}/product-protocol/modules"
   FILE_SET forge_contract_headers
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
   EXPORT ProductProtocolTargets
   FILE ProductProtocolTargets.cmake
   NAMESPACE Product::
   DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ProductProtocol"
)
