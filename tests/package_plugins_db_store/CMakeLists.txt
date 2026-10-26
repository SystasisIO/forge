cmake_minimum_required(VERSION 3.31)

project(forge-package-plugins-db-store LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS plugins_db_store db_object)

add_executable(forge_package_plugins_db_store api_contracts.cpp catalog.cpp typed_operations.cpp)
target_sources(forge_package_plugins_db_store PRIVATE
   FILE_SET CXX_MODULES FILES model.cppm catalog.cppm)
target_link_libraries(forge_package_plugins_db_store PRIVATE
   Forge::forge_plugins_db_store Forge::forge_db_object)

if(Forge_BUILT_WITH_MDBX)
   find_package(Forge CONFIG REQUIRED COMPONENTS db_mdbx asio app config_core api_core db_core)
   target_sources(forge_package_plugins_db_store PRIVATE main.cpp)
   target_link_libraries(forge_package_plugins_db_store PRIVATE
      Forge::forge_db_mdbx Forge::forge_asio Forge::forge_app Forge::forge_config_core
      Forge::forge_api_core Forge::forge_db_core)
   message(STATUS "Native registration runtime gate: real installed MDBX")
else()
   target_sources(forge_package_plugins_db_store PRIVATE main_generic.cpp)
   message(STATUS "Native registration runtime NOT_RUN: installed Forge was built without MDBX")
endif()
