add_library(
   forge_auth_liboauth2 STATIC
   verifier.cpp
   verifier_impl.cpp
   verifier_impl_claims.cpp
   vendor_runtime.cpp
)
forge_target_modules_at(forge_auth_liboauth2 auth/liboauth2)

target_include_directories(forge_auth_liboauth2 PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_auth_liboauth2
   PUBLIC forge_auth_oauth2 forge_asio
   PRIVATE oauth2-internal Boost::url
)
