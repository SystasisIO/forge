cmake_minimum_required(VERSION 3.31)

project(forge-package-auth-session-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS auth_session)

get_target_property(session_links Forge::forge_auth_session INTERFACE_LINK_LIBRARIES)
if("${session_links}" MATCHES "forge_auth_pairing")
   message(FATAL_ERROR "Sessions must use neutral credentials without pairing")
endif()

add_executable(forge_package_auth_session_component main.cpp)
target_link_libraries(forge_package_auth_session_component PRIVATE Forge::forge_auth_session)
