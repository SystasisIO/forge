cmake_minimum_required(VERSION 3.31)

project(forge-package-plugins-descriptor-surfaces LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(
   Forge CONFIG REQUIRED COMPONENTS
      plugins_crypto_secrets
      plugins_chain_signer
      plugins_db_store
      plugins_net_http_server
      plugins_net_p2p_node
      plugins_net_p2p_pubsub
      plugins_net_p2p_resolver
)

add_executable(forge_package_plugins_descriptor_surfaces main.cpp)
target_link_libraries(
   forge_package_plugins_descriptor_surfaces PRIVATE
      Forge::forge_plugins_crypto_secrets
      Forge::forge_plugins_chain_signer
      Forge::forge_plugins_db_store
      Forge::forge_plugins_net_http_server
      Forge::forge_plugins_net_p2p_node
      Forge::forge_plugins_net_p2p_pubsub
      Forge::forge_plugins_net_p2p_resolver
)
