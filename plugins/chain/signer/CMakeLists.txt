add_library(
   forge_plugins_chain_signer STATIC
   descriptor.cpp
   admission.cpp
   config.cpp
   plugin.cpp
   plugin_impl.cpp
   signing_policy.cpp
   transaction_api_impl.cpp
   block_api_impl.cpp
   finality_api_impl.cpp
)
forge_target_modules_at(forge_plugins_chain_signer plugins/chain/signer)

target_include_directories(
   forge_plugins_chain_signer
   PUBLIC ${FORGE_PUBLIC_INCLUDE_DIRS}
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
   forge_plugins_chain_signer PUBLIC
      Boost::asio
      forge_api_auth
      forge_api_core
      forge_app
      forge_asio
      forge_chain_api
      forge_chain_savanna
      forge_chain_transaction
      forge_config_core
      forge_crypto_bls
      forge_crypto_bls_signer
      forge_crypto_signer
      forge_exceptions
      forge_raw
      forge_schema
)
