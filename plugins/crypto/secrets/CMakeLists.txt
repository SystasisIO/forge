add_library(
   forge_plugins_crypto_secrets STATIC
      plugin.cpp
      api_impl.cpp
      config.cpp
      plugin_impl.cpp
      plugin_impl_versions.cpp
      version_store.cpp
      version_store_operations.cpp
      version_store_rules.cpp
      version_payload.cpp
      secret_operations.cpp
      version_api_impl.cpp
      version_write_api_impl.cpp
      version_metadata_api_impl.cpp
      details/loaded_secret.hxx
)
forge_target_modules_at(forge_plugins_crypto_secrets plugins/crypto/secrets)

target_include_directories(
   forge_plugins_crypto_secrets
   PUBLIC
      ${FORGE_PUBLIC_INCLUDE_DIRS}
      "$<BUILD_INTERFACE:${FORGE_PLUGINS_CRYPTO_SECRETS_INCLUDE_DIR}>"
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
   forge_plugins_crypto_secrets PUBLIC
      forge_app
      forge_api_core
      forge_config_core
      forge_schema
      forge_crypto_core
      forge_exceptions
      Boost::asio
   PRIVATE
      forge_codec_base64
      forge_codec_hex
      forge_crypto_keystore
      forge_crypto_symmetric
      forge_crypto_digest
      forge_raw
      forge_asio
)
