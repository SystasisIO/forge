cmake_minimum_required(VERSION 3.31)

project(forge-package-chain-api-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(
   Forge CONFIG REQUIRED
   COMPONENTS
      api_core
      api_http
      api_p2p
      api_transport
      app
      asio
      chain_api
      chain_protocol
      config_core
      crypto_digest
      net_http
      net_p2p
      plugins_crypto_secrets
      plugins_net_p2p_node
      plugins_net_p2p_resolver
)

if(NOT Forge_BUILT_WITH_PORTABLE_VERIFIED_CLIENT)
   message(FATAL_ERROR "Forge package must provide Forge_BUILT_WITH_PORTABLE_VERIFIED_CLIENT")
endif()

add_executable(
   forge_package_chain_api_component
   main.cpp
   p2p_runtime.cpp
   portable_verified_client.cpp
   read_fixture.cpp
   read_e2e.cpp
   read_e2e_http.cpp
   read_e2e_p2p_info_block.cpp
   read_e2e_p2p_info_block_client.cpp
   read_e2e_p2p_info_block_server.cpp
   read_e2e_p2p_state.cpp
   read_e2e_p2p_state_client.cpp
   read_e2e_p2p_state_server.cpp
   surface_checks_api_read.cpp
   surface_checks_api_write.cpp
   surface_checks_block.cpp
   surface_checks_codec.cpp
   surface_checks_info_admin.cpp
   surface_checks_state.cpp
   test_support.cpp
   verifier_fixture.cpp
   write_fixture.cpp
   write_e2e.cpp
   write_e2e_http.cpp
   write_e2e_p2p_admin.cpp
   write_e2e_p2p_admin_client.cpp
   write_e2e_p2p_admin_server.cpp
   write_e2e_p2p_transaction.cpp
   write_e2e_p2p_transaction_client.cpp
   write_e2e_p2p_transaction_server.cpp
)
target_sources(
   forge_package_chain_api_component
   PRIVATE
      FILE_SET CXX_MODULES
      FILES
         p2p_runtime.cppm
         read_e2e.cppm
         read_e2e_http.cppm
         read_e2e_p2p_info_block.cppm
         read_e2e_p2p_info_block_client.cppm
         read_e2e_p2p_info_block_server.cppm
         read_e2e_p2p_state.cppm
         read_e2e_p2p_state_client.cppm
         read_e2e_p2p_state_server.cppm
         read_fixture.cppm
         read_result.cppm
         surface_checks.cppm
         test_support.cppm
         verifier_fixture.cppm
         write_e2e.cppm
         write_e2e_http.cppm
         write_e2e_p2p_admin.cppm
         write_e2e_p2p_admin_client.cppm
         write_e2e_p2p_admin_server.cppm
         write_e2e_p2p_transaction.cppm
         write_e2e_p2p_transaction_client.cppm
         write_e2e_p2p_transaction_server.cppm
         write_fixture.cppm
         write_result.cppm
)
target_link_libraries(
   forge_package_chain_api_component
   PRIVATE
      Forge::forge_api_core
      Forge::forge_api_http
      Forge::forge_api_p2p
      Forge::forge_api_transport
      Forge::forge_app
      Forge::forge_asio
      Forge::forge_chain_api
      Forge::forge_chain_protocol
      Forge::forge_chain_savanna
      Forge::forge_config_core
      Forge::forge_crypto_digest
      Forge::forge_net_http
      Forge::forge_net_p2p
      Forge::forge_plugins_crypto_secrets
      Forge::forge_plugins_net_p2p_node
      Forge::forge_plugins_net_p2p_resolver
)
