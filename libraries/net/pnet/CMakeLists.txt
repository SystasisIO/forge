add_library(
   forge_net_pnet STATIC
   protector.cpp
   protected_stream.cpp
   details/protected_stream.hxx
)
target_sources(
   forge_net_pnet PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
         include/forge/net/pnet/network_fingerprint.cppm
         include/forge/net/pnet/protector.cppm
)
target_compile_features(forge_net_pnet PUBLIC cxx_std_23)

target_include_directories(forge_net_pnet PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_net_pnet PUBLIC
      forge_exceptions
      forge_codec_hex
      forge_crypto_core
      forge_crypto_digest
      forge_crypto_symmetric
      forge_asio
      forge_net_transport
      Boost::asio
)
