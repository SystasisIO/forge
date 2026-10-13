add_library(
   forge_auth_oauth2 STATIC
   access_token.cpp
   interactive_authorizer.cpp
   token_provider.cpp
   token_verifier.cpp
   verification_policy.cpp
   verified_token.cpp
)
forge_target_modules_at(forge_auth_oauth2 auth/oauth2)

target_include_directories(forge_auth_oauth2 PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_auth_oauth2 PUBLIC
      Boost::headers
      forge_crypto_core
      forge_exceptions
)
