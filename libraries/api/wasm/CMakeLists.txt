add_library(forge_api_wasm STATIC validation.cpp)
target_sources(forge_api_wasm PUBLIC
   FILE_SET CXX_MODULES
   BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
   FILES
      include/forge/api/wasm/capability.cppm
      include/forge/api/wasm/codec.cppm
      include/forge/api/wasm/context.cppm
      include/forge/api/wasm/continuation.cppm
      include/forge/api/wasm/effect.cppm
      include/forge/api/wasm/event.cppm
      include/forge/api/wasm/exceptions.cppm
      include/forge/api/wasm/http_command.cppm
      include/forge/api/wasm/http_stream.cppm
      include/forge/api/wasm/limits.cppm
      include/forge/api/wasm/payload.cppm
      include/forge/api/wasm/step.cppm
      include/forge/api/wasm/validation.cppm
)
target_compile_features(forge_api_wasm PUBLIC cxx_std_23)

target_include_directories(forge_api_wasm PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_api_wasm PUBLIC
   forge_api_core_values
   forge_net_http_values
   forge_raw
   forge_exceptions
   Boost::headers
)
forge_target_contract_guest_component(forge_api_wasm ID forge.api.wasm)
