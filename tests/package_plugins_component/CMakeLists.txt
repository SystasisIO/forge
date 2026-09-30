cmake_minimum_required(VERSION 3.31)

project(forge-package-plugins-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS plugins)

add_executable(forge_package_plugins_component main.cpp)
target_link_libraries(
   forge_package_plugins_component PRIVATE
      Forge::forge_plugins
      Forge::forge_plugins_net_p2p_node
      Forge::forge_plugins_crypto_secrets
      Forge::forge_plugins_log_otlp
      Forge::forge_plugins_db_store
)
