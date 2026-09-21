add_library(
   forge_crypto_symmetric STATIC
   aes.cpp
   chacha20_poly1305.cpp
   counter_state.cpp
   kdf.cpp
   salsa20.cpp
   stream_impl.cpp
   xsalsa20.cpp
)
target_sources(
   forge_crypto_symmetric PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
         include/forge/crypto/symmetric/aes.cppm
         include/forge/crypto/symmetric/chacha20_poly1305.cppm
         include/forge/crypto/symmetric/kdf.cppm
         include/forge/crypto/symmetric/salsa20.cppm
         include/forge/crypto/symmetric/xsalsa20.cppm
)
target_compile_features(forge_crypto_symmetric PUBLIC cxx_std_23)
target_sources(
   forge_crypto_symmetric PRIVATE
      $<TARGET_OBJECTS:forge_crypto_symmetric_xsalsa20_vendor>
)

target_include_directories(forge_crypto_symmetric PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_compile_definitions(
   forge_crypto_symmetric
   PUBLIC OPENSSL_API_COMPAT=0x30000000L
          OPENSSL_NO_DEPRECATED
)
target_link_libraries(
   forge_crypto_symmetric
   PUBLIC
      forge_crypto_core
      forge_exceptions
   PRIVATE OpenSSL::Crypto
)
