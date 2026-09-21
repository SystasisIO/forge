add_library(
   forge_net_quic STATIC
   endpoint.cpp
   security.cpp
   options.cpp
   libp2p.cpp
   framed_stream.cpp
   runtime.cpp
   acknowledged_ranges.cpp
   details/acknowledged_ranges.hxx
   initial_token.cpp
   details/initial_token.hxx
   client_token_cache.cpp
   details/client_token_cache.hxx
   quic_engine.cpp
   server_udp_socket.cpp
   details/server_udp_socket.hxx
   details/quic_engine.hxx
   details/engine_client_options.hxx
   details/engine_server_options.hxx
   details/wrapper_handles.hxx
   stream.cpp
   connection.cpp
   transport.cpp
   connector.cpp
   listener.cpp
)
forge_target_modules_at(forge_net_quic net/quic)

target_include_directories(forge_net_quic PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_net_quic PUBLIC
      forge_exceptions
      forge_crypto_core
      forge_crypto_digest
      forge_crypto_pki
      forge_asio
      forge_net_transport
      Boost::asio
      OpenSSL::SSL
      OpenSSL::Crypto
      Ngtcp2::ngtcp2
      Ngtcp2::crypto_ossl
)
