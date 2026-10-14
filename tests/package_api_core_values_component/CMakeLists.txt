cmake_minimum_required(VERSION 3.31)
project(forge-package-api-core-values LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS api_core_values)
get_target_property(value_dependencies Forge::forge_api_core_values INTERFACE_LINK_LIBRARIES)
if(NOT "${value_dependencies}" STREQUAL "Forge::forge_raw;Boost::headers")
   message(FATAL_ERROR "API value dependency boundary changed: ${value_dependencies}")
endif()

add_executable(forge_package_api_core_values_component main.cpp)
target_link_libraries(forge_package_api_core_values_component PRIVATE Forge::forge_api_core_values Forge::forge_raw)
