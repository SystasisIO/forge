cmake_minimum_required(VERSION 3.31)

project(forge-package-core-lightweight LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED)

get_target_property(_core_links Forge::forge_core INTERFACE_LINK_LIBRARIES)
if(_core_links MATCHES "forge_(raw|reflect|api_core|swarm_core|db_object)")
   message(FATAL_ERROR "Core-only consumer acquired an upper-layer dependency: ${_core_links}")
endif()

add_executable(forge_package_core_lightweight main.cpp)
target_sources(forge_package_core_lightweight
   PRIVATE FILE_SET CXX_MODULES FILES conversion.cppm
)
target_link_libraries(forge_package_core_lightweight PRIVATE Forge::forge_core)
