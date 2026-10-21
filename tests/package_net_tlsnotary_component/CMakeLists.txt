cmake_minimum_required(VERSION 3.31)

project(forge-package-net-tlsnotary LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(FORGE_TLSNOTARY_TEST_PLUGIN "Exercise the installed session plugin in addition to the native leaf" ON)
set(_forge_tlsnotary_components asio net_tlsnotary)
if(FORGE_TLSNOTARY_TEST_PLUGIN)
   list(APPEND _forge_tlsnotary_components plugins_net_tlsnotary_session)
endif()
find_package(Forge CONFIG REQUIRED COMPONENTS ${_forge_tlsnotary_components})
if(NOT FORGE_TLSNOTARY_TEST_PLUGIN)
   if(NOT TARGET Forge::forge_log)
      message(FATAL_ERROR "Native TLSNotary leaf must load its log dependency")
   endif()
endif()

add_executable(forge_package_net_tlsnotary_component main.cpp)
target_link_libraries(
   forge_package_net_tlsnotary_component PRIVATE
      Forge::forge_asio
      Forge::forge_net_tlsnotary
)
target_compile_definitions(forge_package_net_tlsnotary_component PRIVATE
   FORGE_TLSNOTARY_TEST_PLUGIN=$<BOOL:${FORGE_TLSNOTARY_TEST_PLUGIN}>)
if(FORGE_TLSNOTARY_TEST_PLUGIN)
   target_link_libraries(forge_package_net_tlsnotary_component PRIVATE Forge::forge_plugins_net_tlsnotary_session)
endif()
