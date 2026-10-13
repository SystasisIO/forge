add_library(
   forge_api_websocket STATIC
   binding.cpp
   connection.cpp
   stream.cpp
   websocket_stream.cpp
)
forge_target_modules_at(forge_api_websocket api/websocket)

target_include_directories(forge_api_websocket PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_api_websocket PUBLIC
      forge_exceptions
      forge_api_core
      forge_api_auth
      forge_api_stream
      forge_api_transport
      forge_net_websocket
      forge_net_http
      forge_net_transport
      Boost::asio
)
