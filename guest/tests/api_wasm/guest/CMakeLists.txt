cmake_minimum_required(VERSION 3.31)
project(ForgeApiWasmGuest LANGUAGES CXX)
find_package(ForgeContract CONFIG REQUIRED)
add_subdirectory(../protocol protocol)
forge_add_contract(wasmproto SOURCES contract.cpp LIBRARIES api_wasm_fixture_protocol
   ABI_METADATA ABI_ROOT_TYPES
      forge::api::wasm::step forge::api::wasm::event forge::api::wasm::context
      forge::api::wasm::http_command forge::api::wasm::http_response
      fixture::request fixture::access_result)
