add_library(forge_auth_pairing STATIC pairing.cpp)
forge_target_modules_at(forge_auth_pairing auth/pairing)

target_include_directories(forge_auth_pairing PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_auth_pairing PUBLIC
      forge_auth_credential
      forge_codec_base64
      forge_crypto_core
      forge_crypto_digest
      forge_exceptions
      forge_raw
)
