cmake_minimum_required(VERSION 3.31)
project(forge-package-app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS app)
add_executable(forge_package_app main.cpp)
target_link_libraries(forge_package_app PRIVATE Forge::forge_app Forge::forge_log)
