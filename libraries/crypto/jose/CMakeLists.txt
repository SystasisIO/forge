add_library(forge_crypto_jose STATIC codec.cpp header_policy.cpp trusted_keys.cpp)
forge_target_modules_at(forge_crypto_jose crypto/jose)

target_include_directories(forge_crypto_jose PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(
   forge_crypto_jose
   PUBLIC forge_crypto_asymmetric forge_crypto_digest forge_exceptions
   PRIVATE forge_codec_base64 forge_codec_hex forge_codec_json forge_variant cjose::cjose_static
)
