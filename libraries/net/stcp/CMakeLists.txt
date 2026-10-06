add_library(
   forge_net_stcp STATIC
   connection.cpp
   connection_access.cpp
   connection_impl.cpp
   connector.cpp
   details/handshake_deadline.hxx
   details/stream_backend.hxx
   details/transport_stream_adapter.hxx
   io_gates.cpp
   listener.cpp
   stream_backend.cpp
   stream_model.cpp
   transport_stream_adapter.cpp
)
forge_target_modules_at(forge_net_stcp net/stcp)

target_include_directories(forge_net_stcp PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_net_stcp PUBLIC
      forge_exceptions
      forge_net_transport
      forge_net_tcp
      forge_net_tls
      Boost::asio
   PRIVATE
      forge_asio
)
