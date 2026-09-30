add_subdirectory(chain/signer)
add_subdirectory(crypto/secrets)
add_subdirectory(crypto/wallet)
add_subdirectory(db/store)
add_subdirectory(net)
add_subdirectory(log/otlp)

add_library(forge_plugins INTERFACE)

target_include_directories(forge_plugins INTERFACE ${FORGE_PUBLIC_INCLUDE_DIRS})

target_link_libraries(
   forge_plugins INTERFACE
      forge_plugins_chain_signer
      forge_plugins_crypto_secrets
      forge_plugins_crypto_wallet
      forge_plugins_net_p2p_node
      forge_plugins_net_p2p_resolver
      forge_plugins_net_p2p_diagnostics
      forge_plugins_net_p2p_pubsub
      forge_plugins_net_http_server
      forge_plugins_log_otlp
      forge_plugins_db_store
)
