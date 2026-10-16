cmake_minimum_required(VERSION 3.31)

project(forge-package-api-stream-component LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Forge CONFIG REQUIRED COMPONENTS api_stream)

foreach(property IN ITEMS INTERFACE_LINK_LIBRARIES IMPORTED_CXX_MODULES_LINK_LIBRARIES)
   get_target_property(stream_dependencies Forge::forge_api_stream "${property}")
   if(NOT "Forge::forge_api_core_values" IN_LIST stream_dependencies)
      message(FATAL_ERROR
         "api_stream must directly provide api_core_values through ${property}")
   endif()
endforeach()

add_executable(forge_package_api_stream_component main.cpp)
target_link_libraries(forge_package_api_stream_component PRIVATE Forge::forge_api_stream)
