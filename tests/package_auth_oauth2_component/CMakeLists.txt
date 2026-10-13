cmake_minimum_required(VERSION 3.31)
project(forge-package-auth-oauth2-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_DISABLE_FIND_PACKAGE_OAuth2Dependencies TRUE)
set(CMAKE_DISABLE_FIND_PACKAGE_CURL TRUE)

find_package(Forge CONFIG REQUIRED COMPONENTS auth_oauth2)
get_target_property(auth_links Forge::forge_auth_oauth2 INTERFACE_LINK_LIBRARIES)
if("${auth_links}" MATCHES "auth_liboauth2|oauth2_internal|CURL|CJOSE|JANSSON")
   message(FATAL_ERROR "The neutral OAuth2 contract must not require the verification backend")
endif()

add_executable(forge_package_auth_oauth2_component main.cpp)
target_link_libraries(forge_package_auth_oauth2_component PRIVATE Forge::forge_auth_oauth2)
