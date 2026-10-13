add_library(forge_auth_session STATIC session.cpp)
forge_target_modules_at(forge_auth_session auth/session)

target_include_directories(forge_auth_session PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_auth_session PUBLIC
      forge_auth_credential
      forge_codec_base64
      forge_crypto_core
      forge_crypto_digest
      forge_exceptions
      forge_raw
)
