cmake_minimum_required(VERSION 3.31)
project(ForgeApiWasmGuest LANGUAGES CXX)
find_package(ForgeContract CONFIG REQUIRED)
add_subdirectory(../protocol protocol)
forge_add_contract(wasmproto SOURCES contract.cpp LIBRARIES api_wasm_fixture_protocol
   ABI_METADATA ABI_ROOT_TYPES
      forge::api::wasm::step forge::api::wasm::event forge::api::wasm::context
      forge::api::wasm::http_command forge::api::wasm::http_response
      forge::api::wasm::http_stream_open forge::api::wasm::http_stream_read forge::api::wasm::http_stream_event
      fixture::request fixture::access_result fixture::json_request
      fixture::stream_request fixture::stream_item fixture::stream_state fixture::stream_summary)
