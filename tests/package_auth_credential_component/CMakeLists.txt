cmake_minimum_required(VERSION 3.31)

project(forge-package-auth-credential-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS auth_credential)

get_target_property(credential_links Forge::forge_auth_credential INTERFACE_LINK_LIBRARIES)
if("${credential_links}" MATCHES "forge_auth_(pairing|session|oauth2)")
   message(FATAL_ERROR "Neutral credentials must not depend on pairing, sessions or OAuth clients")
endif()

add_executable(forge_package_auth_credential_component main.cpp)
target_link_libraries(forge_package_auth_credential_component PRIVATE Forge::forge_auth_credential)
