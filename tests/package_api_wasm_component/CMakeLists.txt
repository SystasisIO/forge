cmake_minimum_required(VERSION 3.31)
project(ForgePackageApiWasm LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
find_package(Forge CONFIG REQUIRED COMPONENTS api_wasm)

# Audit an installed native dependency closure, never reconstruct a guest graph.
function(check_protocol_dependency target)
   get_property(visited GLOBAL PROPERTY checked_protocol_dependencies)
   if("${target}" IN_LIST visited OR NOT TARGET "${target}")
      return()
   endif()
   set_property(GLOBAL APPEND PROPERTY checked_protocol_dependencies "${target}")
   if(target MATCHES "Forge::forge_(vm|execution|asio|auth|db|plugins|tooling)($|_)"
      OR target MATCHES "Forge::forge_api_(core$|http|auth|stream|transport|quic|p2p|websocket)"
      OR target MATCHES "Forge::forge_net_(http$|tls|tcp|quic|p2p|transport|websocket)"
      OR target MATCHES "^(OpenSSL::|Boost::asio$|Boost::beast$)")
      message(FATAL_ERROR "Forbidden api_wasm dependency: ${target}")
   endif()
   get_target_property(dependencies "${target}" INTERFACE_LINK_LIBRARIES)
   foreach(dependency IN LISTS dependencies)
      string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" dependency "${dependency}")
      check_protocol_dependency("${dependency}")
   endforeach()
endfunction()
check_protocol_dependency(Forge::forge_api_wasm)

add_executable(forge_package_api_wasm_component main.cpp)
target_link_libraries(forge_package_api_wasm_component PRIVATE Forge::forge_api_wasm)
