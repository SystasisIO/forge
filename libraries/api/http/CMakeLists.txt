add_library(
   forge_api_http STATIC
   binding.cpp
   body_stream_endpoint.cpp
   endpoint_body_source.cpp
   invocation_body_source.cpp
   openapi.cpp
   server_stream_state.cpp
   stream_frame_decoder.cpp
)
forge_target_modules_at(forge_api_http api/http)

target_include_directories(forge_api_http PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_api_http PUBLIC
      forge_api_auth
      forge_api_core
      forge_asio
      forge_net_http
      forge_net_transport
      forge_codec_json
      forge_codec_xml
      forge_exceptions
      forge_raw
      forge_reflect
      forge_schema
      Boost::asio
)
