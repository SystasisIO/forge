add_library(
   forge_net_transport STATIC
   buffer.cpp
   bounded_frame_buffer.cpp
   connector.cpp
   datagram_io.cpp
   endpoint.cpp
   frame.cpp
   listener.cpp
   registry.cpp
   stream.cpp
   session.cpp
)
forge_target_modules_at(forge_net_transport net/transport)

target_include_directories(forge_net_transport PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_net_transport PUBLIC
      forge_exceptions
      Boost::asio
   PRIVATE
      forge_asio
)
