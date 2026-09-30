add_library(forge_plugins_crypto_wallet STATIC plugin.cpp plugin_impl.cpp api_impl.cpp provider.cpp provider_impl.cpp
   provider_source_impl.cpp)
forge_target_modules_at(forge_plugins_crypto_wallet plugins/crypto/wallet)
target_include_directories(forge_plugins_crypto_wallet PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS}
                           PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(forge_plugins_crypto_wallet PUBLIC forge_app forge_api_auth forge_api_core forge_asio
   forge_config_core forge_crypto_core forge_crypto_wallet forge_crypto_keystore forge_crypto_signer forge_crypto_asymmetric
   forge_exceptions forge_schema forge_raw forge_plugins_net_http_server Boost::asio)
