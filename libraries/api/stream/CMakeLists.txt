add_library(
   forge_api_stream STATIC
   session.cpp
   session_impl.cpp
   session_impl_call.cpp
   session_impl_reader.cpp
   session_impl_writer.cpp
   server.cpp
)
forge_target_modules_at(forge_api_stream api/stream)

target_include_directories(forge_api_stream PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_api_stream PUBLIC
      forge_api_core
      forge_api_core_values
      forge_asio
      forge_raw
      forge_net_transport
      Boost::asio
)
