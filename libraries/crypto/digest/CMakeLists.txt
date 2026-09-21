add_library(
   forge_crypto_digest STATIC
   blake2.cpp
   ripemd160.cpp
   sha1.cpp
   sha224.cpp
   sha256.cpp
   sha3.cpp
   sha512.cpp
   shake128.cpp
)
target_sources(
   forge_crypto_digest PUBLIC
      FILE_SET CXX_MODULES
      BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/include"
      FILES
         include/forge/crypto/digest/blake2.cppm
         include/forge/crypto/digest/digest.cppm
         include/forge/crypto/digest/hmac.cppm
         include/forge/crypto/digest/packhash.cppm
         include/forge/crypto/digest/ripemd160.cppm
         include/forge/crypto/digest/ripemd160_value.cppm
         include/forge/crypto/digest/sha1.cppm
         include/forge/crypto/digest/sha224.cppm
         include/forge/crypto/digest/sha256.cppm
         include/forge/crypto/digest/sha256_value.cppm
         include/forge/crypto/digest/sha3.cppm
         include/forge/crypto/digest/sha512.cppm
         include/forge/crypto/digest/sha512_value.cppm
         include/forge/crypto/digest/shake128.cppm
)
target_compile_features(forge_crypto_digest PUBLIC cxx_std_23)

target_include_directories(forge_crypto_digest PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_compile_definitions(
   forge_crypto_digest
   PUBLIC OPENSSL_API_COMPAT=0x30000000L
          OPENSSL_NO_DEPRECATED
)
target_link_libraries(
   forge_crypto_digest
   PUBLIC
      forge_codec_hex
      forge_core
      forge_crypto_core
      forge_exceptions
      forge_raw
      forge_variant
      Boost::headers
   PRIVATE OpenSSL::Crypto
)
forge_target_contract_guest_component(forge_crypto_digest ID forge.crypto.digest)
