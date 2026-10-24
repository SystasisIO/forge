cmake_minimum_required(VERSION 3.31)
project(ModuleEntry LANGUAGES CXX)
find_package(ForgeContract CONFIG REQUIRED)

forge_add_contract_library(
   probe_contract
   ID probe.contract
   MODULE_BASE_DIRS include
   MODULE_SOURCES include/probe/contract.cppm
   SOURCES contract.cpp
   PUBLIC_LIBRARIES Forge::forge_contract_runtime
)

set(ENTRY_MODE "module" CACHE STRING "Module entry fixture mode")
set(_legacy "${CMAKE_CURRENT_BINARY_DIR}/legacy.cpp")
file(CONFIGURE OUTPUT "${_legacy}" CONTENT "import probe.contract;\n" @ONLY)
set_source_files_properties("${_legacy}" PROPERTIES GENERATED TRUE)

if(ENTRY_MODE STREQUAL "module")
   forge_add_contract(moduleprobe DISPATCH_MODULE probe.contract LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "legacy")
   forge_add_contract(moduleprobe SOURCES "${_legacy}" LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "invalid")
   forge_add_contract(moduleprobe DISPATCH_MODULE probe:partition LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "empty")
   forge_add_contract(moduleprobe DISPATCH_MODULE LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "sources")
   forge_add_contract(moduleprobe DISPATCH_MODULE probe.contract SOURCES "${_legacy}" LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "dispatch")
   forge_add_contract(moduleprobe DISPATCH_MODULE probe.contract DISPATCH_SOURCE "${_legacy}" LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "unknown")
   forge_add_contract(moduleprobe DISPATCH_MODULE probe.missing LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "outside")
   forge_add_contract(../escaped DISPATCH_MODULE probe.contract LIBRARIES probe_contract)
elseif(ENTRY_MODE STREQUAL "nested")
   forge_add_contract(link/new DISPATCH_MODULE probe.contract LIBRARIES probe_contract)
else()
   message(FATAL_ERROR "Unknown module entry fixture mode")
endif()
