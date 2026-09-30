add_library(
   forge_crypto_keystore STATIC
   encrypted_file.cpp
   password.cpp
   ownership.cpp
   ownership_impl.cpp
   store.cpp
   store_impl.cpp
)
forge_target_modules_at(forge_crypto_keystore crypto/keystore)

target_include_directories(forge_crypto_keystore PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_include_directories(forge_crypto_keystore PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(
   forge_crypto_keystore PUBLIC
      Boost::headers
      forge_crypto_core
      forge_crypto_signer
      forge_crypto_symmetric
      forge_codec_hex
      forge_exceptions
      forge_raw
)
