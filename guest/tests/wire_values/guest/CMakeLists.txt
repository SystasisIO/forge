cmake_minimum_required(VERSION 3.31)
project(ForgeWireValuesGuest LANGUAGES CXX)
find_package(ForgeContract CONFIG REQUIRED)
add_subdirectory(../protocol protocol)
forge_add_contract(wirevalues SOURCES contract.cpp LIBRARIES wire_values_protocol
   ABI_METADATA ABI_ROOT_TYPES fixture::packet forge::api::core::error_payload)
