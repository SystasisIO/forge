cmake_minimum_required(VERSION 3.31)

project(forge-package-plugins-net-p2p-node LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(retired_components)
foreach(role IN ITEMS pubsub diagnostics)
   list(APPEND retired_components "plugins_net_p2p_${role}")
endforeach()
find_package(Forge CONFIG REQUIRED COMPONENTS plugins_net_p2p_node
   OPTIONAL_COMPONENTS ${retired_components})
foreach(component IN LISTS retired_components)
   if(Forge_${component}_FOUND OR TARGET Forge::forge_${component})
      message(FATAL_ERROR "Retired P2P plugin component remains available: ${component}")
   endif()
endforeach()

add_executable(forge_package_plugins_net_p2p_node main.cpp)
target_link_libraries(forge_package_plugins_net_p2p_node PRIVATE Forge::forge_plugins_net_p2p_node)
