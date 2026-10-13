cmake_minimum_required(VERSION 3.31)
project(forge-package-auth-liboauth2-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS auth_liboauth2)

add_executable(forge_package_auth_liboauth2_component main.cpp)
target_link_libraries(forge_package_auth_liboauth2_component PRIVATE Forge::forge_auth_liboauth2)
