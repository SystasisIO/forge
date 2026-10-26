cmake_minimum_required(VERSION 3.31)
project(forge-package-net-tlsnotary-values LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS net_tlsnotary_values raw)
add_executable(forge_package_net_tlsnotary_values_component main.cpp)
target_link_libraries(forge_package_net_tlsnotary_values_component PRIVATE
   Forge::forge_net_tlsnotary_values Forge::forge_raw)
