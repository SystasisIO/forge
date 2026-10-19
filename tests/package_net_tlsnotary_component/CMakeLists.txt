cmake_minimum_required(VERSION 3.31)

project(forge-package-net-tlsnotary LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS net_tlsnotary plugins_net_tlsnotary_session)

add_executable(forge_package_net_tlsnotary_component main.cpp)
target_link_libraries(
   forge_package_net_tlsnotary_component PRIVATE
      Forge::forge_net_tlsnotary
      Forge::forge_plugins_net_tlsnotary_session
)
