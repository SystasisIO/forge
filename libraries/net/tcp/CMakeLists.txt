add_library(
   forge_net_tcp STATIC
   connection.cpp
   connection_access.cpp
   connection_impl.cpp
   connector.cpp
   connector_access.cpp
   connector_impl.cpp
   listener.cpp
   listener_impl.cpp
   socket_reuse.cpp
   socket_stream.cpp
   transport.cpp
)
forge_target_modules_at(forge_net_tcp net/tcp)

target_include_directories(forge_net_tcp PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_net_tcp PUBLIC
      forge_exceptions
      forge_net_transport
      Boost::asio
      forge_asio
)
