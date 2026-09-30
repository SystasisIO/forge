cmake_minimum_required(VERSION 3.31)

project(forge-package-plugins-net-p2p-resolver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS plugins_net_p2p_resolver)

add_executable(forge_package_plugins_net_p2p_resolver main.cpp)
target_link_libraries(
   forge_package_plugins_net_p2p_resolver PRIVATE
      Forge::forge_plugins_net_p2p_resolver
)
