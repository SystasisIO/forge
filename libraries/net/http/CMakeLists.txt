add_library(forge_net_http_values STATIC method.cpp)
target_sources(forge_net_http_values PUBLIC
   FILE_SET CXX_MODULES
   BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
   FILES "${CMAKE_CURRENT_SOURCE_DIR}/include/forge/net/http/method.cppm"
)
target_compile_features(forge_net_http_values PUBLIC cxx_std_23)
target_include_directories(forge_net_http_values PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_net_http_values PUBLIC forge_raw)
forge_target_contract_guest_component(forge_net_http_values ID forge.net.http_values)

if(FORGE_CONTRACT_SDK_HOST_PROFILE)
   return()
endif()

add_library(
   forge_net_http STATIC
   types.cpp
   cookie.cpp
   base_url.cpp
   target.cpp
   range.cpp
   upload.cpp
   route_context.cpp
   middleware.cpp
   connection.cpp
   client.cpp
   stream.cpp
   router.cpp
   router_server_access.cpp
   stream_server_access.cpp
   file.cpp
   assets.cpp
   server.cpp
   server_impl.cpp
   server_session.cpp
   server_session_model.cpp
   beast_body_reader_source.cpp
   server_log.cpp
   request_read_ownership.cpp
)
set(FORGE_net_http_DISABLED_MODULES method)
forge_target_modules_at(forge_net_http net/http)
unset(FORGE_net_http_DISABLED_MODULES)

target_include_directories(forge_net_http PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_net_http PUBLIC
      forge_net_http_values
      forge_exceptions
      forge_api_core
      forge_asio
      forge_codec_json
      forge_crypto_digest
      forge_schema
      forge_net_tls
      forge_net_websocket
      Boost::asio
      Boost::beast
      Boost::url
   PRIVATE
      forge_log
)
