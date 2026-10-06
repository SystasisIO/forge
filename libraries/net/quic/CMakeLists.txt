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
   quic_engine_stream.cpp
   quic_engine_connection.cpp
   quic_engine_connector.cpp
   quic_engine_listener.cpp
   quic_engine_support.cpp
   engine_connection_impl.cpp
   engine_connection_metrics_state.cpp
   engine_connector_impl.cpp
   engine_listener_impl.cpp
   listener_shutdown.cpp
   details/listener_shutdown.hxx
   listener_callback.cpp
   details/listener_callback.hxx
   tls_handles.cpp
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
   connector_impl.cpp
   listener.cpp
   listener_impl.cpp
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
