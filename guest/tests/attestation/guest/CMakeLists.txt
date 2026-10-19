cmake_minimum_required(VERSION 3.31)
project(ForgeAttestationGuest LANGUAGES CXX)
find_package(ForgeContract CONFIG REQUIRED)
add_subdirectory(../protocol protocol)
forge_add_contract(granttest SOURCES contract.cpp LIBRARIES attestation_fixture_protocol
   ABI_METADATA ABI_ROOT_TYPES fixture::grant)
