add_library(
   forge_plugins_net_p2p_node STATIC
      descriptor.cpp
      plugin.cpp
      api_impl.cpp
      dht_api_impl.cpp
      api_publication_generation.cpp
      api_publication_slot.cpp
      api_publication_registry.cpp
      plugin_diagnostics_source_adapter.cpp
      plugin_host_event_source_adapter.cpp
      plugin_pubsub_source_adapter.cpp
      config.cpp
      plugin_impl.cpp
      object_dht_record_store_adapter.cpp
      object_peer_state_adapter.cpp
      p2p_state_schema.cpp
      details/lifecycle_phase.hxx
      details/dht_api_impl.hxx
      details/object_dht_record_store_adapter.hxx
      details/object_peer_state_adapter.hxx
      details/p2p_state_schema.hxx
      details/plugin_diagnostics_source_adapter.hxx
      details/plugin_host_event_source_adapter.hxx
      details/plugin_pubsub_source_adapter.hxx
      details/api_publication_generation.hxx
      details/api_publication_slot.hxx
      details/api_publication_registry.hxx
)
forge_target_modules_at(forge_plugins_net_p2p_node plugins/net/p2p/node)

target_include_directories(
   forge_plugins_net_p2p_node
   PUBLIC
      ${FORGE_PUBLIC_INCLUDE_DIRS}
      "$<BUILD_INTERFACE:${FORGE_PLUGINS_NET_P2P_NODE_INCLUDE_DIR}>"
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(
   forge_plugins_net_p2p_node PUBLIC
      forge_app
      forge_api_core
      forge_api_transport
      forge_api_p2p
      forge_net_p2p
      forge_config_core
      forge_schema
      forge_asio
      forge_exceptions
      Boost::asio
      forge_plugins_db_store
      forge_plugins_crypto_secrets
   PRIVATE
      forge_api_stream
)
