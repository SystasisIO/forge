cmake_minimum_required(VERSION 3.31)

project(forge-package-swarm-core-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_Ngtcp2 ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Notcurses ON)

find_package(Forge CONFIG REQUIRED COMPONENTS swarm_core)

get_target_property(_swarm_links Forge::forge_swarm_core INTERFACE_LINK_LIBRARIES)
if(_swarm_links MATCHES "forge_(chain|db|api|asio|app|net|plugins)(_|;|$)")
   message(FATAL_ERROR "Swarm Core acquired a runtime dependency: ${_swarm_links}")
endif()

add_executable(forge_package_swarm_core_component main.cpp)
target_link_libraries(forge_package_swarm_core_component PRIVATE Forge::forge_swarm_core)
