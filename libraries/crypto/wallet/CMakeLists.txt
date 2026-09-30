add_library(forge_crypto_wallet STATIC protocol.cpp)
forge_target_modules_at(forge_crypto_wallet crypto/wallet)
target_include_directories(forge_crypto_wallet PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS})
target_link_libraries(forge_crypto_wallet PUBLIC Boost::asio forge_api_auth forge_api_core forge_api_http
                     forge_crypto_digest forge_exceptions forge_raw forge_schema forge_variant)
