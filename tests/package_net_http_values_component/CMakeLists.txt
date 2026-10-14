cmake_minimum_required(VERSION 3.31)
project(forge-package-net-http-values LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS net_http_values)
get_target_property(value_dependencies Forge::forge_net_http_values INTERFACE_LINK_LIBRARIES)
if(NOT "${value_dependencies}" STREQUAL "Forge::forge_raw")
   message(FATAL_ERROR "HTTP method values must depend only on Raw metadata: ${value_dependencies}")
endif()

add_executable(forge_package_net_http_values_component main.cpp)
target_link_libraries(forge_package_net_http_values_component PRIVATE Forge::forge_net_http_values Forge::forge_raw)
