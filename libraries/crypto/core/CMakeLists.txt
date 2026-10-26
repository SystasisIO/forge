add_library(
   forge_crypto_core STATIC
   constant_time.cpp
   random.cpp
   secret_bytes.cpp
   secret_string.cpp
)
forge_target_modules_at(forge_crypto_core crypto/core)

target_include_directories(forge_crypto_core PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_compile_definitions(
   forge_crypto_core
   PUBLIC OPENSSL_API_COMPAT=0x30000000L
          OPENSSL_NO_DEPRECATED
)
target_link_libraries(
   forge_crypto_core
   PUBLIC forge_exceptions
   PRIVATE OpenSSL::Crypto
)

forge_target_contract_guest_component(forge_crypto_core ID forge.crypto.core)
