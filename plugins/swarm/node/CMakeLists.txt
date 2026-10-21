add_library(forge_plugins_swarm_node STATIC
   plugin.cpp
   plugin_impl.cpp
   api_impl.cpp
   config.cpp
   exceptions.cpp
   catalog_access.cpp
   provider_key.cpp
   peer_capabilities.cpp
   peer_route.cpp
   peer_connection.cpp
   provider_registration.cpp
)
forge_target_modules_at(forge_plugins_swarm_node plugins/swarm/node)
target_include_directories(forge_plugins_swarm_node PUBLIC
   ${FORGE_PUBLIC_INCLUDE_DIRS}
   "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
   PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(forge_plugins_swarm_node PUBLIC
   forge_app forge_api_core forge_swarm_runtime forge_swarm_storage forge_net_p2p
   forge_multiformats forge_asio forge_config_core forge_schema forge_exceptions Boost::asio
   PRIVATE forge_plugins_net_p2p_node forge_plugins_db_store forge_api_p2p
   forge_api_transport forge_crypto_digest forge_raw
)
