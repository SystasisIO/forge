if(NOT TARGET OIDCAgent::client)
   message(STATUS "Skipping forge_auth_oidc_agent: coordinator must provide OIDCAgent::client pinned to 5.3.8-systasis.1")
   return()
endif()

if(NOT DEFINED OIDCAgent_VERSION OR NOT OIDCAgent_VERSION STREQUAL "5.3.8-systasis.1")
   message(FATAL_ERROR "forge_auth_oidc_agent requires OIDCAgent_VERSION 5.3.8-systasis.1")
endif()

add_library(forge_auth_oidc_agent STATIC client.cpp client_impl.cpp)
forge_target_modules_at(forge_auth_oidc_agent auth/oidc_agent)

target_include_directories(forge_auth_oidc_agent PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_auth_oidc_agent PUBLIC
      Boost::headers
      forge_asio
      forge_auth_oauth2
      forge_crypto_core
      forge_exceptions
   PRIVATE
      OIDCAgent::client
)
