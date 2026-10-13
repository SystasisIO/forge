add_library(forge_auth_http STATIC policy.cpp)
forge_target_modules_at(forge_auth_http auth/http)

target_include_directories(forge_auth_http PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_auth_http PUBLIC
      forge_auth_credential
      forge_exceptions
      forge_crypto_core
      forge_auth_session
      forge_net_http
   PRIVATE
      Boost::url
)
