add_library(forge_auth_api STATIC)
forge_target_modules_at(forge_auth_api auth/api)

target_include_directories(forge_auth_api PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_auth_api PUBLIC
   Boost::asio
   Boost::headers
   forge_api_auth
   forge_api_core
   forge_auth_attestation
   forge_exceptions
   forge_raw
   forge_variant
)
